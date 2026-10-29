#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include <Windows.h>

#include "CityRuntime.hpp"
#include "Retinue.hpp"

struct RetinueObject {
  alignas(8) std::array<std::uint8_t, 0x1800> data{};

  template <class T> auto put(std::size_t offset, T value) -> void {
    std::memcpy(data.data() + offset, &value, sizeof(value));
  }
  template <class T> auto get(std::size_t offset) const -> T {
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
  }
  auto pointer() -> void * { return data.data(); }
};

static int retinue_checks{};
static int retinue_failures{};
static int command_calls{};
static int initialize_calls{};
static int growth_calls{};
static std::uint64_t test_clock{};
static std::function<void(void *)> command_body;
static std::function<void(void *, void *)> initialize_body;
static bool growth_fails{};
static bool growth_throws{};
static std::vector<std::unique_ptr<void *[]>> grown_arrays;

static auto retinue_check(bool passed, const char *message) -> void {
  ++retinue_checks;
  if (!passed) {
    ++retinue_failures;
    std::fprintf(stderr, "FAIL retinue: %s\n", message);
  }
}

static auto mock_commands(void *master) -> void {
  ++command_calls;
  if (command_body) {
    command_body(master);
  }
}

static auto mock_initialize_squad(void *pawn, void *region) -> void {
  ++initialize_calls;
  if (initialize_body) {
    initialize_body(pawn, region);
    return;
  }
  const std::int32_t id = 1000 + initialize_calls;
  std::memcpy(static_cast<std::uint8_t *>(region) + 0x508, &id, sizeof(id));
}

static auto mock_grow(CityArray *array, std::int32_t old_count) -> void {
  ++growth_calls;
  retinue_check(array->count == old_count + 1,
                "ResizeGrow receives incremented Num and original count");
  if (growth_throws) {
    throw std::runtime_error("native allocation mock failed");
  }
  if (growth_fails) {
    return;
  }
  const auto capacity = old_count + 4;
  auto data = std::make_unique<void *[]>(static_cast<std::size_t>(capacity));
  if (old_count != 0) {
    std::memcpy(data.get(), array->data,
                static_cast<std::size_t>(old_count) * sizeof(void *));
  }
  array->data = data.get();
  array->capacity = capacity;
  grown_arrays.push_back(std::move(data));
}

static auto mock_clock() -> std::uint64_t { return test_clock; }

struct RetinueFixture {
  alignas(8) std::array<std::uintptr_t, 3> object_class{0x140001000, 0, 0};
  RetinueObject master, player, region, manor, palace, other, squad_manager;
  alignas(8) std::array<std::uint8_t, 0x410 * 3> squads{};
  std::array<void *, 3> regions{};
  std::array<void *, 3> modules{};
  std::array<void *, 8> requests{};

  RetinueFixture() {
    command_calls = initialize_calls = growth_calls = 0;
    test_clock = 1000;
    command_body = {};
    initialize_body = {};
    growth_fails = growth_throws = false;
    grown_arrays.clear();
    city_image = 0x140000000;
    std::int32_t index = 1;
    for (auto *object :
         {&master, &player, &region, &manor, &palace, &other, &squad_manager}) {
      object->put(0, city_image + 0x2000);
      object->put(12, index++);
      object->put(16, object_class.data());
    }
    player.put<std::uint8_t>(0x34c, 1);
    player.put(0x340, master.pointer());
    master.put<std::uint8_t>(0x318, 1);
    master.put(0x5b0, player.pointer());
    master.put(0x2d0, squad_manager.pointer());
    squad_manager.put(0xa0, CityArray{squads.data(), 0, 3});
    regions[0] = region.pointer();
    modules[0] = palace.pointer();
    master.put(0x560, CityArray{regions.data(), 1, 3});
    master.put(0xf98, CityArray{requests.data(), 0, 8});
    region.put(0x350, player.pointer());
    region.put(0x358, manor.pointer());
    manor.put(0x2c8, region.pointer());
    manor.put(0x2e0, player.pointer());
    manor.put(0x2e8, master.pointer());
    manor.put(0x2f8, CityArray{modules.data(), 1, 3});
    manor.put<std::int32_t>(0x3b8, 98);
    manor.put<std::uint8_t>(0x3c1, 1);
    // Actual 0.8.117 Gopsberg snapshot: building enum 98, runtime Palace
    // model 144, +0x312 still set, work completed at 200/200.
    palace.put<std::int32_t>(0x2c0, 144);
    palace.put(0x2c8, manor.pointer());
    palace.put<std::uint8_t>(0x312, 1);
    palace.put<float>(0x314, 200.0F);
    palace.put<float>(0x318, 200.0F);
    configure_retinue(
        {mock_commands, mock_initialize_squad, mock_grow, mock_clock});
  }

  ~RetinueFixture() {
    configure_retinue({});
    grown_arrays.clear();
    command_body = {};
    initialize_body = {};
    city_image = 0;
  }

  auto run() -> void { city_process_retinue_requests(master.pointer()); }
  auto expect_no_repair(const char *message) -> void {
    run();
    retinue_check(command_calls == 1 && initialize_calls == 0 &&
                      growth_calls == 0 && city_retinue_repair_count() == 0,
                  message);
  }
};

auto run_retinue_tests() -> int {
  retinue_checks = retinue_failures = 0;
  {
    RetinueFixture f;
    const auto manor_before = f.manor.data;
    const auto palace_before = f.palace.data;
    const auto player_before = f.player.data;
    const auto master_before = f.master.data;
    auto region_expected = f.region.data;
    f.run();
    const std::int32_t id = 1001;
    std::memcpy(region_expected.data() + 0x508, &id, sizeof(id));
    auto master_expected = master_before;
    const std::int32_t queue_count = 1;
    std::memcpy(master_expected.data() + 0xfa0, &queue_count,
                sizeof(queue_count));
    retinue_check(command_calls == 1 && initialize_calls == 1 &&
                      growth_calls == 0,
                  "repair runs native processor and initializer once");
    retinue_check(f.region.data == region_expected &&
                      f.master.data == master_expected,
                  "only native region ID and pending queue count are modified");
    retinue_check(
        f.requests[0] == f.manor.pointer() && city_retinue_repair_count() == 1,
        "exact Manor is queued once for native initial five recruits");
    retinue_check(
        f.manor.data == manor_before && f.palace.data == palace_before &&
            f.player.data == player_before,
        "Manor/module/player data and other completion rewards unchanged");
    f.run();
    retinue_check(command_calls == 2 && initialize_calls == 1,
                  "throttling never skips original command processing");
    test_clock += 1000;
    f.master.put(0xf98, CityArray{f.requests.data(), 0, 8});
    f.run();
    retinue_check(
        initialize_calls == 1 && f.master.get<CityArray>(0xf98).count == 0,
        "positive squad ID prevents replenishing dead or empty retinue");
  }
  for (std::int32_t model : {144, 497, 419}) {
    RetinueFixture f;
    f.palace.put(0x2c0, model);
    f.palace.put<float>(0x314, 0);
    f.palace.put<float>(0x318, 0);
    f.run();
    retinue_check(initialize_calls == 1,
                  "current Palace runtime models and legal zero-work "
                  "completed modules qualify");
  }
  {
    RetinueFixture f;
    f.master.put<std::uint8_t>(0x318, 0);
    f.expect_no_repair("partial save loading is excluded");
  }
  {
    RetinueFixture f;
    f.player.put<std::uint8_t>(0x34d, 1);
    f.expect_no_repair("AI is excluded even if main-player flag is also set");
  }
  {
    RetinueFixture f;
    f.master.put(0x5b0, f.other.pointer());
    f.expect_no_repair("non-player canonical pawn is excluded");
  }
  {
    RetinueFixture f;
    f.player.put(0x340, f.other.pointer());
    f.expect_no_repair("canonical player's master backlink must match");
  }
  for (std::size_t offset : {0x350U, 0x358U}) {
    RetinueFixture f;
    f.region.put(offset, f.other.pointer());
    f.expect_no_repair("non-player region or noncanonical Manor is excluded");
  }
  for (std::size_t offset : {0x2c8U, 0x2e0U, 0x2e8U}) {
    RetinueFixture f;
    f.manor.put(offset, f.other.pointer());
    f.expect_no_repair("Manor region/player/master backlinks must match");
  }
  for (std::size_t offset : {0x388U, 0x3a0U, 0x3a1U, 0x3a2U, 0x3a3U}) {
    RetinueFixture f;
    f.manor.put<std::uint8_t>(offset, 1);
    f.expect_no_repair("fire, demolition and upgrade transitions are excluded");
  }
  {
    RetinueFixture f;
    f.manor.put<std::uint8_t>(0x3c1, 0);
    f.expect_no_repair("unfinished Manor is excluded");
  }
  for (std::int32_t id : {-1, 1, 200}) {
    RetinueFixture f;
    f.region.put(0x508, id);
    f.expect_no_repair("only exact native zero squad sentinel is repaired");
  }
  for (std::int32_t id : {-1, 0, 333}) {
    RetinueFixture f;
    f.squad_manager.put(0xa0, CityArray{f.squads.data(), 1, 3});
    f.squads[0x10] = 3;
    std::memcpy(f.squads.data() + 0x64, &id, sizeof(id));
    void *owner = f.player.pointer();
    void *origin = f.region.pointer();
    std::memcpy(f.squads.data() + 0x3a8, &owner, sizeof(owner));
    std::memcpy(f.squads.data() + 0x1f8, &origin, sizeof(origin));
    const auto squads_before = f.squads;
    f.expect_no_repair(
        "matching orphan retinue must not create duplicate squad");
    retinue_check(
        city_retinue_conflict_count() == 1 && f.squads == squads_before &&
            f.region.get<std::int32_t>(0x508) == 0,
        "orphan with zero members or invalid ID is untouched and reported");
  }
  for (int mismatch : {0, 1, 2}) {
    RetinueFixture f;
    f.squad_manager.put(0xa0, CityArray{f.squads.data(), 1, 3});
    f.squads[0x10] = mismatch == 0 ? 1 : 3;
    void *owner = mismatch == 1 ? f.other.pointer() : f.player.pointer();
    void *origin = mismatch == 2 ? f.other.pointer() : f.region.pointer();
    std::memcpy(f.squads.data() + 0x3a8, &owner, sizeof(owner));
    std::memcpy(f.squads.data() + 0x1f8, &origin, sizeof(origin));
    f.run();
    retinue_check(
        initialize_calls == 1 && city_retinue_conflict_count() == 0,
        "other squad type, owner or origin does not block missing retinue");
  }
  {
    RetinueFixture f;
    f.master.put<void *>(0x2d0, nullptr);
    f.expect_no_repair("missing SquadManager prevents unsafe initialization");
  }
  {
    RetinueFixture f;
    f.master.put(0x2d0, reinterpret_cast<void *>(1));
    f.expect_no_repair("unreadable SquadManager fails closed");
  }
  {
    RetinueFixture f;
    f.region.put<std::int32_t>(0x508, 7);
    f.master.put(0x2d0, reinterpret_cast<void *>(1));
    f.expect_no_repair(
        "existing positive region ID is skipped before manager inspection");
    retinue_check(city_retinue_conflict_count() == 0,
                  "positive ID does not count as an orphan conflict");
  }
  for (CityArray invalid :
       {CityArray{nullptr, 1, 1}, CityArray{reinterpret_cast<void *>(1), 1, 1},
        CityArray{nullptr, -1, 0}, CityArray{nullptr, 0, -1},
        CityArray{nullptr, 1, 0},
        CityArray{nullptr, 0, std::numeric_limits<std::int32_t>::max()}}) {
    RetinueFixture f;
    f.squad_manager.put(0xa0, invalid);
    f.expect_no_repair(
        "invalid FSquad arrays cannot be treated as absence of squads");
  }
  {
    RetinueFixture f;
    f.manor.put(0x2f8, CityArray{});
    f.expect_no_repair("empty preconstructed planner cannot grant retinue");
  }
  for (std::int32_t model : {98, 474, 81, 100, 167, 498, 444, 470, 516, 520}) {
    RetinueFixture f;
    f.palace.put(0x2c0, model);
    f.expect_no_repair(
        "building enum IDs, stairs, walls and towers are not Palace models");
  }
  {
    RetinueFixture f;
    f.palace.put(0x2c8, f.other.pointer());
    f.expect_no_repair("foreign Palace module is excluded");
  }
  for (float work : {-1.0F, 199.0F, std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::quiet_NaN()}) {
    RetinueFixture f;
    f.palace.put(0x314, work);
    f.expect_no_repair("incomplete or invalid Palace progress is excluded");
  }
  {
    RetinueFixture f;
    f.palace.put<float>(0x318, -1);
    f.expect_no_repair("negative required work is invalid");
  }
  {
    RetinueFixture f;
    f.requests[0] = f.manor.pointer();
    f.master.put(0xf98, CityArray{f.requests.data(), 1, 8});
    f.expect_no_repair(
        "already queued Manor is not initialized or queued twice");
  }
  {
    RetinueFixture f;
    f.master.put(0xf98, CityArray{});
    initialize_body = [&](void *, void *region) {
      retinue_check(
          f.master.get<CityArray>(0xf98).count == 0,
          "preallocation restores native queue count before initialization");
      const std::int32_t id = 111;
      std::memcpy(static_cast<std::uint8_t *>(region) + 0x508, &id, sizeof(id));
    };
    f.run();
    const auto queue = f.master.get<CityArray>(0xf98);
    retinue_check(growth_calls == 1 && initialize_calls == 1 &&
                      queue.count == 1 &&
                      static_cast<void **>(queue.data)[0] == f.manor.pointer(),
                  "empty native queue is grown and receives one request");
  }
  for (bool throwing : {false, true}) {
    RetinueFixture f;
    f.master.put(0xf98, CityArray{});
    growth_fails = !throwing;
    growth_throws = throwing;
    f.run();
    retinue_check(
        growth_calls == 1 && initialize_calls == 0 &&
            f.master.get<CityArray>(0xf98).count == 0,
        "failed native allocation restores Num and never creates empty squad");
  }
  {
    RetinueFixture f;
    f.requests[0] = f.other.pointer();
    f.master.put(0xf98, CityArray{f.requests.data(), 1, 1});
    f.run();
    const auto queue = f.master.get<CityArray>(0xf98);
    retinue_check(growth_calls == 1 && queue.count == 2 &&
                      static_cast<void **>(queue.data)[0] ==
                          f.other.pointer() &&
                      static_cast<void **>(queue.data)[1] == f.manor.pointer(),
                  "grow preserves all existing native request entries");
  }
  {
    RetinueFixture f;
    f.regions[1] = f.region.pointer();
    f.master.put(0x560, CityArray{f.regions.data(), 2, 3});
    f.run();
    retinue_check(initialize_calls == 1 &&
                      f.master.get<CityArray>(0xf98).count == 1,
                  "duplicate region references still generate only one squad");
  }
  for (CityArray invalid :
       {CityArray{nullptr, 1, 1}, CityArray{reinterpret_cast<void *>(1), 1, 1},
        CityArray{nullptr, -1, 0}, CityArray{nullptr, 0, -1},
        CityArray{nullptr, 1, 0},
        CityArray{nullptr, 0, std::numeric_limits<std::int32_t>::max()}}) {
    RetinueFixture f;
    f.master.put(0x560, invalid);
    f.expect_no_repair("invalid region arrays fail closed without allocation");
  }
  {
    RetinueFixture f;
    f.master.put(0xf98, CityArray{reinterpret_cast<void *>(1), 1, 1});
    f.expect_no_repair("unreadable request queue fails closed");
  }
  {
    RetinueFixture f;
    initialize_body = [](void *, void *) {};
    f.run();
    retinue_check(
        initialize_calls == 1 && f.master.get<CityArray>(0xf98).count == 0 &&
            city_retinue_repair_count() == 0,
        "unsuccessful native initialization does not enqueue unusable squad");
  }
  {
    RetinueFixture f;
    initialize_body = [&](void *, void *region) {
      city_process_retinue_requests(f.master.pointer());
      const std::int32_t id = 222;
      std::memcpy(static_cast<std::uint8_t *>(region) + 0x508, &id, sizeof(id));
    };
    f.run();
    retinue_check(command_calls == 2 && initialize_calls == 1 &&
                      f.master.get<CityArray>(0xf98).count == 1,
                  "reentrant native callback runs original but does not "
                  "recursively repair");
  }
  {
    RetinueFixture f;
    f.manor.put<std::uint8_t>(0x3c1, 0);
    f.run();
    f.manor.put<std::uint8_t>(0x3c1, 1);
    test_clock += 999;
    f.run();
    retinue_check(initialize_calls == 0 && command_calls == 2,
                  "same-world scans are limited without suppressing commands");
    ++test_clock;
    f.run();
    retinue_check(
        initialize_calls == 1,
        "previously missing completed Manor is repaired on the next scan");
  }
  {
    RetinueFixture f;
    configure_retinue({mock_commands, nullptr, nullptr, nullptr});
    f.expect_no_repair("partial API configuration still calls original once");
    configure_retinue({});
    f.run();
    retinue_check(
        command_calls == 1,
        "fully unconfigured callback cannot call a null native target");
  }
  std::printf("Retinue: %d checks, %d failures\n", retinue_checks,
              retinue_failures);
  return retinue_failures;
}
