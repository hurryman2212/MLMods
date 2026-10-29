#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

#include "Connections.hpp"
#include "ConnectionsReload.hpp"

enum class ReloadCallKind { Restore, RegisterRoad, UpdateCache };
enum class ReloadGeometry { Complete, Incomplete, Empty, Throws };

template <class T>
concept HasManualCacheTick = requires(T api) { api.update_cache; };

static_assert(!HasManualCacheTick<ConnectionReloadApi>,
              "load recovery must not expose a manual cache tick");

struct ReloadCall {
  ReloadCallKind kind;
  void *object;

  bool operator==(const ReloadCall &) const = default;
};

static std::vector<ReloadCall> calls;
static std::map<void *, ReloadGeometry> geometry_modes;
static std::map<void *, void *> building_manors;
static std::map<void *, std::int32_t> native_remaining;
static std::vector<void *> native_queued_roads;
static int unexpected_arguments{}, geometry_calls{}, resolver_calls{};
static int native_pending_obstacles{}, native_cached_obstacles{},
    native_worker_starts{}, obstacles_at_worker_start{};

ManorGeometry build_manor_geometry(void *manor, std::uintptr_t image) {
  ++geometry_calls;
  const auto found = geometry_modes.find(manor);
  if (image != 0x140000000 || found == geometry_modes.end()) {
    ++unexpected_arguments;
    return {};
  }
  if (found->second == ReloadGeometry::Throws) {
    throw std::runtime_error("synthetic physical geometry failure");
  }
  ManorGeometry result;
  result.complete = found->second != ReloadGeometry::Incomplete;
  if (found->second != ReloadGeometry::Empty) {
    result.module_quads.push_back(
        {{{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}}});
  }
  return result;
}

void *resolve_connection_manor(void *building, void *nav,
                               void *cache) noexcept {
  ++resolver_calls;
  if (nav != nullptr || cache != nullptr) {
    ++unexpected_arguments;
  }
  const auto found = building_manors.find(building);
  return found == building_manors.end() ? nullptr : found->second;
}

static void native_restore(void *building) {
  calls.push_back({ReloadCallKind::Restore, building});
  const auto found = native_remaining.find(building);
  if (found != native_remaining.end()) {
    // Only this native stand-in may alter the engine-owned blocked count.
    std::memcpy(static_cast<std::byte *>(building) + 0xcf0, &found->second,
                sizeof(found->second));
  }
}

static void native_register_road(void *road) {
  calls.push_back({ReloadCallKind::RegisterRoad, road});
  native_queued_roads.push_back(road);
}

static void native_update_cache(void *cache) {
  calls.push_back({ReloadCallKind::UpdateCache, cache});
  // The native tick starts field-graph work before its obstacle queue step.
  // The outer loader must have drained its initial obstacles before this call.
  ++native_worker_starts;
  obstacles_at_worker_start = native_cached_obstacles;
  native_cached_obstacles += native_pending_obstacles;
  native_pending_obstacles = 0;
  native_queued_roads.clear();
}

static void reset_mocks() {
  calls.clear();
  geometry_modes.clear();
  building_manors.clear();
  native_remaining.clear();
  native_queued_roads.clear();
  unexpected_arguments = geometry_calls = resolver_calls = 0;
  native_pending_obstacles = native_cached_obstacles = native_worker_starts =
      obstacles_at_worker_start = 0;
  configure_connection_reload(0x140000000);
  configure_connection_reload_functions({native_restore, native_register_road});
}

struct ReloadFixture {
  static constexpr std::size_t master = 0x0000, region = 0x0800, pawn = 0x1000,
                               manor = 0x2000, building = 0x3000,
                               independent = 0x4000, cache = 0x5000,
                               road_zero = 0x6000, road_one = 0x6400,
                               wall = 0x6800, region_list = 0x7000,
                               building_list = 0x7040,
                               independent_list = 0x7080, road_list = 0x70c0,
                               blocked_points = 0x7200,
                               independent_points = 0x7300,
                               live_points = 0x7400;
  alignas(std::max_align_t) std::array<std::byte, 0x8000> memory{};

  void *at(std::size_t offset) { return memory.data() + offset; }

  template <class T> void put(std::size_t offset, const T &value) {
    std::memcpy(memory.data() + offset, &value, sizeof(value));
  }

  ReloadFixture() {
    put(master + 0x318, std::uint8_t{1});
    put(region_list, std::array{at(region), at(region)});
    put(building_list,
        std::array<void *, 3>{at(building), at(building), nullptr});
    put(independent_list,
        std::array{at(building), at(independent), at(independent)});
    put(road_list, std::array{at(road_zero), at(road_one), at(wall),
                              at(road_zero), at(road_one)});
    put(master + 0x560,
        NativeArray<void *>{static_cast<void **>(at(region_list)), 2, 2});
    put(master + 0x508,
        NativeArray<void *>{static_cast<void **>(at(building_list)), 3, 3});
    put(master + 0x518,
        NativeArray<void *>{static_cast<void **>(at(independent_list)), 3, 3});
    put(region + 0x318, at(master));
    put(region + 0x320,
        NativeArray<void *>{static_cast<void **>(at(road_list)), 5, 5});
    put(region + 0x350, at(pawn));
    put(region + 0x358, at(manor));
    put(region + 0x738, at(cache));
    put(cache + 0x20, at(region));
    put(pawn + 0x34c, std::uint8_t{1});
    put(pawn + 0x8d8, std::uint8_t{0xa5});
    put(pawn + 0x8d9, std::uint8_t{0x5a});
    put(manor + 0x3b8, std::int32_t{98});
    for (const auto offset : {building, independent}) {
      put(offset + 0x2c8, at(region));
      put(offset + 0x2e8, at(master));
      put(offset + 0xcd8,
          NativeArray<Vec3>{static_cast<Vec3 *>(at(live_points)), 2, 2});
      put(offset + 0xbb8, std::uint64_t{0x1122334455667788});
      put(offset + 0xbc8, std::uint64_t{0x8877665544332211});
      building_manors[at(offset)] = at(manor);
    }
    put(blocked_points,
        std::array{Vec3{1, 2, 3}, Vec3{4, 5, 6}, Vec3{7, 8, 9}});
    put(independent_points, std::array{Vec3{10, 11, 12}, Vec3{13, 14, 15}});
    put(live_points, std::array{Vec3{16, 17, 18}, Vec3{19, 20, 21}});
    put(building + 0xce8,
        NativeArray<Vec3>{static_cast<Vec3 *>(at(blocked_points)), 3, 3});
    put(independent + 0xce8,
        NativeArray<Vec3>{static_cast<Vec3 *>(at(independent_points)), 2, 2});
    for (const auto offset : {road_zero, road_one, wall}) {
      put(offset + 0x2a8, at(master));
    }
    put(road_one + 0x2b4, std::uint8_t{1});
    put(wall + 0x2b4, std::uint8_t{3});
    geometry_modes[at(manor)] = ReloadGeometry::Complete;
  }

  std::vector<ReloadCall> expected_calls() {
    return {{ReloadCallKind::Restore, at(building)},
            {ReloadCallKind::Restore, at(independent)},
            {ReloadCallKind::RegisterRoad, at(road_zero)},
            {ReloadCallKind::RegisterRoad, at(road_one)}};
  }
};

int main() {
  int checks{}, failures{};
  const auto require = [&](bool value, const char *message) {
    ++checks;
    if (!value) {
      ++failures;
      std::fprintf(stderr, "connection reload: %s\n", message);
    }
  };
  {
    reset_mocks();
    ReloadFixture fixture;
    auto expected = fixture;
    native_remaining[fixture.at(ReloadFixture::building)] = 1;
    native_remaining[fixture.at(ReloadFixture::independent)] = 0;
    expected.put(ReloadFixture::building + 0xcf0, std::int32_t{1});
    expected.put(ReloadFixture::independent + 0xcf0, std::int32_t{0});
    const auto stats =
        connections_after_load(fixture.at(ReloadFixture::master));
    require(stats.regions == 1 && stats.buildings == 2 && stats.roads == 2 &&
                stats.caches == 1 && stats.skipped == 0,
            "duplicate regions, the 508/518 building union and regular roads "
            "are processed once each");
    require(calls == fixture.expected_calls(),
            "all blocked points restore before road registrations without "
            "manually ticking the regional cache");
    require(stats.recovered_points == 4,
            "recovery statistics reflect the counts changed by the native "
            "restore operation");
    require(fixture.memory == expected.memory,
            "only the native mock changes C98 counts; arrays, geometry, "
            "associations, actor fields and setting bytes remain untouched");
    require(unexpected_arguments == 0 && resolver_calls == 2,
            "geometry and connection resolution receive current identities");
  }
  {
    reset_mocks();
    ReloadFixture fixture;
    native_pending_obstacles = 317;
    fixture.put(ReloadFixture::master + 0x318, std::uint8_t{0});
    fixture.put(ReloadFixture::cache + 0x230, std::int32_t{317});
    const auto premature =
        connections_after_load(fixture.at(ReloadFixture::master));
    require(
        premature.skipped == 1 && calls.empty() && native_worker_starts == 0,
        "earlier child-load callback cannot recover entrances or start work");
    // The native outer loader installs collision data and marks the master
    // ready.
    native_cached_obstacles += native_pending_obstacles;
    native_pending_obstacles = 0;
    fixture.put(ReloadFixture::cache + 0x230, std::int32_t{0});
    fixture.put(ReloadFixture::master + 0x318, std::uint8_t{1});
    const auto before = fixture.memory;
    const auto stats =
        connections_after_load(fixture.at(ReloadFixture::master));
    require(stats.caches == 1 && calls == fixture.expected_calls() &&
                native_worker_starts == 0 && native_pending_obstacles == 0 &&
                native_cached_obstacles == 317,
            "completed-load recovery sees installed obstacles without "
            "starting field-graph workers");
    require(native_queued_roads ==
                    std::vector<void *>{fixture.at(ReloadFixture::road_zero),
                                        fixture.at(ReloadFixture::road_one)} &&
                fixture.memory == before,
            "native road registrations remain queued for the ordinary tick "
            "without changing engine-owned collision or worker state");

    native_update_cache(fixture.at(ReloadFixture::cache));
    require(native_worker_starts == 1 && obstacles_at_worker_start == 317 &&
                native_queued_roads.empty(),
            "the ordinary tick starts queued graph work only after the outer "
            "loader has installed every initial collision obstacle");
  }
  for (const auto offset : {0x230, 0x280}) {
    reset_mocks();
    ReloadFixture fixture;
    fixture.put(ReloadFixture::cache + offset, std::int32_t{1});
    const auto before = fixture.memory;
    const auto stats =
        connections_after_load(fixture.at(ReloadFixture::master));
    require(stats.skipped > 0 && stats.regions == 0 && calls.empty() &&
                fixture.memory == before,
            "pending obstacle queues never admit false entrance recovery");
  }
  {
    reset_mocks();
    ReloadFixture fixture;
    const auto before = fixture.memory;
    const auto stats =
        connections_after_load(fixture.at(ReloadFixture::master));
    require(stats.buildings == 2 && stats.recovered_points == 0 &&
                calls == fixture.expected_calls(),
            "native refusal to restore physically obstructed points is "
            "preserved while regeneration still executes once");
    require(fixture.memory == before,
            "reload itself writes no engine memory when native callbacks "
            "leave the world unchanged");
  }
  for (int mode{}; mode < 13; ++mode) {
    reset_mocks();
    ReloadFixture fixture;
    switch (mode) {
    case 0:
      fixture.put(ReloadFixture::pawn + 0x34c, std::uint8_t{0});
      break;
    case 1:
      fixture.put(ReloadFixture::pawn + 0x34c, std::uint8_t{2});
      break;
    case 2:
      fixture.put(ReloadFixture::region + 0x318,
                  fixture.at(ReloadFixture::manor));
      break;
    case 3:
      fixture.put(ReloadFixture::manor + 0x3b8, std::int32_t{57});
      break;
    case 4:
    case 5:
    case 6:
    case 7: {
      const std::array<std::uint32_t, 4> flags{0x10, 0x8000, 0x10000,
                                               0x40000000};
      fixture.put(ReloadFixture::manor + 8, flags[mode - 4]);
      break;
    }
    case 8:
      fixture.put(ReloadFixture::cache + 0x20,
                  fixture.at(ReloadFixture::manor));
      break;
    case 9:
      fixture.put(ReloadFixture::region + 0x738, std::uintptr_t{});
      break;
    case 10:
      fixture.put(ReloadFixture::region + 0x358, std::uintptr_t{});
      break;
    case 11:
      fixture.put(ReloadFixture::region + 0x350, std::uintptr_t{});
      break;
    case 12:
      fixture.put(ReloadFixture::region + 0x350, std::uintptr_t{1});
      break;
    }
    const auto before = fixture.memory;
    const auto stats =
        connections_after_load(fixture.at(ReloadFixture::master));
    require(stats.regions == 0 && stats.buildings == 0 && stats.roads == 0 &&
                stats.caches == 0 && calls.empty() && geometry_calls == 0,
            "AI, other worlds, non-Manors, invalid actors and wrong cache "
            "outers cannot enter load recovery");
    require(fixture.memory == before,
            "ineligible region context remains byte-for-byte unchanged");
  }
  for (const auto mode : {ReloadGeometry::Incomplete, ReloadGeometry::Empty,
                          ReloadGeometry::Throws}) {
    reset_mocks();
    ReloadFixture fixture;
    geometry_modes[fixture.at(ReloadFixture::manor)] = mode;
    const auto before = fixture.memory;
    const auto stats =
        connections_after_load(fixture.at(ReloadFixture::master));
    require(stats.regions == 0 && stats.skipped > 0 && calls.empty(),
            "unavailable physical geometry leaves every native connection "
            "and cache unchanged");
    require(fixture.memory == before,
            "geometry failure never writes a replacement footprint");
  }
  for (int mode{}; mode < 3; ++mode) {
    reset_mocks();
    ReloadFixture fixture;
    if (mode == 0) {
      fixture.put(ReloadFixture::building + 0x2c8,
                  fixture.at(ReloadFixture::manor));
    } else if (mode == 1) {
      fixture.put(ReloadFixture::building + 0x2e8,
                  fixture.at(ReloadFixture::manor));
    } else {
      building_manors[fixture.at(ReloadFixture::building)] = nullptr;
    }
    const auto before = fixture.memory;
    auto expected = fixture.expected_calls();
    expected.erase(expected.begin());
    const auto stats =
        connections_after_load(fixture.at(ReloadFixture::master));
    require(stats.buildings == 1 && calls == expected,
            "building region, master and resolved Manor must all match "
            "before native restoration");
    require(fixture.memory == before,
            "foreign or unresolved buildings are never edited");
  }
  for (int mode{}; mode < 10; ++mode) {
    reset_mocks();
    ReloadFixture fixture;
    auto blocked = NativeArray<Vec3>{
        static_cast<Vec3 *>(fixture.at(ReloadFixture::blocked_points)), 3, 3};
    switch (mode) {
    case 0:
      blocked.count = -1;
      break;
    case 1:
      blocked.capacity = 2;
      break;
    case 2:
      blocked.capacity = 65537;
      break;
    case 3:
      blocked.data = nullptr;
      break;
    case 4:
      fixture.put(ReloadFixture::blocked_points,
                  std::numeric_limits<double>::quiet_NaN());
      break;
    case 5:
      fixture.put(ReloadFixture::blocked_points + sizeof(double),
                  std::numeric_limits<double>::infinity());
      break;
    case 6:
      fixture.put(ReloadFixture::blocked_points + 2 * sizeof(double),
                  -std::numeric_limits<double>::infinity());
      break;
    case 7:
      blocked.data = reinterpret_cast<Vec3 *>(std::uintptr_t{1});
      break;
    case 8:
      blocked.count = 0;
      break;
    case 9:
      blocked = {};
      break;
    }
    fixture.put(ReloadFixture::building + 0xce8, blocked);
    const auto before = fixture.memory;
    auto expected = fixture.expected_calls();
    expected.erase(expected.begin());
    const auto stats =
        connections_after_load(fixture.at(ReloadFixture::master));
    require(stats.buildings == 1 && stats.recovered_points == 0 &&
                stats.skipped == (mode < 8 ? 1 : 0) && calls == expected,
            "only readable finite nonempty C98 points reach native restore; "
            "invalid buildings do not suppress valid independent buildings");
    require(fixture.memory == before,
            "malformed or empty point arrays are never repaired by mod writes");
  }
  for (const std::size_t offset :
       {std::size_t{0x560}, std::size_t{0x508}, std::size_t{0x518},
        ReloadFixture::region + 0x320}) {
    reset_mocks();
    ReloadFixture fixture;
    fixture.put(offset, NativeArray<void *>{nullptr, 1, 1});
    const auto before = fixture.memory;
    const auto stats =
        connections_after_load(fixture.at(ReloadFixture::master));
    require(stats.skipped > 0 && stats.regions == 0 && calls.empty(),
            "invalid region, master building and regional road arrays fail "
            "without scheduling partial recovery");
    require(fixture.memory == before,
            "invalid native collection descriptors are not rewritten");
  }
  for (int mode{}; mode < 7; ++mode) {
    reset_mocks();
    ReloadFixture fixture;
    switch (mode) {
    case 0:
      fixture.put(ReloadFixture::road_zero + 0x2a8,
                  fixture.at(ReloadFixture::manor));
      break;
    case 1:
      fixture.put(ReloadFixture::road_zero + 0x2b4, std::uint8_t{2});
      break;
    case 2:
      fixture.put(ReloadFixture::road_zero + 0x2b4, std::uint8_t{3});
      break;
    case 3:
      fixture.put(ReloadFixture::road_zero + 0x2b5, std::uint8_t{1});
      break;
    case 4:
      fixture.put(ReloadFixture::road_zero + 8, std::uint32_t{0x10});
      break;
    case 5:
    case 6:
      fixture.put(ReloadFixture::road_list,
                  std::uintptr_t{mode == 5 ? 0U : 1U});
      fixture.put(ReloadFixture::road_list + 3 * sizeof(void *),
                  std::uintptr_t{mode == 5 ? 0U : 1U});
      break;
    }
    const auto before = fixture.memory;
    auto expected = fixture.expected_calls();
    expected.erase(expected.begin() + 2);
    const auto stats =
        connections_after_load(fixture.at(ReloadFixture::master));
    require(stats.roads == 1 && stats.caches == 1 && calls == expected,
            "only live regular road types zero and one in the loaded world "
            "are registered; wall roads and invalid actors are excluded");
    require(fixture.memory == before,
            "road filtering never edits node flags, edges or actor fields");
  }
  for (const std::int32_t remaining : {0, 3, -1, 4}) {
    reset_mocks();
    ReloadFixture fixture;
    auto expected = fixture;
    native_remaining[fixture.at(ReloadFixture::building)] = remaining;
    expected.put(ReloadFixture::building + 0xcf0, remaining);
    const auto stats =
        connections_after_load(fixture.at(ReloadFixture::master));
    require(stats.recovered_points == (remaining == 0 ? 3U : 0U) &&
                stats.buildings == 2 && calls == fixture.expected_calls(),
            "recovered counts are read after native restore and malformed "
            "native counts cannot underflow or inflate statistics");
    require(fixture.memory == expected.memory,
            "native remaining counts are observed without mod correction");
  }
  {
    reset_mocks();
    ReloadFixture first, second;
    const auto first_before = first.memory, second_before = second.memory;
    for (auto *fixture : {&first, &second, &first}) {
      calls.clear();
      const auto stats =
          connections_after_load(fixture->at(ReloadFixture::master));
      require(stats.regions == 1 && stats.buildings == 2 && stats.roads == 2 &&
                  stats.caches == 1 && stats.skipped == 0 &&
                  calls == fixture->expected_calls(),
              "A to B to A loads resolve the current world and regenerate "
              "exactly once per invocation without stale cache identities");
    }
    require(first.memory == first_before && second.memory == second_before &&
                unexpected_arguments == 0,
            "repeated loads neither mutate nor reuse another world's data");
  }
  for (int mode{}; mode < 5; ++mode) {
    reset_mocks();
    ReloadFixture fixture;
    auto *master = fixture.at(ReloadFixture::master);
    switch (mode) {
    case 0:
      master = nullptr;
      break;
    case 1:
      configure_connection_reload(0);
      break;
    case 2:
      configure_connection_reload_functions({nullptr, native_register_road});
      break;
    case 3:
      configure_connection_reload_functions({native_restore, nullptr});
      break;
    case 4:
      master = reinterpret_cast<void *>(std::uintptr_t{1});
      break;
    }
    const auto before = fixture.memory;
    const auto stats = connections_after_load(master);
    require(stats.skipped == 1 && stats.regions == 0 && calls.empty() &&
                fixture.memory == before,
            "missing configuration or unreadable master safely skips all "
            "native operations");
  }
  std::printf("connection reload: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
