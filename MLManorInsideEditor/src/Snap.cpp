#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "Placement.hpp"
#include "Snap.hpp"

// Bound corrupt creator arrays without allocating per graph candidate.
static constexpr std::int32_t maximum_snap_node_creators = 64;

static SnapPathQuery native_snap_path{};
static SnapSegmentQuery native_snap_segment{};
static SnapGraphBuild native_snap_graph{};
static SnapWeakCacheGet native_snap_weak_cache{};
static std::uintptr_t snap_image{};

struct SnapQueryContext {
  void *cache{};
  void *pawn{};
  void *manor{};
  ManorGeometry geometry;
  mutable PlacementSegmentMemo segment_verdicts;
};

static thread_local const SnapQueryContext *active_snap_query{};
static thread_local const SnapQueryContext *active_snap_graph{};

template <typename T>
static bool read_snap_value(std::uintptr_t address, T &value) noexcept {
  SIZE_T copied{};
  return address != 0 &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), &value,
                           sizeof(value), &copied) != 0 &&
         copied == sizeof(value);
}

class ScopedSnapQuery {
  const SnapQueryContext *&slot_;
  const SnapQueryContext *previous_;

public:
  ScopedSnapQuery(const SnapQueryContext &query,
                  const SnapQueryContext *&slot) noexcept
      : slot_(slot), previous_(slot) {
    slot_ = &query;
  }

  ~ScopedSnapQuery() { slot_ = previous_; }

  ScopedSnapQuery(const ScopedSnapQuery &) = delete;
  auto operator=(const ScopedSnapQuery &) -> ScopedSnapQuery & = delete;
};

void configure_snap_query_functions(SnapPathQuery path,
                                    SnapSegmentQuery segment) noexcept {
  native_snap_path = path;
  native_snap_segment = segment;
}

void configure_snap_graph_functions(SnapGraphBuild build,
                                    SnapWeakCacheGet weak_cache) noexcept {
  native_snap_graph = build;
  native_snap_weak_cache = weak_cache;
}

void configure_snap_queries(std::uintptr_t image) noexcept {
  snap_image = image;
  configure_snap_query_functions(
      reinterpret_cast<SnapPathQuery>(image + 0x4bb1390),
      reinterpret_cast<SnapSegmentQuery>(image + 0x4bc73f0));
  configure_snap_graph_functions(
      reinterpret_cast<SnapGraphBuild>(image + 0x4b8abb0),
      reinterpret_cast<SnapWeakCacheGet>(image + 0x1497740));
}

NativeArray<Vec3> *snap_project_path(void *cache, NativeArray<Vec3> *output,
                                     void *plot_points, float threshold,
                                     std::uint8_t mode, void *pawn) {
  SnapQueryContext query{};
  query.cache = cache;
  std::uintptr_t castle_parent{};
  SIZE_T copied{};
  if (mode == 3 && cache != nullptr && pawn != nullptr &&
      std::isfinite(threshold) &&
      ReadProcessMemory(GetCurrentProcess(),
                        static_cast<const std::byte *>(pawn) + 0x840,
                        &castle_parent, sizeof(castle_parent), &copied) != 0 &&
      copied == sizeof(castle_parent) && castle_parent == 0) {
    query.pawn = pawn;
    try {
      query.manor = resolve_placement_manor(cache, pawn);
      if (query.manor != nullptr) {
        query.geometry = build_manor_geometry(query.manor, snap_image);
        if (!query.geometry.complete) {
          query.manor = nullptr;
        }
      }
    } catch (...) {
      query.manor = nullptr;
    }
  }
  // A nested ineligible query intentionally masks an outer eligible query.
  // The RAII scope restores the previous query even if the native call throws.
  const ScopedSnapQuery scope(query, active_snap_query);
  return native_snap_path(cache, output, plot_points, threshold, mode);
}

bool snap_segment_collision(void *cache, const double *start,
                            const double *end) {
  const auto *query = active_snap_query;
  if (query != nullptr && query->manor != nullptr && query->cache == cache) {
    // This existing routine validates the region/Manor, checks every physical
    // component, and calls the native query exactly once with scoped
    // exclusions.
    return placement_segment_for_manor(cache, start, end, query->manor,
                                       &query->geometry,
                                       &query->segment_verdicts);
  }
  return native_snap_segment(cache, start, end);
}

static void *resolve_snap_graph_manor(void *cache) noexcept {
  const auto address = reinterpret_cast<std::uintptr_t>(cache);
  std::uintptr_t region{}, region_cache{}, owner{}, manor{};
  std::uint8_t main_player{};
  std::int32_t building_type{};
  std::uint32_t object_flags{};
  // The grid UObject's Outer is its region. Verify the reverse pointer as well
  // so another cache/owner context can never acquire this exception.
  if (cache != nullptr && read_snap_value(address + 0x20, region) &&
      region != 0 && read_snap_value(region + 0x738, region_cache) &&
      region_cache == address && read_snap_value(region + 0x350, owner) &&
      owner != 0 && read_snap_value(owner + 0x34c, main_player) &&
      main_player == 1 && read_snap_value(region + 0x358, manor) &&
      manor != 0 && read_snap_value(manor + 8, object_flags) &&
      (object_flags & 0x40018000U) == 0 &&
      read_snap_value(manor + 0x3b8, building_type) && building_type == 98) {
    return reinterpret_cast<void *>(manor);
  }
  return nullptr;
}

std::uintptr_t snap_build_graph(void *work) {
  SnapQueryContext query{};
  try {
    std::uint64_t weak_cache{};
    if (work != nullptr &&
        read_snap_value(reinterpret_cast<std::uintptr_t>(work) + 0x18,
                        weak_cache)) {
      query.cache = native_snap_weak_cache(&weak_cache);
      query.manor = resolve_snap_graph_manor(query.cache);
      if (query.manor != nullptr) {
        query.geometry = build_manor_geometry(query.manor, snap_image);
        if (!query.geometry.complete) {
          query.manor = nullptr;
        }
      }
    }
  } catch (...) {
    query.manor = nullptr;
  }
  const ScopedSnapQuery scope(query, active_snap_graph);
  return native_snap_graph(work);
}

static std::optional<bool> snap_node_created_by_manor(const void *node,
                                                      void *manor) {
  const auto address = reinterpret_cast<std::uintptr_t>(node);
  constexpr auto maximum_address = std::numeric_limits<std::uintptr_t>::max();
  NativeArray<std::uint64_t> creators{}, current_creators{};
  // These are value-node pointers. Creator weak refs are at +0x38, while
  // +0x48 holds unrelated road refs. TMap records add another 0x18 prefix.
  if (node == nullptr || native_snap_weak_cache == nullptr ||
      address > maximum_address - 0x38 - sizeof(creators) ||
      !read_snap_value(address + 0x38, creators) || creators.count < 0 ||
      creators.capacity < creators.count ||
      creators.capacity > maximum_snap_node_creators ||
      (creators.capacity != 0 && creators.data == nullptr)) {
    return std::nullopt;
  }

  const auto size =
      static_cast<std::size_t>(creators.count) * sizeof(std::uint64_t);
  const auto data_address = reinterpret_cast<std::uintptr_t>(creators.data);
  if (data_address > maximum_address - size) {
    return std::nullopt;
  }
  std::array<std::uint64_t, maximum_snap_node_creators> weak_creators{};
  SIZE_T copied{};
  if (size != 0 &&
      (ReadProcessMemory(GetCurrentProcess(), creators.data,
                         weak_creators.data(), size, &copied) == 0 ||
       copied != size)) {
    return std::nullopt;
  }

  bool contains_manor{};
  for (std::int32_t index = 0; index < creators.count; ++index) {
    void *creator = native_snap_weak_cache(&weak_creators[index]);
    if (creator == nullptr) {
      return std::nullopt;
    }
    contains_manor = contains_manor || creator == manor;
  }

  std::array<std::uint64_t, maximum_snap_node_creators> current_weak_creators{};
  if (!read_snap_value(address + 0x38, current_creators) ||
      creators.data != current_creators.data ||
      creators.count != current_creators.count ||
      creators.capacity != current_creators.capacity ||
      (size != 0 &&
       (ReadProcessMemory(GetCurrentProcess(), creators.data,
                          current_weak_creators.data(), size, &copied) == 0 ||
        copied != size ||
        std::memcmp(weak_creators.data(), current_weak_creators.data(), size) !=
            0)) ||
      !read_snap_value(address + 0x38, current_creators) ||
      creators.data != current_creators.data ||
      creators.count != current_creators.count ||
      creators.capacity != current_creators.capacity) {
    return std::nullopt;
  }
  return contains_manor;
}

bool snap_graph_segment_collision(void *cache, const double *start,
                                  const double *end, const void *node_a,
                                  const void *node_b) {
  const auto *query = active_snap_graph;
  if (query != nullptr && query->manor != nullptr && query->cache == cache) {
    try {
      const auto first = snap_node_created_by_manor(node_a, query->manor);
      const auto second = snap_node_created_by_manor(node_b, query->manor);
      if (first.has_value() && second.has_value() && !(*first && *second)) {
        return placement_segment_for_manor(cache, start, end, query->manor,
                                           &query->geometry,
                                           &query->segment_verdicts);
      }
      // Native Manor boundary candidates lie on wall centerlines. Preserve
      // the native graph test for these edges, including every other obstacle.
    } catch (...) {
      // Unreadable, stale, or changing creator data never grants an exception.
    }
  }
  return native_snap_segment(cache, start, end);
}
