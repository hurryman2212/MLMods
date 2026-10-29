#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include <Windows.h>

#include "Connections.hpp"
#include "ConnectionsReload.hpp"

static constexpr std::int32_t maximum_reload_regions = 256;
static constexpr std::int32_t maximum_reload_objects = 100000;
static constexpr std::int32_t maximum_reload_points = 65536;

static std::uintptr_t reload_image{};
static ConnectionReloadApi reload_api{};

template <class T>
static bool reload_read(std::uintptr_t base, std::size_t offset,
                        T &value) noexcept {
  SIZE_T copied{};
  return base != 0 &&
         offset <= std::numeric_limits<std::uintptr_t>::max() - base &&
         sizeof(T) <=
             std::numeric_limits<std::uintptr_t>::max() - base - offset &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(base + offset),
                           &value, sizeof(value), &copied) != 0 &&
         copied == sizeof(value);
}

template <class T>
static bool reload_array(std::uintptr_t base, std::size_t offset,
                         std::int32_t limit, std::vector<T> &values) {
  NativeArray<T> array{};
  if (!reload_read(base, offset, array) || array.count < 0 ||
      array.capacity < array.count || array.capacity > limit ||
      (array.count != 0 && array.data == nullptr))
    return false;
  values.resize(static_cast<std::size_t>(array.count));
  if (values.empty())
    return true;
  SIZE_T copied{};
  const auto size = values.size() * sizeof(T);
  return ReadProcessMemory(GetCurrentProcess(), array.data, values.data(), size,
                           &copied) != 0 &&
         copied == size;
}

void configure_connection_reload_functions(
    const ConnectionReloadApi &api) noexcept {
  reload_api = api;
}

void configure_connection_reload(std::uintptr_t image) noexcept {
  reload_image = image;
  configure_connection_reload_functions(
      {reinterpret_cast<decltype(ConnectionReloadApi::restore_blocked)>(
           image + 0x4cff790),
       reinterpret_cast<decltype(ConnectionReloadApi::register_road)>(
           image + 0x4ca71a0)});
}

ConnectionReloadStats connections_after_load(void *master_pointer) noexcept {
  ConnectionReloadStats stats{};
  const auto master = reinterpret_cast<std::uintptr_t>(master_pointer);
  if (!reload_image || !master || !reload_api.restore_blocked ||
      !reload_api.register_road) {
    stats.skipped = 1;
    return stats;
  }
  try {
    std::uint8_t ready{};
    if (!reload_read(master, 0x318, ready) || ready != 1) {
      stats.skipped = 1;
      return stats;
    }
    std::vector<std::uintptr_t> regions, buildings, independent;
    if (!reload_array(master, 0x560, maximum_reload_regions, regions) ||
        !reload_array(master, 0x508, maximum_reload_objects, buildings) ||
        !reload_array(master, 0x518, maximum_reload_objects, independent)) {
      stats.skipped = 1;
      return stats;
    }
    std::map<std::uintptr_t, std::uintptr_t> eligible_regions;
    std::set<std::uintptr_t> roads, caches;
    for (const auto region : regions) {
      std::uintptr_t region_master{}, owner{}, manor{}, cache{}, cache_region{};
      std::uint8_t player{};
      std::uint32_t flags{};
      std::int32_t type{};
      if (!reload_read(region, 0x318, region_master) ||
          region_master != master || !reload_read(region, 0x350, owner) ||
          !reload_read(owner, 0x34c, player) || player != 1 ||
          !reload_read(region, 0x358, manor) || !reload_read(manor, 8, flags) ||
          (flags & 0x40018010U) != 0 || !reload_read(manor, 0x3b8, type) ||
          type != 98 || !reload_read(region, 0x738, cache) ||
          !reload_read(cache, 0x20, cache_region) || cache_region != region)
        continue;
      std::int32_t additions{}, free_additions{}, removals{}, free_removals{};
      if (!reload_read(cache, 0x230, additions) || additions < 0 ||
          !reload_read(cache, 0x25c, free_additions) ||
          !reload_read(cache, 0x280, removals) || removals < 0 ||
          !reload_read(cache, 0x2ac, free_removals) ||
          additions != free_additions || removals != free_removals) {
        // Never restore entrances using an incomplete obstacle lookup, even
        // if another mod or unusual load left native updates pending.
        ++stats.skipped;
        continue;
      }
      const auto geometry =
          build_manor_geometry(reinterpret_cast<void *>(manor), reload_image);
      std::vector<std::uintptr_t> region_roads;
      if (!geometry.complete || geometry.module_quads.empty() ||
          !reload_array(region, 0x320, maximum_reload_objects, region_roads)) {
        ++stats.skipped;
        continue;
      }
      eligible_regions.emplace(region, manor);
      caches.insert(cache);
      for (const auto road : region_roads) {
        std::uintptr_t road_master{};
        std::uint8_t road_type{}, wall_type{};
        if (reload_read(road, 8, flags) && (flags & 0x40018010U) == 0 &&
            reload_read(road, 0x2a8, road_master) && road_master == master &&
            reload_read(road, 0x2b4, road_type) && road_type <= 1 &&
            reload_read(road, 0x2b5, wall_type) && wall_type == 0)
          roads.insert(road);
      }
    }
    stats.regions = eligible_regions.size();
    std::set<std::uintptr_t> owners(buildings.begin(), buildings.end());
    owners.insert(independent.begin(), independent.end());
    for (const auto building : owners) {
      std::uintptr_t region{}, building_master{};
      if (!reload_read(building, 0x2c8, region) ||
          !eligible_regions.contains(region) ||
          !reload_read(building, 0x2e8, building_master) ||
          building_master != master ||
          resolve_connection_manor(reinterpret_cast<void *>(building)) !=
              reinterpret_cast<void *>(eligible_regions.at(region)))
        continue;
      std::vector<Vec3> blocked;
      if (!reload_array(building, 0xce8, maximum_reload_points, blocked) ||
          !std::all_of(blocked.begin(), blocked.end(), [](const Vec3 &point) {
            return std::isfinite(point.x) && std::isfinite(point.y) &&
                   std::isfinite(point.z);
          })) {
        ++stats.skipped;
        continue;
      }
      if (blocked.empty())
        continue;
      // This native operation moves only newly valid CE8 points back to CD8,
      // then regenerates their markers, BB8/BC8 associations and road-nav data.
      // The scoped query hook retains every non-Manor and physical obstruction.
      reload_api.restore_blocked(reinterpret_cast<void *>(building));
      ++stats.buildings;
      std::int32_t remaining{};
      if (reload_read(building, 0xcf0, remaining) && remaining >= 0 &&
          static_cast<std::size_t>(remaining) <= blocked.size())
        stats.recovered_points +=
            blocked.size() - static_cast<std::size_t>(remaining);
    }
    // Re-register through the engine, which merges existing point identities
    // and queues their XYZ for link rebuilding. No node flags or edges are
    // fabricated here. All identity sets are local, so repeated loads cannot
    // retain old worlds.
    for (const auto road : roads) {
      reload_api.register_road(reinterpret_cast<void *>(road));
      ++stats.roads;
    }
    // Recovery runs after the outer loader has registered obstacles. Keep the
    // resulting road/snap work queued for the ordinary native tick; never
    // start workers recursively from a load callback.
    stats.caches = caches.size();
  } catch (...) {
    ++stats.skipped;
  }
  return stats;
}
