#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "Placement.hpp"

// Bound a malformed preview array without changing the game's valid geometry.
static constexpr std::int32_t maximum_placement_vertices = 65536;
// New keys past this bound are evaluated without memoization.
static constexpr std::size_t maximum_placement_segment_verdicts = 4096;

struct PlacementQuery {
  void *cache{};
  std::uint32_t actor_index{};
  bool skip{};
};

static thread_local PlacementQuery *active_placement_query{};
static thread_local void *initial_point_manor{};

static auto read_placement_memory(std::uintptr_t address, void *destination,
                                  std::size_t size) noexcept -> bool {
  SIZE_T copied{};
  return address != 0 &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), destination,
                           size, &copied) != 0 &&
         copied == size;
}

auto resolve_placement_manor(void *cache, void *pawn) noexcept -> void * {
  if (cache == nullptr || pawn == nullptr) {
    return nullptr;
  }
  const auto address = reinterpret_cast<std::uintptr_t>(pawn);
  std::uintptr_t castle_parent{};
  if (!read_placement_memory(address + 0x840, &castle_parent,
                             sizeof(castle_parent)) ||
      castle_parent != 0) {
    return nullptr;
  }
  // Ordinary building preview uses +0x978; polygon plot preview uses +0xf68.
  for (const auto offset : {0x978, 0xf68}) {
    std::uintptr_t region{};
    std::uintptr_t region_cache{};
    std::uintptr_t manor{};
    std::int32_t building_type{};
    if (!read_placement_memory(address + offset, &region, sizeof(region)) ||
        region == 0 ||
        !read_placement_memory(region + 0x738, &region_cache,
                               sizeof(region_cache)) ||
        region_cache != reinterpret_cast<std::uintptr_t>(cache) ||
        !read_placement_memory(region + 0x358, &manor, sizeof(manor)) ||
        manor == 0 ||
        !read_placement_memory(manor + 0x3b8, &building_type,
                               sizeof(building_type)) ||
        building_type != 98) {
      continue;
    }
    return reinterpret_cast<void *>(manor);
  }
  return nullptr;
}

auto placement_skip_record(void *cache, std::uint32_t actor_index) noexcept
    -> bool {
  const auto *query = active_placement_query;
  return query != nullptr && query->skip && query->cache == cache &&
         query->actor_index == actor_index;
}

static bool placement_cache_pending(void *cache) noexcept {
  // The ordinary native cache tick (0x4baff82) permits dependent work only
  // after both sparse obstacle queues have no live entries. A native query
  // against a partially populated cache can otherwise report false clearance.
  const auto address = reinterpret_cast<std::uintptr_t>(cache);
  if (!cache || address > std::numeric_limits<std::uintptr_t>::max() - 0x2b0)
    return true;
  std::array<std::int32_t, 4> counts{};
  const std::array<std::size_t, 4> offsets{0x230, 0x25c, 0x280, 0x2ac};
  for (std::size_t i{}; i < offsets.size(); ++i) {
    if (!read_placement_memory(address + offsets[i], &counts[i],
                               sizeof(counts[i])) ||
        counts[i] < 0)
      return true;
  }
  return counts[0] != counts[1] || counts[2] != counts[3];
}

auto placement_collision(void *cache, NativeArray<Vec3> *triangles,
                         const double *bounds, NativeArray<void *> *ignored,
                         std::uint8_t skip_segments, void *pawn) noexcept
    -> bool {
  const auto image =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  using NativeCollision = bool (*)(void *, NativeArray<Vec3> *, const double *,
                                   NativeArray<void *> *, std::uint8_t);
  const auto original = reinterpret_cast<NativeCollision>(image + 0x4b9d080);
  const bool cache_pending = placement_cache_pending(cache);
  PlacementQuery query{cache};
  try {
    void *manor = resolve_placement_manor(cache, pawn);
    NativeArray<Vec3> input{};
    if (!cache_pending && manor != nullptr &&
        read_placement_memory(reinterpret_cast<std::uintptr_t>(triangles),
                              &input, sizeof(input)) &&
        input.data != nullptr && input.count > 0 && input.count % 3 == 0 &&
        input.count <= input.capacity &&
        input.capacity <= maximum_placement_vertices) {
      std::vector<Vec3> vertices(static_cast<std::size_t>(input.count));
      if (read_placement_memory(reinterpret_cast<std::uintptr_t>(input.data),
                                vertices.data(),
                                vertices.size() * sizeof(Vec3))) {
        bool finite = true;
        for (const auto &vertex : vertices) {
          finite = finite && std::isfinite(vertex.x) &&
                   std::isfinite(vertex.y) && std::isfinite(vertex.z);
        }
        if (finite) {
          const auto geometry = build_manor_geometry(manor, image);
          const auto verdict =
              triangles_intersect_manor_modules(geometry, vertices);
          query.skip = verdict == GeometryVerdict::Clear &&
                       read_placement_memory(
                           reinterpret_cast<std::uintptr_t>(manor) + 0xc,
                           &query.actor_index, sizeof(query.actor_index));
        }
      }
    }
  } catch (...) {
    // Allocation/geometry failures retain the original full-area collision.
    query.skip = false;
  }
  auto *previous = active_placement_query;
  active_placement_query = &query;
  // Do not short-circuit this call: it owns the ignored array's allocation.
  // Overlapping modules keep the Manor in the native query; they must not add
  // a second veto. Native plot boundaries can lie on visual wall centerlines.
  const bool native_blocked =
      original(cache, triangles, bounds, ignored, skip_segments);
  active_placement_query = previous;
  // Physical overlap keeps the Manor in the native query; native clearance
  // still permits the boundary placements accepted by the unmodified game.
  return cache_pending || native_blocked;
}

auto placement_segment_for_manor(void *cache, const double *start,
                                 const double *end, void *manor,
                                 const ManorGeometry *snapshot,
                                 PlacementSegmentMemo *segment_memo) noexcept
    -> bool {
  const auto image =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  using NativeSegment = bool (*)(void *, const double *, const double *);
  const auto original = reinterpret_cast<NativeSegment>(image + 0x4bc73f0);
  PlacementQuery query{cache};
  try {
    Vec3 a{}, b{};
    // This engine query receives FVector2D, not FVector. Do not read a Z value
    // beyond the two input doubles.
    if (manor != nullptr &&
        read_placement_memory(reinterpret_cast<std::uintptr_t>(start), &a,
                              2 * sizeof(double)) &&
        read_placement_memory(reinterpret_cast<std::uintptr_t>(end), &b,
                              2 * sizeof(double)) &&
        std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(b.x) &&
        std::isfinite(b.y)) {
      ManorGeometry owned_geometry;
      if (snapshot == nullptr) {
        owned_geometry = build_manor_geometry(manor, image);
        snapshot = &owned_geometry;
        // Only the caller's immutable query snapshot may own a reusable
        // verdict. A freshly acquired world snapshot never reuses one.
        segment_memo = nullptr;
      }
      // Derive the key from the exact private coordinates evaluated below,
      // never from a separate read of the caller's potentially changing data.
      const std::array key{
          std::bit_cast<std::uint64_t>(a.x), std::bit_cast<std::uint64_t>(a.y),
          std::bit_cast<std::uint64_t>(b.x), std::bit_cast<std::uint64_t>(b.y)};
      std::optional<GeometryVerdict> cached_verdict;
      if (segment_memo != nullptr) {
        const auto found = segment_memo->find(key);
        if (found != segment_memo->end())
          cached_verdict = found->second;
      }
      const auto verdict =
          cached_verdict.has_value()
              ? *cached_verdict
              : segment_intersects_manor_modules(*snapshot, a, b, 0.0);
      if (!cached_verdict.has_value() && segment_memo != nullptr &&
          segment_memo->size() < maximum_placement_segment_verdicts) {
        try {
          segment_memo->try_emplace(key, verdict);
        } catch (...) {
          // Allocation failure must retain the freshly computed verdict.
        }
      }
      query.skip =
          verdict == GeometryVerdict::Clear &&
          read_placement_memory(reinterpret_cast<std::uintptr_t>(manor) + 0xc,
                                &query.actor_index, sizeof(query.actor_index));
    }
  } catch (...) {
    query.skip = false;
  }
  auto *previous = active_placement_query;
  active_placement_query = &query;
  const bool native_blocked = original(cache, start, end);
  active_placement_query = previous;
  return native_blocked;
}

auto placement_segment_collision(void *cache, const double *start,
                                 const double *end, void *pawn) noexcept
    -> bool {
  // UI-only guard: the shared helper is also called by the graph worker and
  // must not reject that worker's candidates merely because updates are queued.
  if (placement_cache_pending(cache))
    return true;
  return placement_segment_for_manor(cache, start, end,
                                     resolve_placement_manor(cache, pawn));
}

auto placement_initial_point(void *game, const Vec3 *point, void *pawn) noexcept
    -> bool {
  const auto image =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  using NativePoint = bool (*)(void *, const Vec3 *);
  const auto original = reinterpret_cast<NativePoint>(image + 0x4cd5b20);
  void *ignored_manor{};
  bool cache_pending{};
  try {
    const auto address = reinterpret_cast<std::uintptr_t>(pawn);
    std::uintptr_t pawn_game{}, region{}, cache{};
    Vec3 position{};
    if (pawn != nullptr &&
        read_placement_memory(address + 0x340, &pawn_game, sizeof(pawn_game)) &&
        pawn_game == reinterpret_cast<std::uintptr_t>(game) &&
        read_placement_memory(address + 0xf68, &region, sizeof(region)) &&
        region != 0 &&
        read_placement_memory(region + 0x738, &cache, sizeof(cache)) &&
        read_placement_memory(reinterpret_cast<std::uintptr_t>(point),
                              &position, sizeof(position)) &&
        std::isfinite(position.x) && std::isfinite(position.y) &&
        std::isfinite(position.z)) {
      void *manor =
          resolve_placement_manor(reinterpret_cast<void *>(cache), pawn);
      cache_pending = placement_cache_pending(reinterpret_cast<void *>(cache));
      if (!cache_pending && manor != nullptr) {
        const auto geometry = build_manor_geometry(manor, image);
        if (geometry.complete && !point_in_manor_modules(geometry, position)) {
          ignored_manor = manor;
        }
      }
    }
  } catch (...) {
    ignored_manor = nullptr;
  }
  void *previous = initial_point_manor;
  initial_point_manor = ignored_manor;
  const bool native_blocked = original(game, point);
  initial_point_manor = previous;
  return cache_pending || native_blocked;
}

auto placement_initial_point_bounds(void *building, const Vec3 *point) noexcept
    -> bool {
  if (initial_point_manor != nullptr && building == initial_point_manor) {
    return false;
  }
  const auto image =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  using NativeBounds = bool (*)(void *, const Vec3 *);
  return reinterpret_cast<NativeBounds>(image + 0x4d47920)(building, point);
}
