#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "CityRuntime.hpp"
#include "Construction.hpp"

struct ConstructionObject {
  alignas(8) std::array<std::uint8_t, 0x1100> bytes{};

  template <class T> auto put(std::size_t offset, T value) -> void {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }
  template <class T> auto get(std::size_t offset) const -> T {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }
  auto data() -> void * { return bytes.data(); }
};

static std::function<void(void *, bool)> placement_body;
static std::vector<std::pair<void *, bool>> placement_calls;
static std::vector<std::pair<void *, int>> treasury_calls;
static std::vector<std::pair<void *, std::uint8_t>> initialization_calls;
static int construction_checks{};
static int construction_failures{};

static auto construction_check(bool passed, const char *description) -> void {
  ++construction_checks;
  if (!passed) {
    ++construction_failures;
    std::fprintf(stderr, "FAIL construction: %s\n", description);
  }
}

static auto record_placement(void *pawn, bool immediate) -> void {
  placement_calls.emplace_back(pawn, immediate);
  if (placement_body) {
    placement_body(pawn, immediate);
  }
}

static auto record_treasury(void *pawn, int amount) -> void {
  treasury_calls.emplace_back(pawn, amount);
}

static auto record_initialization(void *building) -> void {
  std::uint8_t flag{};
  city_read(reinterpret_cast<std::uintptr_t>(building) + 0x3c1, &flag,
            sizeof(flag));
  initialization_calls.emplace_back(building, flag);
}

auto run_construction_tests() -> int {
  construction_checks = 0;
  construction_failures = 0;
  placement_calls.clear();
  treasury_calls.clear();
  initialization_calls.clear();
  placement_body = {};
  city_image = 0x140000000;
  alignas(8) std::array<std::uintptr_t, 3> object_class{city_image + 0x1000, 0,
                                                        0};
  ConstructionObject player, second_player, ai, ordinary, building, ai_building;
  int index = 1;
  for (auto *object :
       {&player, &second_player, &ai, &ordinary, &building, &ai_building}) {
    object->put(0, city_image + 0x2000);
    object->put(12, index++);
    object->put(16, object_class.data());
  }
  player.put<std::uint8_t>(0x34c, 1);
  second_player.put<std::uint8_t>(0x34c, 1);
  ai.put<std::uint8_t>(0x34d, 1);
  building.put(0x2e0, player.data());
  ai_building.put(0x2e0, ai.data());
  const auto player_before = player.bytes;
  const auto ai_before = ai.bytes;

  configure_construction({});
  city_place_building(player.data(), false);
  city_construction_treasury(player.data(), -20);
  city_initialize_plot(building.data(), player.data());
  construction_check(placement_calls.empty() && treasury_calls.empty() &&
                         initialization_calls.empty(),
                     "unconfigured callbacks do not call null targets");
  construction_check(building.get<std::uint8_t>(0x3c1) == 0,
                     "unconfigured initializer does not modify building");

  configure_construction(
      {record_placement, record_treasury, record_initialization});
  city_place_building(player.data(), false);
  construction_check(placement_calls.size() == 1 &&
                         placement_calls.back() ==
                             std::pair{player.data(), true},
                     "player request calls native once with instant true");
  for (void *pawn : {ai.data(), ordinary.data(), static_cast<void *>(nullptr),
                     reinterpret_cast<void *>(1)}) {
    const auto count = placement_calls.size();
    city_place_building(pawn, false);
    construction_check(placement_calls.size() == count + 1 &&
                           placement_calls.back() == std::pair{pawn, false},
                       "non-player or invalid request remains native false");
  }
  city_place_building(ai.data(), true);
  construction_check(placement_calls.back().second,
                     "existing native AI instant flag is preserved");
  ai.put<std::uint8_t>(0x34c, 1);
  city_place_building(ai.data(), false);
  construction_check(!placement_calls.back().second,
                     "isAI overrides contradictory main-player flag");
  ai.put<std::uint8_t>(0x34c, 0);

  city_construction_treasury(player.data(), -100);
  construction_check(treasury_calls.back().second == -100,
                     "outside placement scope treasury debit is unchanged");
  bool nested = false;
  placement_body = [&](void *pawn, bool) {
    if (nested) {
      city_construction_treasury(player.data(), -30);
      city_construction_treasury(pawn, -40);
      return;
    }
    city_construction_treasury(pawn, -10);
    city_construction_treasury(pawn, 25);
    city_construction_treasury(second_player.data(), -50);
    nested = true;
    city_place_building(ai.data(), false);
    nested = false;
    city_construction_treasury(pawn, -60);
  };
  const auto debit_begin = treasury_calls.size();
  city_place_building(player.data(), false);
  const std::array<int, 6> expected_debits{0, 25, -50, -30, -40, 0};
  construction_check(treasury_calls.size() ==
                         debit_begin + expected_debits.size(),
                     "all nested native treasury calls occur once");
  for (std::size_t i = 0; i < expected_debits.size(); ++i) {
    construction_check(treasury_calls[debit_begin + i].second ==
                           expected_debits[i],
                       "nested AI masks outer scope then restores it");
  }

  placement_body = [&](void *pawn, bool) {
    city_construction_treasury(pawn, -70);
    throw std::runtime_error("native mock failure");
  };
  bool caught = false;
  try {
    city_place_building(player.data(), false);
  } catch (const std::runtime_error &) {
    caught = true;
  }
  construction_check(caught && treasury_calls.back().second == 0,
                     "native exception propagates after scoped debit");
  city_construction_treasury(player.data(), -80);
  construction_check(treasury_calls.back().second == -80,
                     "exception unwinding restores placement scope");
  placement_body = {};

  player.put<std::uint8_t>(0x5fc, 1);
  bool native_rejected = false;
  placement_body = [&](void *pawn, bool) {
    std::uint8_t blocked{};
    city_read(reinterpret_cast<std::uintptr_t>(pawn) + 0x5fc, &blocked, 1);
    native_rejected = blocked != 0;
  };
  city_place_building(player.data(), false);
  construction_check(native_rejected && player.get<std::uint8_t>(0x5fc) == 1,
                     "native placement checks remain active and untouched");
  player.put<std::uint8_t>(0x5fc, 0);
  placement_body = {};

  auto building_before = building.bytes;
  city_initialize_plot(building.data(), player.data());
  building_before[0x3c1] = 1;
  construction_check(building.bytes == building_before,
                     "fresh player plot changes only constructed byte");
  construction_check(initialization_calls.size() == 1 &&
                         initialization_calls.back().second == 1,
                     "native plot initialization sees constructed flag once");
  building.put<std::uint8_t>(0x3c1, 0);
  city_initialize_plot(building.data(), second_player.data());
  construction_check(initialization_calls.size() == 2 &&
                         initialization_calls.back().second == 0,
                     "different player does not change owned plot flag");
  city_initialize_plot(building.data(), nullptr);
  construction_check(initialization_calls.back().second == 0,
                     "missing caller pawn does not modify player-owned plot");
  city_initialize_plot(ai_building.data(), ai.data());
  construction_check(initialization_calls.back().second == 0,
                     "AI plot remains under construction");
  city_initialize_plot(nullptr, player.data());
  construction_check(initialization_calls.back().first == nullptr,
                     "invalid plot still forwards original exactly once");
  construction_check(player.bytes == player_before && ai.bytes == ai_before,
                     "all wrappers preserve player and AI object data");

  building.put<std::int32_t>(0x3b8, 98);
  player.put(0x840, building.data());
  auto manor_before = building.bytes;
  auto editing_player_before = player.bytes;
  std::uint8_t instant_flag = 0;
  city_enable_manor_instant(building.data(), &instant_flag);
  construction_check(
      instant_flag == 1,
      "active player Manor conversion enables local instant flag");
  construction_check(building.bytes == manor_before &&
                         player.bytes == editing_player_before,
                     "Manor helper modifies no building or player data");
  player.put<void *>(0x840, nullptr);
  instant_flag = 0;
  city_enable_manor_instant(building.data(), &instant_flag);
  construction_check(instant_flag == 0,
                     "Manor outside active planning remains unchanged");
  player.put(0x840, building.data());
  building.put<std::int32_t>(0x3b8, 3);
  city_enable_manor_instant(building.data(), &instant_flag);
  construction_check(instant_flag == 0,
                     "non-Manor building does not enable instant flag");
  city_enable_manor_instant(ai_building.data(), &instant_flag);
  construction_check(instant_flag == 0,
                     "AI building does not enable instant flag");
  building.put<std::int32_t>(0x3b8, 98);
  city_enable_manor_instant(building.data(), nullptr);
  city_enable_manor_instant(building.data(),
                            reinterpret_cast<std::uint8_t *>(1));
  construction_check(building.bytes == manor_before,
                     "unreadable flag pointer fails without modifying Manor");

  configure_construction({});
  city_image = 0;
  placement_body = {};
  std::printf("Construction: %d checks, %d failures\n", construction_checks,
              construction_failures);
  return construction_failures;
}
