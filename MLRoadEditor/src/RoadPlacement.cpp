#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

#include <Windows.h>

#include "RoadPlacement.hpp"

static std::uintptr_t road_image{};
static std::atomic<std::uint64_t> normal_path_count{};

struct RoadQueryContext {
  void *cache{};
  void *pawn{};
  void *manor{};
  bool castle{};
  bool geometry_attempted{};
  ManorGeometry geometry;
  RoadQueryContext *previous{};
  static thread_local RoadQueryContext *active;

  RoadQueryContext(void *source, void *player)
      : cache(source), pawn(player), previous(active) {
    active = this;
  }
  ~RoadQueryContext() { active = previous; }
};

thread_local RoadQueryContext *RoadQueryContext::active{};

template <typename T>
static bool read_road_value(std::uintptr_t address, T &value) {
  SIZE_T count{};
  return address &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), &value,
                           sizeof(value), &count) &&
         count == sizeof(value);
}

// Region lookup is the same lookup used by the native road queries. Checking
// both endpoints also supports the second-region query of a crossing road.
static void *road_manor(void *pawn_pointer, void *expected_cache,
                        const Vec3 &point, bool &castle_tool,
                        void *expected_game = nullptr) {
  const auto pawn = reinterpret_cast<std::uintptr_t>(pawn_pointer);
  void *castle{}, *preview{}, *game{};
  std::uint8_t road_mode{}, player{}, alternate_tool{}, removal{}, platform{};
  std::int32_t building_type{};
  if (!road_image || !pawn || !read_road_value(pawn + 0x840, castle) ||
      !read_road_value(pawn + 0x34c, player) || !player ||
      !read_road_value(pawn + 0x598, alternate_tool) || alternate_tool ||
      !read_road_value(pawn + 0x5f8, building_type) || building_type > 0 ||
      !read_road_value(pawn + 0x7a0, road_mode) || !road_mode ||
      !read_road_value(pawn + 0x7a1, removal) || removal ||
      !read_road_value(pawn + 0xc58, platform) || platform ||
      !read_road_value(pawn + 0x778, preview) || !preview ||
      !read_road_value(pawn + 0x340, game) || !game ||
      (expected_game && game != expected_game) || !std::isfinite(point.x) ||
      !std::isfinite(point.y) || !std::isfinite(point.z)) {
    return nullptr;
  }
  const auto preview_address = reinterpret_cast<std::uintptr_t>(preview);
  std::uint8_t road_type{}, wall_type{};
  if (!read_road_value(preview_address + 0x2b4, road_type) || road_type > 1 ||
      !read_road_value(preview_address + 0x2b5, wall_type) || wall_type != 0) {
    return nullptr;
  }
  using FindRegion = void *(*)(void *, const Vec3 *, bool);
  const auto find_region = reinterpret_cast<FindRegion>(road_image + 0x4cc9ab0);
  const auto region =
      reinterpret_cast<std::uintptr_t>(find_region(game, &point, false));
  void *cache{}, *manor{};
  if (!region || !read_road_value(region + 0x738, cache) || !cache ||
      (expected_cache && cache != expected_cache) ||
      !read_road_value(region + 0x358, manor) || !manor) {
    return nullptr;
  }
  const auto manor_address = reinterpret_cast<std::uintptr_t>(manor);
  std::int32_t type{};
  std::uint32_t flags{};
  if (!read_road_value(manor_address + 8, flags) || (flags & 0x40000000U) ||
      !read_road_value(manor_address + 0x3b8, type) || type != 98 ||
      (castle != nullptr && castle != manor)) {
    return nullptr;
  }
  castle_tool = castle != nullptr;
  return manor;
}

void configure_road_placement(std::uintptr_t image) noexcept {
  road_image = image;
  normal_path_count = 0;
}

std::uint64_t road_normal_path_count() noexcept {
  return normal_path_count.load(std::memory_order_relaxed);
}

bool road_use_normal_obstacle_path(void *pawn_pointer) noexcept {
  const auto pawn = reinterpret_cast<std::uintptr_t>(pawn_pointer);
  std::uintptr_t castle{}, preview{}, master{}, owner{}, castle_master{},
      preview_master{};
  std::uint8_t player{}, planning{}, road_mode{}, alternate{}, removal{},
      platform{}, road_type{}, wall_type{};
  std::int32_t building_type{}, castle_type{};
  std::uint32_t pawn_flags{}, castle_flags{}, preview_flags{};
  // RoadTick's planner branch otherwise skips its entire native obstacle
  // detour solver. Route only the player's ordinary CastlePlan road through
  // that solver; leave the current Manor ignore and gate-generation state live.
  if (!road_image || !pawn || !read_road_value(pawn + 8, pawn_flags) ||
      (pawn_flags & 0x40018000U) != 0 ||
      !read_road_value(pawn + 0x34c, player) || player != 1 ||
      !read_road_value(pawn + 0x848, planning) || planning != 1 ||
      !read_road_value(pawn + 0x7a0, road_mode) || road_mode != 1 ||
      !read_road_value(pawn + 0x598, alternate) || alternate != 0 ||
      !read_road_value(pawn + 0x7a1, removal) || removal != 0 ||
      !read_road_value(pawn + 0x5f8, building_type) || building_type > 0 ||
      !read_road_value(pawn + 0xc58, platform) || platform != 0 ||
      !read_road_value(pawn + 0x840, castle) || castle == 0 ||
      !read_road_value(pawn + 0x340, master) || master == 0 ||
      !read_road_value(pawn + 0x778, preview) || preview == 0 ||
      !read_road_value(castle + 8, castle_flags) ||
      (castle_flags & 0x40018000U) != 0 ||
      !read_road_value(castle + 0x3b8, castle_type) || castle_type != 98 ||
      !read_road_value(castle + 0x2e0, owner) || owner != pawn ||
      !read_road_value(castle + 0x2e8, castle_master) ||
      castle_master != master || !read_road_value(preview + 8, preview_flags) ||
      (preview_flags & 0x40018000U) != 0 ||
      !read_road_value(preview + 0x2a8, preview_master) ||
      preview_master != master ||
      !read_road_value(preview + 0x2b4, road_type) || road_type > 1 ||
      !read_road_value(preview + 0x2b5, wall_type) || wall_type != 0) {
    return false;
  }
  normal_path_count.fetch_add(1, std::memory_order_relaxed);
  return true;
}

static const ManorGeometry &
road_query_geometry(void *manor, std::optional<ManorGeometry> &local) {
  auto *context = RoadQueryContext::active;
  if (context != nullptr && context->manor == manor) {
    if (!context->geometry_attempted) {
      context->geometry_attempted = true;
      context->geometry = build_manor_geometry(manor, road_image);
    }
    return context->geometry;
  }
  local = build_manor_geometry(manor, road_image);
  return *local;
}

static bool road_point_in_modules(const ManorGeometry &geometry,
                                  const Vec3 &point, float radius) {
  // This is the exact predicate called by native 0x144b9c590. Its second
  // argument is float in XMM1: the radius is squared and used for triangle
  // edge clearance, not merely for the spatial-cell broad phase.
  using NativeCircle =
      std::uint8_t (*)(const Vec3 *, float, const NativeArray<Vec3> *);
  const auto circle = reinterpret_cast<NativeCircle>(road_image + 0x4b16cf0);
  for (const auto &quad : geometry.module_quads) {
    std::array<Vec3, 6> triangles{quad[0], quad[1], quad[2],
                                  quad[0], quad[2], quad[3]};
    const NativeArray<Vec3> view{triangles.data(), 6, 6};
    if (circle(&point, radius, &view)) {
      return true;
    }
  }
  return false;
}

std::uint8_t road_endpoint_collision(void *game, const Vec3 *point,
                                     float radius, void *ignored_actor,
                                     void *pawn) {
  using Native = std::uint8_t (*)(void *, const Vec3 *, float, void *);
  const auto original = reinterpret_cast<Native>(road_image + 0x4c90000);
  const auto native_result = original(game, point, radius, ignored_actor);
  if (native_result == 0)
    return native_result;
  try {
    Vec3 position{};
    if (!ignored_actor && std::isfinite(radius) && radius >= 0 &&
        static_cast<double>(radius) <=
            std::numeric_limits<std::int32_t>::max() &&
        read_road_value(reinterpret_cast<std::uintptr_t>(point), position)) {
      bool castle{};
      if (void *manor = road_manor(pawn, nullptr, position, castle, game)) {
        if (castle)
          return original(game, point, radius, manor);
        std::optional<ManorGeometry> local;
        const auto &geometry = road_query_geometry(manor, local);
        if (geometry.complete) {
          // The original endpoint helper narrows to int before point-querying.
          if (road_point_in_modules(
                  geometry, position,
                  static_cast<float>(static_cast<std::int32_t>(radius)))) {
            return native_result;
          }
          return original(game, point, radius, manor);
        }
      }
    }
  } catch (...) {
    // Unknown geometry or allocation failure retains the original restriction.
  }
  return native_result;
}

std::uint8_t road_point_collision(void *cache, const Vec3 *point,
                                  std::int32_t radius,
                                  std::uint64_t ignored_weak_actor,
                                  void *pawn) {
  using Native =
      std::uint8_t (*)(void *, const Vec3 *, std::int32_t, std::uint64_t);
  const auto original = reinterpret_cast<Native>(road_image + 0x4b9c590);
  const auto native_result = original(cache, point, radius, ignored_weak_actor);
  if (native_result == 0)
    return native_result;
  try {
    Vec3 position{};
    // A null actor is encoded by a zero serial number, not necessarily zero
    // object index. A non-null pre-existing ignore is never replaced.
    if ((ignored_weak_actor >> 32) == 0 && radius >= 0 &&
        read_road_value(reinterpret_cast<std::uintptr_t>(point), position)) {
      bool castle{};
      if (void *manor = road_manor(pawn, cache, position, castle)) {
        std::optional<ManorGeometry> local;
        const auto *geometry =
            castle ? nullptr : &road_query_geometry(manor, local);
        if (castle || geometry->complete) {
          if (!castle && road_point_in_modules(*geometry, position,
                                               static_cast<float>(radius)))
            return native_result;
          std::uint64_t manor_weak{};
          using AssignWeak = void (*)(std::uint64_t *, void *);
          reinterpret_cast<AssignWeak>(road_image + 0x14938f0)(&manor_weak,
                                                               manor);
          return original(cache, point, radius, manor_weak);
        }
      }
    }
  } catch (...) {
  }
  return native_result;
}

std::uint8_t road_segment_collision(void *cache, const double *start_xy,
                                    const double *end_xy, float half_width,
                                    void *ignored_actor,
                                    std::uint8_t check_flag, void *pawn) {
  using Native = std::uint8_t (*)(void *, const double *, const double *, float,
                                  void *, std::uint8_t);
  const auto original = reinterpret_cast<Native>(road_image + 0x4b9dd70);
  const auto native_result =
      original(cache, start_xy, end_xy, half_width, ignored_actor, check_flag);
  if (native_result == 0)
    return native_result;
  try {
    std::array<double, 2> start{}, end{};
    if (!ignored_actor && std::isfinite(half_width) && half_width > 0 &&
        read_road_value(reinterpret_cast<std::uintptr_t>(start_xy), start) &&
        read_road_value(reinterpret_cast<std::uintptr_t>(end_xy), end)) {
      const Vec3 a{start[0], start[1], 0}, b{end[0], end[1], 0};
      bool castle{};
      void *manor = road_manor(pawn, cache, a, castle);
      if (!manor) {
        manor = road_manor(pawn, cache, b, castle);
      }
      if (manor) {
        if (castle)
          return original(cache, start_xy, end_xy, half_width, manor,
                          check_flag);
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double length_squared = dx * dx + dy * dy;
        if (std::isfinite(length_squared) && length_squared > 0) {
          // Same perpendicular, butt-ended strip as native 0x144b9dd70.
          const double scale = half_width / std::sqrt(length_squared);
          const double nx = -dy * scale, ny = dx * scale;
          const Vec3 a_left{a.x + nx, a.y + ny, 0};
          const Vec3 a_right{a.x - nx, a.y - ny, 0};
          const Vec3 b_left{b.x + nx, b.y + ny, 0};
          const Vec3 b_right{b.x - nx, b.y - ny, 0};
          const std::array triangles{a_right, a_left, b_left,
                                     a_right, b_left, b_right};
          std::optional<ManorGeometry> local;
          const auto &geometry = road_query_geometry(manor, local);
          switch (triangles_intersect_manor_modules(geometry, triangles)) {
          case GeometryVerdict::Blocked:
            return native_result;
          case GeometryVerdict::Clear:
            return original(cache, start_xy, end_xy, half_width, manor,
                            check_flag);
          case GeometryVerdict::Unknown:
            break;
          }
        }
      }
    }
  } catch (...) {
  }
  return native_result;
}

NativeArray<Vec3> *road_path_query(void *cache, NativeArray<Vec3> *output,
                                   const Vec3 *start, const Vec3 *end,
                                   void *pawn) {
  using Native = NativeArray<Vec3> *(*)(void *, NativeArray<Vec3> *,
                                        const Vec3 *, const Vec3 *);
  const auto original = reinterpret_cast<Native>(road_image + 0x4b90890);
  // Even an ineligible nested query masks its parent's policy and restores it
  // on return or a native C++ exception. No geometry allocation occurs here.
  RoadQueryContext scope(cache, pawn);
  Vec3 point{};
  if (cache != nullptr &&
      read_road_value(reinterpret_cast<std::uintptr_t>(start), point))
    scope.manor = road_manor(pawn, cache, point, scope.castle);
  if (scope.manor == nullptr && cache != nullptr &&
      read_road_value(reinterpret_cast<std::uintptr_t>(end), point))
    scope.manor = road_manor(pawn, cache, point, scope.castle);
  return original(cache, output, start, end);
}

std::uint8_t road_path_point_collision(void *cache, const Vec3 *point,
                                       std::int32_t radius,
                                       std::uint64_t ignored_weak_actor) {
  const auto *scope = RoadQueryContext::active;
  if (scope != nullptr && scope->cache == cache && scope->manor != nullptr)
    return road_point_collision(cache, point, radius, ignored_weak_actor,
                                scope->pawn);
  using Native =
      std::uint8_t (*)(void *, const Vec3 *, std::int32_t, std::uint64_t);
  return reinterpret_cast<Native>(road_image + 0x4b9c590)(cache, point, radius,
                                                          ignored_weak_actor);
}

std::uint8_t road_path_segment_collision(void *cache, const double *start_xy,
                                         const double *end_xy, float half_width,
                                         void *ignored_actor,
                                         std::uint8_t check_flag) {
  const auto *scope = RoadQueryContext::active;
  if (scope != nullptr && scope->cache == cache && scope->manor != nullptr)
    return road_segment_collision(cache, start_xy, end_xy, half_width,
                                  ignored_actor, check_flag, scope->pawn);
  using Native = std::uint8_t (*)(void *, const double *, const double *, float,
                                  void *, std::uint8_t);
  return reinterpret_cast<Native>(road_image + 0x4b9dd70)(
      cache, start_xy, end_xy, half_width, ignored_actor, check_flag);
}
