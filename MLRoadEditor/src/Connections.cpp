#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <Windows.h>

#include "Connections.hpp"

static std::uintptr_t connection_image{};
static ConnectionNavPoint native_connection_nav{};
static ConnectionCachePoint native_connection_cache{};
static ConnectionCircle native_connection_circle{};
static ConnectionAssignWeak native_connection_weak{};

struct ConnectionContext {
  void *manor{};
  const Vec3 *point{};
  float radius{};
  bool geometry_attempted{};
  ManorGeometry geometry;
};

static thread_local ConnectionContext *active_connection{};

class ScopedConnection {
  ConnectionContext *previous_;

public:
  explicit ScopedConnection(ConnectionContext &context) noexcept
      : previous_(active_connection) {
    active_connection = &context;
  }
  ~ScopedConnection() { active_connection = previous_; }
  ScopedConnection(const ScopedConnection &) = delete;
  auto operator=(const ScopedConnection &) -> ScopedConnection & = delete;
};

template <class T>
static bool connection_read(std::uintptr_t base, std::size_t offset,
                            T &value) noexcept {
  SIZE_T copied{};
  return base != 0 &&
         offset <= std::numeric_limits<std::uintptr_t>::max() - base &&
         sizeof(T) <=
             std::numeric_limits<std::uintptr_t>::max() - base - offset &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(base + offset),
                           &value, sizeof(T), &copied) != 0 &&
         copied == sizeof(T);
}

void *resolve_connection_manor(void *building_pointer, void *nav,
                               void *cache) noexcept {
  const auto building = reinterpret_cast<std::uintptr_t>(building_pointer);
  std::uintptr_t region{}, owner{}, manager{}, region_manager{}, manor{},
      region_cache{}, global_nav{};
  std::uint8_t main_player{};
  std::uint32_t building_flags{}, manor_flags{};
  std::int32_t type{};
  if (!connection_image || !connection_read(building, 8, building_flags) ||
      (building_flags & 0x40018000U) != 0 ||
      !connection_read(building, 0x2c8, region) || !region ||
      !connection_read(building, 0x2e8, manager) || !manager ||
      !connection_read(region, 0x318, region_manager) ||
      region_manager != manager || !connection_read(region, 0x350, owner) ||
      !owner || !connection_read(owner, 0x34c, main_player) ||
      main_player != 1 || !connection_read(region, 0x738, region_cache) ||
      !region_cache ||
      (cache && region_cache != reinterpret_cast<std::uintptr_t>(cache)) ||
      !connection_read(manager, 0x2c0, global_nav) || !global_nav ||
      (nav && global_nav != reinterpret_cast<std::uintptr_t>(nav)) ||
      !connection_read(region, 0x358, manor) || !manor ||
      !connection_read(manor, 8, manor_flags) ||
      (manor_flags & 0x40018000U) != 0 ||
      !connection_read(manor, 0x3b8, type) || type != 98) {
    return nullptr;
  }
  return reinterpret_cast<void *>(manor);
}

static bool connection_point(const Vec3 *point, float radius,
                             Vec3 &position) noexcept {
  return std::isfinite(radius) && radius >= 0 &&
         connection_read(reinterpret_cast<std::uintptr_t>(point), 0,
                         position) &&
         std::isfinite(position.x) && std::isfinite(position.y) &&
         std::isfinite(position.z);
}

static bool connection_hits_modules(const ManorGeometry &geometry,
                                    const Vec3 &position, float radius) {
  for (const auto &quad : geometry.module_quads) {
    std::array<Vec3, 6> vertices{quad[0], quad[1], quad[2],
                                 quad[0], quad[2], quad[3]};
    const NativeArray<Vec3> triangles{vertices.data(), 6, 6};
    if (native_connection_circle(&position, radius, &triangles) != 0) {
      return true;
    }
  }
  return false;
}

void configure_connection_functions(ConnectionNavPoint nav,
                                    ConnectionCachePoint cache,
                                    ConnectionCircle circle,
                                    ConnectionAssignWeak weak) noexcept {
  native_connection_nav = nav;
  native_connection_cache = cache;
  native_connection_circle = circle;
  native_connection_weak = weak;
}

void configure_connections(std::uintptr_t image) noexcept {
  connection_image = image;
  configure_connection_functions(
      reinterpret_cast<ConnectionNavPoint>(image + 0x4c46b20),
      reinterpret_cast<ConnectionCachePoint>(image + 0x4b9c590),
      reinterpret_cast<ConnectionCircle>(image + 0x4b16cf0),
      reinterpret_cast<ConnectionAssignWeak>(image + 0x14938f0));
}

std::uint8_t connections_initial_point(void *nav, const Vec3 *point,
                                       float radius, void *building) {
  ConnectionContext context{};
  try {
    Vec3 position{};
    if (connection_point(point, radius, position)) {
      context.manor = resolve_connection_manor(building, nav, nullptr);
      if (context.manor) {
        context.point = point;
        context.radius = radius;
      }
    }
  } catch (...) {
    context.manor = nullptr;
  }
  // Even an ineligible nested query masks the outer exception. Native query
  // execution and any traversal/output ownership remain with the game.
  const ScopedConnection scope(context);
  return native_connection_nav(nav, point, radius);
}

std::uint8_t connections_nav_circle(const Vec3 *point, float radius,
                                    const NativeArray<Vec3> *triangles) {
  const auto original = native_connection_circle(point, radius, triangles);
  auto *context = active_connection;
  if (context && context->manor && context->point == point &&
      context->radius == radius &&
      reinterpret_cast<std::uintptr_t>(triangles) ==
          reinterpret_cast<std::uintptr_t>(context->manor) + 0x688) {
    try {
      Vec3 position{};
      if (connection_point(point, radius, position)) {
        // Most native queries never reach the Manor. Snapshot only when its
        // own triangles are tested, and retry no failed snapshot in this query.
        if (!context->geometry_attempted) {
          context->geometry_attempted = true;
          context->geometry =
              build_manor_geometry(context->manor, connection_image);
        }
        if (context->geometry.complete &&
            !context->geometry.module_quads.empty()) {
          return connection_hits_modules(context->geometry, position, radius)
                     ? 1
                     : 0;
        }
      }
    } catch (...) {
    }
  }
  return original;
}

std::uint8_t connections_mask_circle(const Vec3 *point, float radius,
                                     const NativeArray<Vec3> *triangles,
                                     void *masking_building) {
  const auto original = native_connection_circle(point, radius, triangles);
  try {
    Vec3 position{};
    if (connection_point(point, radius, position) &&
        reinterpret_cast<std::uintptr_t>(triangles) ==
            reinterpret_cast<std::uintptr_t>(masking_building) + 0x688) {
      void *manor =
          resolve_connection_manor(masking_building, nullptr, nullptr);
      if (manor && manor == masking_building) {
        const auto geometry = build_manor_geometry(manor, connection_image);
        if (geometry.complete && !geometry.module_quads.empty()) {
          return connection_hits_modules(geometry, position, radius) ? 1 : 0;
        }
      }
    }
  } catch (...) {
  }
  return original;
}

std::uint8_t connections_restore_point(void *cache, const Vec3 *point,
                                       std::int32_t radius,
                                       std::uint64_t ignored, void *building) {
  std::uint64_t native_ignored = ignored;
  bool physical_collision{};
  try {
    Vec3 position{};
    if (radius >= 0 && (ignored >> 32) == 0 &&
        connection_point(point, static_cast<float>(radius), position)) {
      if (void *manor = resolve_connection_manor(building, nullptr, cache)) {
        const auto geometry = build_manor_geometry(manor, connection_image);
        if (geometry.complete && !geometry.module_quads.empty()) {
          physical_collision = connection_hits_modules(
              geometry, position, static_cast<float>(radius));
          if (!physical_collision) {
            native_connection_weak(&native_ignored, manor);
            if ((native_ignored >> 32) == 0) {
              native_ignored = ignored;
            }
          }
        }
      }
    }
  } catch (...) {
    native_ignored = ignored;
    physical_collision = false;
  }
  const auto original =
      native_connection_cache(cache, point, radius, native_ignored);
  return original == 0 && physical_collision ? 1 : original;
}
