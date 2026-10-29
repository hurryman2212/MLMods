#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "Upgrade.hpp"

static std::size_t upgrade_checks{};
static void check_upgrade(bool condition, const char *message) {
  ++upgrade_checks;
  if (!condition)
    throw std::runtime_error(message);
}

template <typename T, std::size_t N>
static void upgrade_put(std::array<std::uint8_t, N> &buffer, std::size_t offset,
                        const T &value) {
  if (offset + sizeof(value) > N)
    throw std::runtime_error("upgrade fixture write bounds");
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

struct UpgradeFixture {
  alignas(16) std::array<std::uint8_t, 0xc00> building{};
  alignas(16) std::array<std::uint8_t, 0x360> pawn{};
  alignas(16) std::array<std::uint8_t, 0x360> master{};
  alignas(16) std::array<std::uint8_t, 0x340> module{};
  alignas(16) std::array<std::uint8_t, 0x30> object_class{};
  std::array<void *, 1> modules{};

  UpgradeFixture() {
    upgrade_put(object_class, 0, city_image + 0x1000);
    const auto setup = [&](auto &object, int index) {
      upgrade_put(object, 0, city_image + 0x2000);
      upgrade_put(object, 0xc, index);
      upgrade_put(object, 0x10,
                  reinterpret_cast<std::uintptr_t>(object_class.data()));
    };
    setup(building, 10);
    setup(pawn, 11);
    setup(module, 12);
    setup(master, 13);
    upgrade_put(building, 0x2e0, reinterpret_cast<std::uintptr_t>(pawn.data()));
    upgrade_put(building, 0x2e8,
                reinterpret_cast<std::uintptr_t>(master.data()));
    upgrade_put(pawn, 0x34c, std::uint8_t{1});
    upgrade_put(module, 0x314, 2.0F);
    upgrade_put(module, 0x318, 10.0F);
    modules[0] = module.data();
    upgrade_put(building, 0x2f8, CityArray{modules.data(), 1, 1});
    upgrade_put(building, 0x3c1, std::uint8_t{1});
    upgrade_put(building, 0x100, std::uint64_t{0x1122334455667788ULL});
  }
};

enum class UpgradeMode { Sync, Async, WrongId, Complete, Throw };
static UpgradeMode upgrade_mode{};
static std::unordered_map<std::uint64_t, void *> upgrade_weak_objects;
static std::uint64_t upgrade_next_weak{1};
static std::vector<int> upgrade_events;
static std::size_t upgrade_begins{}, upgrade_setups{}, upgrade_finishes{};
static std::size_t upgrade_resource_calls{}, upgrade_wealth_calls{};
static std::size_t upgrade_reconstruction_calls{};
static std::int32_t upgrade_reconstruction_type{};
static void *upgrade_reconstruction_building{};
static bool upgrade_finished_rebuilding{};
static std::size_t upgrade_type_initializations{};
static std::uint8_t upgrade_native_function{};
static std::uint8_t upgrade_function_before_initialize{};
static std::size_t upgrade_matures{};
static void *upgrade_mature_building{};
static std::int32_t upgrade_mature_id{};
static float upgrade_work_argument{}, upgrade_visual_argument{};
static std::uint8_t upgrade_visual_material{};
static std::int32_t upgrade_last_id{};
static bool upgrade_reenter_finish{};
static std::array<std::uint8_t, 24> upgrade_original_resources{};
static std::function<void(void *)> upgrade_begin_tail, upgrade_artisan_tail;
static std::function<void(void *)> upgrade_finish_tail;
static std::function<void(void *)> upgrade_initialize_body;
static std::function<void(void *, std::int32_t)> upgrade_mature_body;
static std::function<void(void *, void **)> upgrade_artisan_callback_body;
using UpgradeAssemblyBody = std::function<void(void *, void *, void *, void *,
                                               std::uint8_t, CityArray *)>;
static UpgradeAssemblyBody upgrade_request_body, upgrade_callback_body;
static std::size_t upgrade_requests{}, upgrade_callbacks{};

static void upgrade_mock_assign(std::uint64_t *weak, void *object) {
  for (const auto &[key, value] : upgrade_weak_objects)
    if (value == object) {
      *weak = key;
      return;
    }
  *weak = upgrade_next_weak++;
  upgrade_weak_objects[*weak] = object;
}

static void *upgrade_mock_get(const std::uint64_t *weak) {
  const auto found = upgrade_weak_objects.find(*weak);
  return found == upgrade_weak_objects.end() ? nullptr : found->second;
}

static CityArray *upgrade_mock_resource(void *, CityArray *out,
                                        std::int32_t id) {
  ++upgrade_resource_calls;
  upgrade_last_id = id;
  if (out)
    *out = {upgrade_original_resources.data(), 1, 1};
  return out;
}

static std::int32_t upgrade_mock_wealth(void *, std::int32_t id) {
  ++upgrade_wealth_calls;
  upgrade_last_id = id;
  return 23;
}

static CityArray *upgrade_mock_reconstruction_cost(void *building,
                                                   CityArray *out,
                                                   std::int32_t type) {
  ++upgrade_reconstruction_calls;
  upgrade_reconstruction_type = type;
  upgrade_reconstruction_building = building;
  if (out)
    *out = {upgrade_original_resources.data(), 1, 1};
  return out;
}

static void upgrade_mock_initialize_type(void *building) {
  ++upgrade_type_initializations;
  auto *bytes = static_cast<std::uint8_t *>(building);
  std::int32_t type{};
  std::memcpy(&type, bytes + 0x3b8, sizeof(type));
  upgrade_function_before_initialize = bytes[0x3cc];
  // Native Initialize derives Function from the restored building's stats
  // only while Function is Default. It leaves a stale Rubble value intact.
  if (type != 75 && bytes[0x3cc] == 0 && upgrade_native_function != 0)
    bytes[0x3cc] = upgrade_native_function;
  if (upgrade_initialize_body)
    upgrade_initialize_body(building);
}

static void upgrade_mock_setup(void *object, std::int32_t id,
                               std::uint8_t sound) {
  ++upgrade_setups;
  upgrade_events.push_back(20);
  // Native SetupUpgradeCosts retains Initialize's construction costs while
  // rebuilding. Its ordinary upgrade-cost getters are not used on that path.
  if (static_cast<std::uint8_t *>(object)[0x3a2] == 0) {
    CityArray result{};
    city_upgrade_resource_cost(object, &result, id);
    city_upgrade_wealth_cost(object, id);
    std::memcpy(static_cast<std::uint8_t *>(object) + 0x3d0, &result,
                sizeof(result));
  }
  check_upgrade(sound == 1, "native sound flag forwarded");
  upgrade_events.push_back(21);
}

static void upgrade_mock_begin(void *object, std::int32_t id) {
  ++upgrade_begins;
  upgrade_events.push_back(10);
  auto *bytes = static_cast<std::uint8_t *>(object);
  if (upgrade_mode == UpgradeMode::Throw)
    throw std::runtime_error("synthetic native failure");
  if (upgrade_mode != UpgradeMode::Complete) {
    bytes[0x3a1] = 1;
    bytes[0x3c1] = 0;
  }
  if (upgrade_mode != UpgradeMode::Async)
    city_upgrade_cost_setup(
        object, upgrade_mode == UpgradeMode::WrongId ? id + 1 : id, 1);
  if (upgrade_begin_tail)
    upgrade_begin_tail(object);
  upgrade_events.push_back(11);
}

struct UpgradeCapture {
  void *building{};
  std::int32_t id{};
};

static void upgrade_mock_type(void *raw) {
  const auto &capture = *static_cast<UpgradeCapture *>(raw);
  upgrade_events.push_back(30);
  city_upgrade_cost_setup(capture.building, capture.id, 1);
  upgrade_events.push_back(31);
}

static void upgrade_mock_extension(void *object, std::int32_t index,
                                   std::int32_t type) {
  upgrade_events.push_back(40);
  check_upgrade(index == 0 && type == 3, "extension arguments unchanged");
  city_begin_upgrade(object, 103);
  upgrade_events.push_back(41);
}

static void upgrade_mock_artisan(void *object, std::int32_t type) {
  upgrade_events.push_back(50);
  check_upgrade(type == 3, "artisan argument unchanged");
  city_change_extension(object, 0, type);
  if (upgrade_artisan_tail)
    upgrade_artisan_tail(object);
  upgrade_events.push_back(51);
}

static void upgrade_mock_artisan_callback(void *capture, void **replacement) {
  upgrade_events.push_back(70);
  if (upgrade_artisan_callback_body)
    upgrade_artisan_callback_body(capture, replacement);
  upgrade_events.push_back(71);
}

static void upgrade_mock_assembly_request(void *master, void *building,
                                          void *path, void *transform,
                                          std::uint8_t flag, CityArray *tags) {
  ++upgrade_requests;
  upgrade_events.push_back(80);
  if (upgrade_request_body)
    upgrade_request_body(master, building, path, transform, flag, tags);
  upgrade_events.push_back(81);
}

static void upgrade_mock_assembly_callback(void *master, void *building,
                                           void *path, void *transform,
                                           std::uint8_t flag, CityArray *tags) {
  ++upgrade_callbacks;
  upgrade_events.push_back(90);
  if (upgrade_callback_body)
    upgrade_callback_body(master, building, path, transform, flag, tags);
  upgrade_events.push_back(91);
}

static float upgrade_mock_done(void *object) {
  CityArray modules{};
  std::memcpy(&modules, static_cast<std::uint8_t *>(object) + 0x2f8,
              sizeof(modules));
  float result{};
  for (std::int32_t i = 0; i < modules.count; ++i) {
    auto *module = static_cast<std::uint8_t **>(modules.data)[i];
    float work{};
    std::memcpy(&work, module + 0x314, sizeof(work));
    result += work;
  }
  return result;
}

static float upgrade_mock_required(void *object) {
  CityArray modules{};
  std::memcpy(&modules, static_cast<std::uint8_t *>(object) + 0x2f8,
              sizeof(modules));
  float result{};
  for (std::int32_t i = 0; i < modules.count; ++i) {
    auto *module = static_cast<std::uint8_t **>(modules.data)[i];
    float work{};
    std::memcpy(&work, module + 0x318, sizeof(work));
    result += work;
  }
  return result;
}

static void upgrade_mock_add(void *object, float work) {
  upgrade_events.push_back(60);
  upgrade_work_argument = work;
  CityArray modules{};
  std::memcpy(&modules, static_cast<std::uint8_t *>(object) + 0x2f8,
              sizeof(modules));
  // Match native AddWork: distribute across positive-work modules, skipping
  // decoration. A single-module mock conceals real house/extension ordering.
  for (std::int32_t i = 0; i < modules.count; ++i) {
    auto *module = static_cast<std::uint8_t **>(modules.data)[i];
    float done{}, required{};
    std::memcpy(&done, module + 0x314, sizeof(done));
    std::memcpy(&required, module + 0x318, sizeof(required));
    if (required <= 0.0F)
      continue;
    const float added = std::min(required - done, work);
    done += added;
    work -= added;
    std::memcpy(module + 0x314, &done, sizeof(done));
    if (work <= 0.0F)
      break;
  }
}

static void upgrade_mock_visual(void *, float progress,
                                std::uint8_t materials) {
  upgrade_events.push_back(61);
  upgrade_visual_argument = progress;
  upgrade_visual_material = materials;
}

static void upgrade_mock_finish(void *object) {
  upgrade_events.push_back(62);
  ++upgrade_finishes;
  if (upgrade_reenter_finish)
    city_upgrade_cost_setup(object, 7, 1);
  auto *bytes = static_cast<std::uint8_t *>(object);
  upgrade_finished_rebuilding = bytes[0x3a2] == 1;
  bytes[0x3c1] = 1;
  bytes[0x3a1] = 0;
  bytes[0x3a2] = 0;
  if (upgrade_finish_tail)
    upgrade_finish_tail(object);
}

static bool upgrade_mock_mature(void *object, std::int32_t id) {
  upgrade_events.push_back(72);
  ++upgrade_matures;
  upgrade_mature_building = object;
  upgrade_mature_id = id;
  const auto *bytes = static_cast<const std::uint8_t *>(object);
  check_upgrade(
      bytes[0x3c1] == 1 && bytes[0x3a1] == 0,
      "maturity helper runs only after native construction completes");
  if (upgrade_mature_body)
    upgrade_mature_body(object, id);
  return true;
}

static UpgradeApi upgrade_test_api() {
  return {upgrade_mock_begin,
          upgrade_mock_setup,
          upgrade_mock_type,
          upgrade_mock_extension,
          upgrade_mock_artisan,
          upgrade_mock_assign,
          upgrade_mock_get,
          upgrade_mock_done,
          upgrade_mock_required,
          upgrade_mock_add,
          upgrade_mock_visual,
          upgrade_mock_finish,
          upgrade_mock_resource,
          upgrade_mock_wealth,
          upgrade_mock_artisan_callback,
          upgrade_mock_assembly_request,
          upgrade_mock_assembly_callback,
          upgrade_mock_mature,
          upgrade_mock_reconstruction_cost,
          upgrade_mock_initialize_type};
}

static void reset_upgrade_fixture_state() {
  configure_upgrade(upgrade_test_api());
  upgrade_weak_objects.clear();
  upgrade_events.clear();
  upgrade_mode = UpgradeMode::Sync;
  upgrade_begins = upgrade_setups = upgrade_finishes = 0;
  upgrade_resource_calls = upgrade_wealth_calls = 0;
  upgrade_reconstruction_calls = 0;
  upgrade_reconstruction_type = 0;
  upgrade_reconstruction_building = nullptr;
  upgrade_finished_rebuilding = false;
  upgrade_type_initializations = 0;
  upgrade_native_function = 0;
  upgrade_function_before_initialize = 0;
  upgrade_requests = upgrade_callbacks = 0;
  upgrade_matures = 0;
  upgrade_mature_building = nullptr;
  upgrade_mature_id = 0;
  upgrade_reenter_finish = false;
  upgrade_begin_tail = upgrade_artisan_tail = {};
  upgrade_artisan_callback_body = {};
  upgrade_request_body = upgrade_callback_body = {};
  upgrade_finish_tail = {};
  upgrade_initialize_body = {};
  upgrade_mature_body = {};
}

struct UpgradeAssemblyCall {
  struct Path {
    std::array<std::uint64_t, 2> names{0x1234, 0x5678};
    CityArray subpath{};
  } path;
  void *master{};
  void *building{};
  std::u16string subpath{u"House.Brewery"};
  alignas(16) std::array<std::uint8_t, 96> transform{};
  std::uint8_t flag{0xff};
  std::array<std::uint64_t, 2> values{0x1111, 0x2222};
  CityArray tags{};

  explicit UpgradeAssemblyCall(UpgradeFixture &fixture, int variant = 0)
      : master(fixture.master.data()), building(fixture.building.data()) {
    path.names[1] += static_cast<std::uint64_t>(variant);
    upgrade_put(transform, 24, 1.0);
    for (const auto offset : {64, 72, 80})
      upgrade_put(transform, static_cast<std::size_t>(offset), 1.0);
  }

  void refresh() {
    const auto count = static_cast<std::int32_t>(subpath.size() + 1);
    path.subpath = {subpath.data(), count, count};
    tags = {values.data(), static_cast<std::int32_t>(values.size()),
            static_cast<std::int32_t>(values.size())};
  }

  void request() {
    refresh();
    city_upgrade_assembly_request(master, building, &path, transform.data(),
                                  flag, &tags);
  }

  void complete() {
    refresh();
    city_upgrade_assembly_callback(master, building, &path, transform.data(),
                                   flag, &tags);
  }
};

static_assert(sizeof(UpgradeAssemblyCall::Path) == 32);

struct UpgradeHouseModules {
  std::array<std::array<std::uint8_t, 0x340>, 97> storage{};
  std::array<void *, 97> pointers{};

  explicit UpgradeHouseModules(UpgradeFixture &fixture) {
    // Read-only 0.8.117 Kesseleck Brewery snapshot: 97 actual modules,
    // 500 completed work of 1020 required. Preserve their array ordering.
    constexpr std::array models{
        271, 271, 271, 271, 272, 0, 0, 0, 0,   0,   0,   0, 0, 0,
        0,   0,   0,   0,   0,   0, 0, 0, 461, 0,   0,   0, 0, 0,
        0,   0,   0,   0,   0,   0, 0, 0, 0,   0,   0,   0, 0, 0,
        0,   441, 506, 0,   0,   0, 0, 0, 0,   521, 444, 0, 0, 0,
        0,   522, 523, 523, 0,   0, 0, 0, 0,   0,   0,   0, 0, 0,
        0,   0,   0,   0,   0,   0, 0, 0, 0,   0,   0,   0, 0, 271,
        271, 271, 271, 272, 461, 0, 0, 0, 0,   0,   0,   0, 0};
    static_assert(models.size() == 97);
    for (std::size_t i = 0; i < storage.size(); ++i) {
      storage[i] = fixture.module;
      upgrade_put(storage[i], 12, static_cast<std::int32_t>(100 + i));
      upgrade_put(storage[i], 0x2c0, models[i]);
      upgrade_put(storage[i], 0x2c8, fixture.building.data());
      upgrade_put(storage[i], 0x314, 0.0F);
      upgrade_put(storage[i], 0x318, 0.0F);
      pointers[i] = storage[i].data();
    }
    for (const auto i : {22, 43, 44, 51, 52, 88}) {
      auto &module = storage[static_cast<std::size_t>(i)];
      const bool main = i == 22 || i == 88;
      upgrade_put(module, 0x318, main ? 500.0F : 5.0F);
      upgrade_put(module, 0x312, std::uint8_t{1});
      upgrade_put(module, 0x313, static_cast<std::uint8_t>(main));
    }
    upgrade_put(storage[22], 0x314, 500.0F);
  }

  void attach(UpgradeFixture &fixture) {
    upgrade_put(fixture.building, 0x2f8, CityArray{pointers.data(), 97, 97});
  }
};

static void run_house_rebuild_query_tests() {
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture, other;
    auto *building = fixture.building.data();
    check_upgrade(city_can_rebuild_house(building) &&
                      !city_can_rebuild_house(nullptr),
                  "house query accepts idle owner and rejects null");
    UpgradeAssemblyCall house(fixture);
    house.request();
    check_upgrade(!city_can_rebuild_house(building) &&
                      city_can_rebuild_house(other.building.data()),
                  "pending house assembly blocks only its owner");
    upgrade_callback_body = [&](void *, void *, void *, void *, std::uint8_t,
                                CityArray *) {
      check_upgrade(!city_can_rebuild_house(building),
                    "executing house callback remains a barrier");
    };
    house.complete();
    upgrade_callback_body = {};
    check_upgrade(city_can_rebuild_house(building),
                  "finished callback releases rebuild query");
    upgrade_callback_body = [&](void *, void *, void *, void *, std::uint8_t,
                                CityArray *) {
      check_upgrade(!city_can_rebuild_house(building),
                    "unmatched callback cannot authorize nested house cleanup");
    };
    house.complete();
    upgrade_callback_body = {};
    check_upgrade(city_can_rebuild_house(building),
                  "unmatched callback scope releases without leaking a block");
    UpgradeAssemblyCall extension(fixture, 1);
    upgrade_put(fixture.building, 0xb88,
                CityArray{fixture.module.data(), 1, 1});
    extension.flag = 0;
    extension.request();
    check_upgrade(city_can_rebuild_house(building),
                  "retained extension request permits house rebuild");
    extension.complete();
    for (const std::uint8_t index : {1, 127, 128, 254}) {
      UpgradeAssemblyCall unknown(fixture, index + 2);
      unknown.flag = index;
      unknown.request();
      check_upgrade(!city_can_rebuild_house(building),
                    "unknown or signed-negative assembly source cannot "
                    "authorize cleanup");
      unknown.complete();
    }
    upgrade_put(fixture.module, 0x311, std::uint8_t{1});
    check_upgrade(!city_can_rebuild_house(building),
                  "pending old main replacement blocks rebuild");
    upgrade_put(fixture.module, 0x311, std::uint8_t{0});
    UpgradeCapture capture{building, 105};
    void *replacement = fixture.module.data();
    upgrade_artisan_callback_body = [&](void *, void **) {
      check_upgrade(city_can_rebuild_house(building),
                    "artisan scope permits query after main removal");
    };
    city_upgrade_artisan_callback(&capture, &replacement);
    upgrade_artisan_callback_body = {};
    upgrade_finish_tail = [&](void *) {
      check_upgrade(!city_can_rebuild_house(building),
                    "active finalizer cannot authorize rebuild");
    };
    city_begin_upgrade(building, 7);
    upgrade_finish_tail = {};
    check_upgrade(city_can_rebuild_house(building),
                  "finalization scope clears after native return");
    upgrade_put(fixture.building, 0x2f8, CityArray{nullptr, -1, 0});
    check_upgrade(!city_can_rebuild_house(building),
                  "invalid modules deny rebuild");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    UpgradeAssemblyCall request(fixture);
    request.refresh();
    city_upgrade_assembly_request(request.master, request.building, nullptr,
                                  request.transform.data(), request.flag,
                                  &request.tags);
    check_upgrade(!city_can_rebuild_house(request.building),
                  "unreliable tracking denies rebuild");
    auto api = upgrade_test_api();
    api.weak_assign = [](std::uint64_t *weak, void *) { *weak = 0; };
    configure_upgrade(api);
    check_upgrade(!city_can_rebuild_house(request.building),
                  "invalid weak identity denies rebuild");
    api = upgrade_test_api();
    api.finish = nullptr;
    configure_upgrade(api);
    check_upgrade(!city_can_rebuild_house(request.building),
                  "incomplete editor API denies rebuild");
  }
  reset_upgrade_fixture_state();
}

static void run_upgrade_assembly_tests() {
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    UpgradeHouseModules house(fixture);
    UpgradeAssemblyCall first(fixture), second(fixture, 1);
    auto *building = fixture.building.data();
    const auto pawn_before = fixture.pawn;
    UpgradeCapture capture{building, 103};
    void *replacement = fixture.module.data();
    upgrade_artisan_tail = [&](void *) {
      upgrade_put(fixture.module, 0x311, std::uint8_t{1});
      // A flagged module may already be pending destruction: the barrier
      // must defer before treating that transient state as a hard failure.
      upgrade_put(fixture.module, 8, std::uint32_t{0x40000000});
    };
    city_change_artisan(building, 3);
    check_upgrade(upgrade_pending_count() == 1 && upgrade_finishes == 0 &&
                      upgrade_failed_count() == 0,
                  "level-four artisan replacement retains its pending job");
    upgrade_artisan_callback_body = [&](void *raw, void **module) {
      check_upgrade(raw == &capture && module == &replacement,
                    "artisan callback forwards both native arguments");
      house.attach(fixture);
      check_upgrade(upgrade_mock_done(building) == 500.0F &&
                        upgrade_mock_required(building) == 1020.0F,
                    "live Brewery fixture reproduces 500/1020 work");
      first.request();
      second.request();
    };
    city_upgrade_artisan_callback(&capture, &replacement);
    check_upgrade(upgrade_finishes == 0 && upgrade_pending_count() == 1 &&
                      upgrade_requests == 2 && upgrade_failed_count() == 0,
                  "artisan callback cannot finish before its child assemblies");
    first.complete();
    check_upgrade(upgrade_finishes == 0 && upgrade_pending_count() == 1,
                  "first of two assembly completions retains the barrier");
    upgrade_callback_body = [&](void *, void *, void *, void *, std::uint8_t,
                                CityArray *) {
      check_upgrade(
          upgrade_finishes == 0,
          "finalization waits until the last native callback returns");
    };
    second.complete();
    check_upgrade(
        upgrade_finishes == 1 && upgrade_pending_count() == 0 &&
            upgrade_failed_count() == 0 && upgrade_work_argument == 520.0F &&
            upgrade_mock_done(building) == 1020.0F &&
            fixture.building[0x3c1] == 1 && fixture.building[0x3a1] == 0,
        "97-module Brewery completes exactly the missing 520 work");
    check_upgrade(fixture.pawn == pawn_before,
                  "asynchronous completion leaves player data unchanged");
    upgrade_callback_body = {};
    second.complete();
    check_upgrade(upgrade_finishes == 1,
                  "duplicate assembly callback cannot finalize twice");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    UpgradeAssemblyCall request(fixture);
    UpgradeCapture capture{fixture.building.data(), 103};
    void *replacement = fixture.module.data();
    upgrade_request_body = [](void *master, void *building, void *path,
                              void *transform, std::uint8_t flag,
                              CityArray *tags) {
      city_upgrade_assembly_callback(master, building, path, transform, flag,
                                     tags);
      check_upgrade(upgrade_finishes == 0,
                    "inline assembly callback remains inside request scope");
    };
    upgrade_artisan_callback_body = [&](void *, void **) {
      upgrade_put(fixture.module, 0x311, std::uint8_t{0});
      request.request();
      check_upgrade(upgrade_finishes == 0,
                    "inline assembly cannot escape native artisan callback");
    };
    upgrade_artisan_tail = [&](void *) {
      upgrade_put(fixture.module, 0x311, std::uint8_t{1});
      city_upgrade_artisan_callback(&capture, &replacement);
      check_upgrade(upgrade_finishes == 0,
                    "synchronous replacement waits for outer artisan return");
    };
    city_change_artisan(fixture.building.data(), 3);
    check_upgrade(upgrade_finishes == 1 && upgrade_pending_count() == 0 &&
                      upgrade_failed_count() == 0,
                  "fully synchronous nested replacement finishes once");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    UpgradeAssemblyCall request(fixture);
    request.request();
    city_begin_upgrade(fixture.building.data(), 105);
    check_upgrade(upgrade_finishes == 0 && upgrade_pending_count() == 1,
                  "assembly scheduled before BeginUpgrade remains a barrier");
    for (int mismatch = 0; mismatch < 6; ++mismatch) {
      auto wrong = request;
      if (mismatch == 0)
        ++wrong.path.names[0];
      else if (mismatch == 1)
        wrong.subpath += u".Other";
      else if (mismatch == 2)
        wrong.transform[40] ^= 1;
      else if (mismatch == 3)
        wrong.flag ^= 1;
      else if (mismatch == 4)
        ++wrong.values[0];
      else {
        UpgradeFixture other;
        wrong.building = other.building.data();
        wrong.complete();
        check_upgrade(upgrade_finishes == 0 && upgrade_pending_count() == 1,
                      "foreign-building callback cannot consume a request");
        continue;
      }
      wrong.complete();
      check_upgrade(upgrade_finishes == 0 && upgrade_pending_count() == 1,
                    "unmatched immutable assembly key does not unblock work");
    }
    auto copied = request;
    copied.subpath.reserve(128);
    copied.refresh();
    check_upgrade(copied.path.subpath.data != request.path.subpath.data,
                  "completion uses a distinct deep-copied FString allocation");
    upgrade_callback_body = [](void *, void *, void *path, void *, std::uint8_t,
                               CityArray *tags) {
      // Native completion owns and releases these arguments. Identity must
      // have been captured before the original callback consumes them.
      static_cast<UpgradeAssemblyCall::Path *>(path)->subpath = {};
      *tags = {};
    };
    copied.complete();
    check_upgrade(upgrade_finishes == 1 && upgrade_pending_count() == 0,
                  "deep-copy identity survives native argument consumption");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    UpgradeAssemblyCall request(fixture);
    request.request();
    request.complete();
    check_upgrade(upgrade_requests == 1 && upgrade_callbacks == 1 &&
                      upgrade_finishes == 0 && upgrade_pending_count() == 0,
                  "load-only assembly does not authorize an upgrade finish");
    upgrade_put(fixture.pawn, 0x34d, std::uint8_t{1});
    request.request();
    city_begin_upgrade(fixture.building.data(), 105);
    request.complete();
    check_upgrade(upgrade_finishes == 0 && upgrade_pending_count() == 0,
                  "AI assembly and upgrades retain native behavior");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    UpgradeAssemblyCall request(fixture);
    request.request();
    request.request();
    city_begin_upgrade(fixture.building.data(), 105);
    request.complete();
    check_upgrade(
        upgrade_finishes == 0 && upgrade_pending_count() == 1,
        "identical request payloads retain separate outstanding work");
    request.complete();
    check_upgrade(
        upgrade_finishes == 1 && upgrade_pending_count() == 0 &&
            upgrade_requests == 2 && upgrade_callbacks == 2 &&
            upgrade_failed_count() == 0,
        "both identical assembly requests must complete exactly once");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    UpgradeAssemblyCall request(fixture);
    request.request();
    city_begin_upgrade(fixture.building.data(), 105);
    bool reentered{};
    upgrade_callback_body = [&](void *master, void *building, void *path,
                                void *transform, std::uint8_t flag,
                                CityArray *tags) {
      if (!reentered) {
        reentered = true;
        city_upgrade_assembly_callback(master, building, path, transform, flag,
                                       tags);
        check_upgrade(
            upgrade_finishes == 0 && upgrade_pending_count() == 1,
            "reentrant callback cannot consume its executing request");
      }
    };
    request.complete();
    check_upgrade(reentered && upgrade_callbacks == 2 &&
                      upgrade_finishes == 1 && upgrade_pending_count() == 0 &&
                      upgrade_failed_count() == 0,
                  "reentrant native completion preserves exactly-once finish");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    UpgradeAssemblyCall request(fixture);
    request.request();
    city_begin_upgrade(fixture.building.data(), 105);
    for (auto &[weak, object] : upgrade_weak_objects) {
      (void)weak;
      if (object == fixture.building.data())
        object = nullptr;
    }
    request.complete();
    check_upgrade(
        upgrade_finishes == 0,
        "destroyed weak target is never finalized by a late assembly");
    UpgradeFixture next;
    UpgradeAssemblyCall next_request(next);
    next_request.request();
    city_begin_upgrade(next.building.data(), 105);
    check_upgrade(
        upgrade_pending_count() == 1 && upgrade_finishes == 0,
        "new request and job prune expired assembly and upgrade state");
    next_request.complete();
    check_upgrade(upgrade_pending_count() == 0 && upgrade_finishes == 1 &&
                      fixture.building[0x3c1] == 0 && next.building[0x3c1] == 1,
                  "expired assembly state cannot obstruct a new live job");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    UpgradeAssemblyCall request(fixture);
    request.refresh();
    city_upgrade_assembly_request(
        request.master, request.building, reinterpret_cast<void *>(1),
        request.transform.data(), request.flag, &request.tags);
    city_begin_upgrade(fixture.building.data(), 105);
    check_upgrade(upgrade_requests == 1 && upgrade_finishes == 0,
                  "unreadable request identity makes completion fail closed");
  }
  reset_upgrade_fixture_state();
}

static void run_upgrade_maturity_tests() {
  for (const auto mode : {UpgradeMode::Complete, UpgradeMode::Sync}) {
    for (const auto id : {32, 33, 34, 112}) {
      reset_upgrade_fixture_state();
      UpgradeFixture fixture;
      upgrade_mode = mode;
      const auto before = fixture.building;
      const auto pawn_before = fixture.pawn;
      city_begin_upgrade(fixture.building.data(), id);
      check_upgrade(
          upgrade_matures == 1 &&
              upgrade_mature_building == fixture.building.data() &&
              upgrade_mature_id == id && upgrade_pending_count() == 0,
          "each fruit choice and new orchard routes once to maturity");
      const bool instant = mode == UpgradeMode::Complete;
      check_upgrade(
          upgrade_finishes == (instant ? 0U : 1U) &&
              upgrade_events ==
                  (instant ? std::vector<int>{10, 20, 21, 11, 72}
                           : std::vector<int>{10, 20, 21, 11, 60, 61, 62, 72}),
          "native-complete and constructed paths mature after their original "
          "work");
      check_upgrade(fixture.building == before && fixture.pawn == pawn_before,
                    "maturity routing does not itself mutate game state");
      city_upgrade_cost_setup(fixture.building.data(), id, 1);
      check_upgrade(
          upgrade_matures == 1,
          "repeated setup without pending authorization cannot mature again");
    }
  }
  for (const auto id :
       {2, 4, 5, 7, 29, 30, 31, 104, 105, 13, 19, 21, 26, 27, 28, 999}) {
    reset_upgrade_fixture_state();
    UpgradeFixture fixture;
    upgrade_mode = UpgradeMode::Complete;
    city_begin_upgrade(fixture.building.data(), id);
    check_upgrade(upgrade_matures == 0,
                  "house levels, other crafts, vegetables, animals and unknown "
                  "IDs do not mature");
  }
  for (const bool already_complete : {false, true}) {
    reset_upgrade_fixture_state();
    UpgradeFixture fixture;
    upgrade_mode = already_complete ? UpgradeMode::Complete : UpgradeMode::Sync;
    upgrade_put(fixture.pawn, 0x34d, std::uint8_t{1});
    city_begin_upgrade(fixture.building.data(), 33);
    check_upgrade(
        upgrade_matures == 0 && upgrade_finishes == 0,
        "AI orchard remains entirely native in both completion paths");
  }
  for (const auto mode : {UpgradeMode::Complete, UpgradeMode::Sync}) {
    reset_upgrade_fixture_state();
    UpgradeFixture fixture;
    UpgradeAssemblyCall first(fixture), second(fixture, 1);
    upgrade_mode = mode;
    upgrade_begin_tail = [&](void *) {
      first.request();
      second.request();
      check_upgrade(upgrade_matures == 0,
                    "new orchard cannot mature during original BeginUpgrade");
    };
    city_begin_upgrade(fixture.building.data(), 112);
    check_upgrade(upgrade_matures == 0 && upgrade_pending_count() == 1,
                  "orchard waits for both outstanding assemblies");
    auto wrong = first;
    wrong.path.names[0] ^= 1;
    wrong.complete();
    first.complete();
    check_upgrade(
        upgrade_matures == 0 && upgrade_pending_count() == 1,
        "unmatched or partial assembly completion cannot mature orchard");
    upgrade_callback_body = [](void *, void *, void *, void *, std::uint8_t,
                               CityArray *) {
      check_upgrade(
          upgrade_matures == 0,
          "last original assembly callback must return before maturity");
    };
    second.complete();
    check_upgrade(
        upgrade_matures == 1 && upgrade_mature_id == 112 &&
            upgrade_events.back() == 72 && upgrade_pending_count() == 0,
        "last assembly releases exactly one authorized maturity action");
    upgrade_callback_body = {};
    second.complete();
    check_upgrade(
        upgrade_matures == 1,
        "duplicate asynchronous callback cannot remature old orchard");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    UpgradeAssemblyCall request(fixture);
    upgrade_mode = UpgradeMode::Complete;
    upgrade_request_body = [](void *master, void *building, void *path,
                              void *transform, std::uint8_t flag,
                              CityArray *tags) {
      city_upgrade_assembly_callback(master, building, path, transform, flag,
                                     tags);
      check_upgrade(
          upgrade_matures == 0,
          "inline assembly callback stays inside original request scope");
    };
    upgrade_begin_tail = [&](void *) {
      request.request();
      check_upgrade(
          upgrade_matures == 0,
          "inline request completion still waits for BeginUpgrade return");
    };
    city_begin_upgrade(fixture.building.data(), 32);
    check_upgrade(upgrade_matures == 1 && upgrade_finishes == 0,
                  "synchronous nested native-complete orchard matures once");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    UpgradeAssemblyCall request(fixture);
    request.request();
    request.complete();
    UpgradeCapture capture{fixture.building.data(), 33};
    city_upgrade_type_callback(&capture);
    check_upgrade(
        upgrade_matures == 0 && upgrade_pending_count() == 0,
        "load-only assemblies and type callbacks cannot authorize maturity");
  }
  for (const bool expire_identity : {false, true}) {
    reset_upgrade_fixture_state();
    UpgradeFixture fixture;
    UpgradeAssemblyCall request(fixture);
    request.request();
    city_begin_upgrade(fixture.building.data(), 33);
    if (expire_identity) {
      for (auto &[weak, object] : upgrade_weak_objects) {
        (void)weak;
        if (object == fixture.building.data())
          object = nullptr;
      }
      std::uint64_t replacement_identity{};
      upgrade_mock_assign(&replacement_identity, fixture.building.data());
    } else {
      upgrade_put(fixture.pawn, 0x34d, std::uint8_t{1});
    }
    request.complete();
    check_upgrade(
        upgrade_matures == 0 && upgrade_finishes == 0,
        "late callback rejects changed owner or recycled weak identity");
  }
  for (const bool expire_identity : {false, true}) {
    reset_upgrade_fixture_state();
    UpgradeFixture fixture;
    upgrade_finish_tail = [&](void *building) {
      if (expire_identity) {
        for (auto &[weak, object] : upgrade_weak_objects) {
          (void)weak;
          if (object == building)
            object = nullptr;
        }
      } else {
        upgrade_put(fixture.pawn, 0x34d, std::uint8_t{1});
      }
    };
    city_begin_upgrade(fixture.building.data(), 34);
    check_upgrade(
        upgrade_finishes == 1 && upgrade_matures == 0,
        "owner and weak identity revalidated after finalizer before maturity");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    upgrade_put(fixture.module, 0x318, std::numeric_limits<float>::quiet_NaN());
    city_begin_upgrade(fixture.building.data(), 112);
    check_upgrade(upgrade_finishes == 0 && upgrade_matures == 0 &&
                      upgrade_failed_count() == 1,
                  "invalid construction state cannot invoke maturity");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    upgrade_mode = UpgradeMode::Complete;
    upgrade_mature_body = [](void *building, std::int32_t id) {
      city_upgrade_cost_setup(building, id, 1);
    };
    city_begin_upgrade(fixture.building.data(), 33);
    check_upgrade(upgrade_matures == 1 && upgrade_pending_count() == 0,
                  "maturity helper reentrancy cannot authorize itself twice");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    upgrade_mode = UpgradeMode::Throw;
    bool caught{};
    try {
      city_begin_upgrade(fixture.building.data(), 33);
    } catch (const std::runtime_error &) {
      caught = true;
    }
    check_upgrade(
        caught && upgrade_matures == 0 && upgrade_pending_count() == 0,
        "native exception cancels maturity and unwinds authorization");
    upgrade_mode = UpgradeMode::Complete;
    city_begin_upgrade(fixture.building.data(), 34);
    check_upgrade(upgrade_matures == 1,
                  "maturity scopes recover after a previous native exception");
  }
  reset_upgrade_fixture_state();
  {
    UpgradeFixture fixture;
    auto api = upgrade_test_api();
    api.mature = nullptr;
    configure_upgrade(api);
    upgrade_mode = UpgradeMode::Complete;
    city_begin_upgrade(fixture.building.data(), 33);
    check_upgrade(
        upgrade_matures == 0 && upgrade_pending_count() == 0 &&
            upgrade_resource_calls == 1 && upgrade_wealth_calls == 1,
        "incomplete maturity API keeps native cost and completion behavior");
  }
  reset_upgrade_fixture_state();
}

static void run_reconstruction_tests() {
  for (const auto [restored_type, native_function] :
       {std::pair{52, 18}, std::pair{72, 6}, std::pair{8, 1}}) {
    for (const bool asynchronous : {false, true}) {
      reset_upgrade_fixture_state();
      UpgradeFixture fixture;
      UpgradeAssemblyCall request(fixture);
      upgrade_put(fixture.building, 0x3b8, restored_type);
      fixture.building[0x3cc] = 21;
      upgrade_native_function = static_cast<std::uint8_t>(native_function);
      const auto original_building = fixture.building;
      const auto original_pawn = fixture.pawn;
      const auto original_module = fixture.module;
      upgrade_mode = UpgradeMode::Async;
      upgrade_initialize_body = [&](void *building) {
        check_upgrade(building == fixture.building.data() &&
                          upgrade_function_before_initialize == 0 &&
                          fixture.building[0x3cc] == native_function,
                      "native initialization restores the table-derived "
                      "function before typed registration or resource demand");
        CityArray costs{};
        check_upgrade(
            city_reconstruction_resource_cost(fixture.building.data(), &costs,
                                              restored_type) == &costs,
            "reconstruction preserves native output-array ABI");
        upgrade_put(fixture.building, 0x3d0, costs);
        check_upgrade(
            costs.count == 0 && costs.data == nullptr &&
                upgrade_reconstruction_calls == 0,
            "rebuilding costs are empty before native regional demand "
            "registration");
      };
      const auto initialize = [&] {
        city_initialize_reconstruction(fixture.building.data());
        city_upgrade_cost_setup(fixture.building.data(), 16, 1);
        check_upgrade(upgrade_resource_calls == 0 &&
                          upgrade_wealth_calls == 0 &&
                          fixture.building[0x3a2] == 1 && upgrade_finishes == 0,
                      "native rebuild setup skips ordinary cost getters and "
                      "retains rebuilding state until callback return");
      };
      upgrade_begin_tail = [&](void *building) {
        static_cast<std::uint8_t *>(building)[0x3a2] = 1;
        if (asynchronous)
          request.request();
        else
          initialize();
      };
      upgrade_callback_body = [&](void *, void *, void *, void *, std::uint8_t,
                                  CityArray *) { initialize(); };
      city_begin_upgrade(fixture.building.data(), 16);
      if (asynchronous) {
        check_upgrade(upgrade_finishes == 0 && upgrade_pending_count() == 1,
                      "reconstruction waits for its native assembly callback");
        request.complete();
      }
      check_upgrade(
          upgrade_finishes == 1 && upgrade_pending_count() == 0 &&
              upgrade_failed_count() == 0 && upgrade_finished_rebuilding &&
              upgrade_matures == 0 && upgrade_type_initializations == 1,
          "reconstruction reaches native completion exactly once with "
          "its rebuilding flag intact");
      auto expected_module = original_module;
      auto expected_building = original_building;
      expected_building[0x3cc] = static_cast<std::uint8_t>(native_function);
      upgrade_put(expected_module, 0x314, 10.0F);
      check_upgrade(fixture.building == expected_building &&
                        fixture.pawn == original_pawn &&
                        fixture.module == expected_module &&
                        upgrade_work_argument == 8.0F &&
                        upgrade_visual_argument == 1.0F,
                    "native reconstruction finishes only remaining work and "
                    "preserves inventory, upgrades, families and position");
      upgrade_callback_body = {};
      if (asynchronous)
        request.complete();
      check_upgrade(
          upgrade_finishes == 1,
          "duplicate assembly callback cannot finalize rebuild again");
    }
  }
  for (const int state : {0, 1, 2, 3, 4}) {
    reset_upgrade_fixture_state();
    UpgradeFixture fixture;
    upgrade_mode = UpgradeMode::Async;
    fixture.building[0x3a1] = 1;
    fixture.building[0x3a2] = 1;
    fixture.building[0x3c1] = 0;
    fixture.building[0x3cc] = 21;
    upgrade_put(fixture.building, 0x3b8, std::int32_t{52});
    upgrade_native_function = 18;
    if (state == 1) {
      fixture.pawn[0x34d] = 1;
      city_begin_upgrade(fixture.building.data(), 16);
    } else if (state == 2) {
      city_begin_upgrade(fixture.building.data(), 7);
    } else if (state == 3) {
      city_begin_upgrade(fixture.building.data(), 16);
      fixture.building[0x3a2] = 0;
    } else if (state == 4) {
      auto api = upgrade_test_api();
      api.finish = nullptr;
      configure_upgrade(api);
      city_begin_upgrade(fixture.building.data(), 16);
    }
    city_initialize_reconstruction(fixture.building.data());
    check_upgrade(upgrade_type_initializations == 1 &&
                      upgrade_function_before_initialize == 21 &&
                      fixture.building[0x3cc] == 21,
                  "loaded sites, AI, unrelated upgrades, cleared rebuilding "
                  "flags and incomplete APIs keep native Initialize behavior");
    CityArray result{};
    check_upgrade(city_reconstruction_resource_cost(fixture.building.data(),
                                                    &result, 56) == &result &&
                      result.count == 1 && upgrade_reconstruction_calls == 1 &&
                      upgrade_reconstruction_type == 56 &&
                      upgrade_reconstruction_building ==
                          fixture.building.data() &&
                      upgrade_finishes == 0,
                  "loaded sites, AI, unrelated upgrades, non-rebuild flags and "
                  "incomplete APIs retain native construction costs");
    check_upgrade(city_reconstruction_resource_cost(fixture.building.data(),
                                                    nullptr, 123) == nullptr &&
                      upgrade_reconstruction_calls == 2 &&
                      upgrade_reconstruction_type == 123,
                  "null output forwards the original reconstruction type");
  }
  for (const auto [restored_type, function] :
       {std::pair{52, 18}, std::pair{52, 6}, std::pair{75, 21},
        std::pair{0, 21}}) {
    reset_upgrade_fixture_state();
    UpgradeFixture fixture;
    upgrade_mode = UpgradeMode::Async;
    upgrade_put(fixture.building, 0x3b8, restored_type);
    fixture.building[0x3a2] = 1;
    fixture.building[0x3cc] = static_cast<std::uint8_t>(function);
    upgrade_native_function = 18;
    city_begin_upgrade(fixture.building.data(), 16);
    city_initialize_reconstruction(fixture.building.data());
    check_upgrade(upgrade_type_initializations == 1 &&
                      upgrade_function_before_initialize == function &&
                      fixture.building[0x3cc] == function &&
                      upgrade_finishes == 0,
                  "pending rebuild does not reset specialized functions, "
                  "Rubble building type or an invalid restored type");
  }
  reset_upgrade_fixture_state();
}

int run_upgrade_tests() {
  const auto previous_image = city_image;
  try {
    city_image = 0x140000000;
    reset_upgrade_fixture_state();
    UpgradeFixture fixture;
    auto *building = fixture.building.data();
    const auto original_building = fixture.building;
    const auto original_module = fixture.module;
    const auto original_pawn = fixture.pawn;
    CityArray result{};
    for (int id = -1; id <= 130; ++id) {
      const bool expected =
          (id >= 100 && id <= 116) ||
          ((id == 2 || (id >= 4 && id <= 45)) && id != 13 && id != 19 &&
           id != 21 && id != 26 && id != 27 && id != 28);
      check_upgrade(city_upgrade_resource_cost(building, &result, id) ==
                        &result,
                    "resource getter returns hidden result pointer");
      check_upgrade(result.count == (expected ? 0 : 1),
                    "exact building-upgrade ID scope");
      check_upgrade(city_upgrade_wealth_cost(building, id) ==
                        (expected ? 0 : 23),
                    "wealth scope matches resource scope");
    }
    check_upgrade(fixture.building == original_building &&
                      fixture.module == original_module &&
                      fixture.pawn == original_pawn,
                  "UI cost queries do not mutate game data");
    const auto original_calls = upgrade_resource_calls;
    check_upgrade(city_upgrade_resource_cost(building, nullptr, 7) == nullptr &&
                      upgrade_resource_calls == original_calls + 1,
                  "null hidden result forwards safely to original mock");
    check_upgrade(city_upgrade_wealth_cost(nullptr, 7) == 23,
                  "invalid/null building uses original price");
    upgrade_put(fixture.pawn, 0x34d, std::uint8_t{1});
    check_upgrade(city_upgrade_wealth_cost(building, 7) == 23,
                  "AI ownership never receives free upgrade");
    upgrade_put(fixture.pawn, 0x34d, std::uint8_t{0});
    auto incomplete = upgrade_test_api();
    incomplete.finish = nullptr;
    configure_upgrade(incomplete);
    check_upgrade(city_upgrade_wealth_cost(building, 7) == 23,
                  "missing finalizer disables free costs");

    for (const auto [ai, id] :
         {std::pair{true, 7}, std::pair{false, 13}, std::pair{false, 19},
          std::pair{false, 21}, std::pair{false, 26}, std::pair{false, 27},
          std::pair{false, 28}}) {
      reset_upgrade_fixture_state();
      fixture.building = original_building;
      fixture.module = original_module;
      upgrade_put(fixture.pawn, 0x34d, static_cast<std::uint8_t>(ai));
      city_begin_upgrade(building, id);
      check_upgrade(
          upgrade_begins == 1 && upgrade_setups == 1 &&
              upgrade_resource_calls == 1 && upgrade_wealth_calls == 1 &&
              upgrade_finishes == 0 && upgrade_pending_count() == 0,
          "AI and animal actions execute native costs and no mod completion");
    }
    fixture.building = original_building;
    fixture.pawn = original_pawn;

    reset_upgrade_fixture_state();
    city_begin_upgrade(building, 7);
    check_upgrade(
        upgrade_events == std::vector<int>({10, 20, 21, 11, 60, 61, 62}),
        "finish occurs only after original BeginUpgrade fully returns");
    check_upgrade(upgrade_begins == 1 && upgrade_setups == 1 &&
                      upgrade_finishes == 1 && upgrade_pending_count() == 0 &&
                      upgrade_failed_count() == 0,
                  "synchronous completion exactly once and pending cleaned");
    check_upgrade(upgrade_work_argument == 8.0F &&
                      upgrade_visual_argument == 1.0F &&
                      upgrade_visual_material == 1,
                  "native work remainder and visual ABI values are correct");
    auto expected_module = original_module;
    upgrade_put(expected_module, 0x314, 10.0F);
    check_upgrade(fixture.building == original_building &&
                      fixture.module == expected_module &&
                      fixture.pawn == original_pawn,
                  "only mocked native construction work changed; "
                  "position/family/inventory preserved");

    reset_upgrade_fixture_state();
    fixture.module = original_module;
    upgrade_mode = UpgradeMode::Async;
    city_begin_upgrade(building, 7);
    check_upgrade(upgrade_finishes == 0 && upgrade_pending_count() == 1,
                  "asynchronous type replacement not completed prematurely");
    UpgradeCapture capture{building, 7};
    city_upgrade_type_callback(&capture);
    check_upgrade(upgrade_events ==
                      std::vector<int>({10, 11, 30, 20, 21, 31, 60, 61, 62}),
                  "asynchronous type callback fully returns before completion");
    city_upgrade_cost_setup(building, 7, 1);
    check_upgrade(upgrade_finishes == 1 && upgrade_pending_count() == 0,
                  "duplicate native setup cannot finalize twice");

    reset_upgrade_fixture_state();
    fixture.module = original_module;
    city_change_artisan(building, 3);
    check_upgrade(upgrade_events == std::vector<int>({50, 40, 10, 20, 21, 11,
                                                      41, 51, 60, 61, 62}),
                  "nested artisan/extension/begin scopes defer to outermost "
                  "native return");

    reset_upgrade_fixture_state();
    fixture.module = original_module;
    upgrade_mode = UpgradeMode::WrongId;
    city_begin_upgrade(building, 7);
    check_upgrade(upgrade_finishes == 0 && upgrade_pending_count() == 1,
                  "mismatched upgrade id cannot authorize completion");
    reset_upgrades();
    check_upgrade(upgrade_pending_count() == 0,
                  "unload clears pending actions");

    reset_upgrade_fixture_state();
    fixture.module = original_module;
    upgrade_mode = UpgradeMode::Async;
    city_begin_upgrade(building, 7);
    upgrade_weak_objects.begin()->second = nullptr;
    city_upgrade_type_callback(&capture);
    check_upgrade(upgrade_finishes == 0, "destroyed or serial-recycled object "
                                         "never receives pending completion");
    UpgradeFixture other;
    city_begin_upgrade(other.building.data(), 7);
    check_upgrade(upgrade_pending_count() == 1,
                  "dead weak entries pruned before new action");

    reset_upgrade_fixture_state();
    fixture.building = original_building;
    fixture.module = original_module;
    upgrade_mode = UpgradeMode::Complete;
    city_begin_upgrade(building, 22);
    check_upgrade(upgrade_finishes == 0 && upgrade_pending_count() == 0,
                  "already native-complete upgrades are not finalized again");

    for (int empty_modules = 0; empty_modules < 2; ++empty_modules) {
      reset_upgrade_fixture_state();
      fixture.building = original_building;
      fixture.module = original_module;
      if (empty_modules != 0)
        upgrade_put(fixture.building, 0x2f8, CityArray{});
      else {
        upgrade_put(fixture.module, 0x314, 0.0F);
        upgrade_put(fixture.module, 0x318, 0.0F);
      }
      const auto module_before = fixture.module;
      city_begin_upgrade(building, 17);
      check_upgrade(
          upgrade_finishes == 1 && upgrade_pending_count() == 0 &&
              upgrade_failed_count() == 0,
          "zero-module or zero-work valid upgrade uses native completion");
      check_upgrade(
          upgrade_work_argument == 0.0F && upgrade_visual_argument == 1.0F &&
              upgrade_events == std::vector<int>({10, 20, 21, 11, 60, 61, 62}),
          "zero remaining work retains native add/visual/finalizer order");
      check_upgrade(
          fixture.module == module_before && fixture.pawn == original_pawn &&
              fixture.building[0x3c1] == 1 && fixture.building[0x3a1] == 0,
          "zero-work completion keeps unrelated state unchanged");
    }

    for (int invalid = 0; invalid < 8; ++invalid) {
      reset_upgrade_fixture_state();
      fixture.building = original_building;
      fixture.module = original_module;
      if (invalid == 0)
        upgrade_put(fixture.module, 0x318,
                    std::numeric_limits<float>::quiet_NaN());
      else if (invalid == 1)
        upgrade_put(fixture.module, 0x314, 11.0F);
      else if (invalid == 2)
        upgrade_put(fixture.module, 0x318, 0.0F);
      else if (invalid == 3)
        upgrade_put(fixture.building, 0x2f8, CityArray{nullptr, 1, 1});
      else if (invalid == 4)
        upgrade_put(fixture.module, 8, std::uint32_t{0x40000000});
      else if (invalid == 5)
        upgrade_put(fixture.module, 0x318, -1.0F);
      else if (invalid == 6)
        upgrade_put(fixture.module, 0x314, -1.0F);
      else
        upgrade_put(fixture.building, 0x2f8, CityArray{nullptr, -1, 0});
      city_begin_upgrade(building, 7);
      check_upgrade(
          upgrade_finishes == 0 && upgrade_failed_count() == 1 &&
              upgrade_pending_count() == 0,
          "invalid completion state fails closed without forcing flags");
    }

    reset_upgrade_fixture_state();
    fixture.building = original_building;
    fixture.module = original_module;
    upgrade_reenter_finish = true;
    city_begin_upgrade(building, 7);
    check_upgrade(upgrade_finishes == 1 && upgrade_pending_count() == 0,
                  "reentrant setup cannot repeat a finalizer");

    reset_upgrade_fixture_state();
    fixture.building = original_building;
    fixture.module = original_module;
    upgrade_mode = UpgradeMode::Throw;
    bool caught{};
    try {
      city_begin_upgrade(building, 7);
    } catch (const std::runtime_error &) {
      caught = true;
    }
    check_upgrade(caught && upgrade_finishes == 0 &&
                      upgrade_pending_count() == 0,
                  "native exceptions are not swallowed");
    reset_upgrade_fixture_state();
    city_begin_upgrade(building, 7);
    check_upgrade(upgrade_finishes == 1,
                  "TLS scopes unwind after native exceptions");
    run_upgrade_assembly_tests();
    run_house_rebuild_query_tests();
    run_upgrade_maturity_tests();
    run_reconstruction_tests();
    configure_upgrade({});
    upgrade_weak_objects.clear();
    city_image = previous_image;
    std::printf("Upgrade tests PASS %zu checks\n", upgrade_checks);
    return 0;
  } catch (const std::exception &error) {
    configure_upgrade({});
    upgrade_weak_objects.clear();
    city_image = previous_image;
    std::fprintf(stderr, "Upgrade tests FAIL after %zu checks: %s\n",
                 upgrade_checks, error.what());
    return 1;
  }
}
