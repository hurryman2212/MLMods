#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "CityRuntime.hpp"
#include "Windmill.hpp"

struct WindmillObject {
  alignas(8) std::array<std::uint8_t, 0x1100> bytes{};

  template <class T> auto put(std::size_t offset, T value) -> void {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }
  template <class T> auto get(std::size_t offset) const -> T {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }
  auto pointer() -> void * { return bytes.data(); }
};

static int windmill_checks{};
static int windmill_failures{};
static int windmill_find_calls{};
static std::uint64_t windmill_clock{};
static void *windmill_found_component{};
static std::function<void(void *)> windmill_find_body;
static std::function<void(void *, bool)> windmill_activate_body;
static std::vector<std::pair<void *, bool>> windmill_activate_calls;

static auto windmill_check(bool passed, const char *message) -> void {
  ++windmill_checks;
  if (!passed) {
    ++windmill_failures;
    std::fprintf(stderr, "FAIL windmill: %s\n", message);
  }
}

static auto mock_find_windmill(void *building) -> void * {
  ++windmill_find_calls;
  if (windmill_find_body) {
    windmill_find_body(building);
  }
  return windmill_found_component;
}

static auto mock_activate_windmill(void *component, bool activate) -> void {
  windmill_activate_calls.emplace_back(component, activate);
  if (windmill_activate_body) {
    windmill_activate_body(component, activate);
    return;
  }
  void *shaft{};
  std::memcpy(&shaft, static_cast<std::uint8_t *>(component) + 0xb0,
              sizeof(shaft));
  const std::uint64_t timer = 0x100000001;
  std::memcpy(static_cast<std::uint8_t *>(component) + 0xb8, &timer,
              sizeof(timer));
  static_cast<std::uint8_t *>(shaft)[0x188] |= 0x20;
}

static auto mock_windmill_clock() -> std::uint64_t { return windmill_clock; }

struct WindmillFixture {
  alignas(8) std::array<std::uintptr_t, 3> object_class{0x140001000, 0, 0};
  WindmillObject master, player, owner, base, roof, component, shaft, root,
      other;
  std::array<void *, 4> buildings{};
  std::array<void *, 4> modules{};

  WindmillFixture() {
    windmill_find_calls = 0;
    windmill_clock = 1000;
    windmill_find_body = {};
    windmill_activate_body = {};
    windmill_activate_calls.clear();
    city_image = 0x140000000;
    std::int32_t index = 1;
    for (auto *object : {&master, &player, &owner, &base, &roof, &component,
                         &shaft, &root, &other}) {
      object->put(0, city_image + 0x2000);
      object->put(12, index++);
      object->put(16, object_class.data());
    }
    player.put<std::uint8_t>(0x34c, 1);
    player.put(0x340, master.pointer());
    master.put<std::uint8_t>(0x318, 1);
    master.put(0x5b0, player.pointer());
    buildings[0] = owner.pointer();
    master.put(0x508, CityArray{buildings.data(), 1, 4});
    owner.put(0, city_image + 0x7f51b90);
    owner.put(0x1b8, root.pointer());
    owner.put(0x2e0, player.pointer());
    owner.put(0x2e8, master.pointer());
    owner.put<std::int32_t>(0x3b8, 20);
    owner.put<std::uint8_t>(0x3c1, 1);
    modules[0] = base.pointer();
    modules[1] = roof.pointer();
    owner.put(0x2f8, CityArray{modules.data(), 2, 4});
    // Captured 0.8.117 owner: constructed Windmill, runtime models 43/458,
    // completed work at 200/200 even though +0x312 remains set.
    base.put<std::int32_t>(0x2c0, 43);
    roof.put<std::int32_t>(0x2c0, 458);
    for (auto *module : {&base, &roof}) {
      module->put(0, city_image + 0x7f51448);
      module->put(0x2c8, owner.pointer());
      module->put<std::uint8_t>(0x312, 1);
      module->put<std::uint8_t>(0x313, 1);
      module->put<float>(0x314, 200.0F);
      module->put<float>(0x318, 200.0F);
    }
    component.put(0, city_image + 0x7f72d30);
    component.put(0x20, owner.pointer());
    component.put(0x90, owner.pointer());
    component.put(0xa0, master.pointer());
    component.put(0xa8, owner.pointer());
    component.put(0xb0, shaft.pointer());
    shaft.put(0, city_image + 0x79980e0);
    shaft.put(0x20, component.pointer());
    shaft.put(0x90, owner.pointer());
    shaft.put(0xb0, root.pointer());
    shaft.put<std::uint8_t>(0x188, 0x41);
    windmill_found_component = component.pointer();
    configure_windmill(
        {mock_find_windmill, mock_activate_windmill, mock_windmill_clock});
  }

  ~WindmillFixture() {
    configure_windmill({});
    windmill_find_body = {};
    windmill_activate_body = {};
    windmill_found_component = nullptr;
    city_image = 0;
  }

  auto run() -> void { city_repair_windmills(master.pointer()); }

  auto expect_no_repair(const char *message) -> void {
    const auto master_before = master.bytes;
    const auto player_before = player.bytes;
    const auto owner_before = owner.bytes;
    const auto base_before = base.bytes;
    const auto roof_before = roof.bytes;
    const auto component_before = component.bytes;
    const auto shaft_before = shaft.bytes;
    const auto root_before = root.bytes;
    run();
    windmill_check(
        windmill_activate_calls.empty() && city_windmill_repair_count() == 0 &&
            master.bytes == master_before && player.bytes == player_before &&
            owner.bytes == owner_before && base.bytes == base_before &&
            roof.bytes == roof_before && component.bytes == component_before &&
            shaft.bytes == shaft_before && root.bytes == root_before,
        message);
  }
};

auto run_windmill_tests() -> int {
  windmill_checks = windmill_failures = 0;
  {
    WindmillFixture f;
    const auto master_before = f.master.bytes;
    const auto player_before = f.player.bytes;
    const auto owner_before = f.owner.bytes;
    const auto base_before = f.base.bytes;
    const auto roof_before = f.roof.bytes;
    auto component_expected = f.component.bytes;
    auto shaft_expected = f.shaft.bytes;
    const std::uint64_t timer = 0x100000001;
    std::memcpy(component_expected.data() + 0xb8, &timer, sizeof(timer));
    shaft_expected[0x188] |= 0x20;
    f.run();
    windmill_check(
        windmill_find_calls == 1 &&
            windmill_activate_calls ==
                std::vector{std::pair{f.component.pointer(), true}} &&
            city_windmill_repair_count() == 1,
        "captured completed windmill invokes native activation once");
    windmill_check(f.component.bytes == component_expected &&
                       f.shaft.bytes == shaft_expected,
                   "native callback supplies shaft visibility and timer");
    windmill_check(
        f.master.bytes == master_before && f.player.bytes == player_before &&
            f.owner.bytes == owner_before && f.base.bytes == base_before &&
            f.roof.bytes == roof_before,
        "repair preserves owner, module, player and master bytes");
    f.run();
    windmill_clock += 1000;
    f.run();
    windmill_check(
        windmill_activate_calls.size() == 1 &&
            city_windmill_repair_count() == 1 &&
            f.component.bytes == component_expected,
        "repeat scans preserve the native timer without reactivation");
  }
  for (std::uint8_t ready : {0, 2}) {
    WindmillFixture f;
    f.master.put(0x318, ready);
    f.expect_no_repair("unready or malformed master state is excluded");
  }
  for (std::uint8_t constructed : {0, 2}) {
    WindmillFixture f;
    f.owner.put(0x3c1, constructed);
    f.expect_no_repair("unfinished or malformed constructed flag is excluded");
  }
  {
    WindmillFixture f;
    f.player.put<std::uint8_t>(0x34c, 0);
    f.expect_no_repair("non-player ownership is excluded");
  }
  {
    WindmillFixture f;
    f.player.put<std::uint8_t>(0x34d, 1);
    f.expect_no_repair("AI ownership overrides a contradictory player flag");
  }
  {
    WindmillFixture f;
    f.master.put(0x5b0, f.other.pointer());
    f.expect_no_repair("canonical main player must be a valid player pawn");
  }
  {
    WindmillFixture f;
    f.player.put(0x340, f.other.pointer());
    f.expect_no_repair("canonical player must point back to this master");
  }
  for (std::size_t offset : {0x2e0U, 0x2e8U}) {
    WindmillFixture f;
    f.owner.put(offset, f.other.pointer());
    f.expect_no_repair("owner must belong to the canonical player and master");
  }
  for (std::size_t offset : {0x388U, 0x3a0U, 0x3a1U, 0x3a2U, 0x3a3U}) {
    WindmillFixture f;
    f.owner.put<std::uint8_t>(offset, 1);
    f.expect_no_repair("fire, demolition and upgrade transitions are excluded");
  }
  for (std::int32_t type : {0, 19, 21, 43, 98, 458}) {
    WindmillFixture f;
    f.owner.put(0x3b8, type);
    f.expect_no_repair("other building and runtime model IDs are excluded");
  }
  for (std::size_t offset : {0x20U, 0xa0U, 0xa8U}) {
    WindmillFixture f;
    f.component.put(offset, f.other.pointer());
    f.expect_no_repair("component ownership and master backlinks must match");
  }
  for (std::size_t offset : {0x20U, 0xb0U}) {
    WindmillFixture f;
    f.shaft.put(offset, f.other.pointer());
    f.expect_no_repair("shaft must belong to this component and building root");
  }
  for (void *invalid :
       {static_cast<void *>(nullptr), reinterpret_cast<void *>(1)}) {
    WindmillFixture f;
    f.component.put(0xb0, invalid);
    f.expect_no_repair("missing or unreadable shaft is excluded");
    windmill_found_component = invalid;
    windmill_clock += 1000;
    f.expect_no_repair("missing or unreadable native component is excluded");
  }
  for (void *invalid :
       {static_cast<void *>(nullptr), reinterpret_cast<void *>(1)}) {
    WindmillFixture f;
    f.owner.put(0x1b8, invalid);
    f.expect_no_repair("missing or unreadable building root is excluded");
  }
  for (std::size_t target : {0U, 1U, 2U, 3U, 4U, 5U}) {
    for (std::uint32_t flags : {0x40000000U, 0x10000U, 0x8000U}) {
      WindmillFixture f;
      std::array objects{&f.master, &f.owner,     &f.base,
                         &f.roof,   &f.component, &f.shaft};
      objects[target]->put(8, flags);
      f.expect_no_repair("destroyed or garbage UObject headers fail closed");
    }
  }
  for (std::size_t target : {0U, 1U, 2U}) {
    WindmillFixture f;
    std::array objects{&f.owner, &f.component, &f.shaft};
    objects[target]->put(0, city_image + 0x2000);
    f.expect_no_repair(
        "wrong concrete owner, component or shaft class is excluded");
  }
  for (std::size_t target : {0U, 1U, 2U}) {
    for (std::uint8_t actor_flags : {1, 3}) {
      WindmillFixture f;
      std::array objects{&f.owner, &f.base, &f.roof};
      objects[target]->put(0x5d, actor_flags);
      f.expect_no_repair("actors being destroyed cannot activate a mechanism");
    }
    WindmillFixture f;
    std::array objects{&f.owner, &f.base, &f.roof};
    objects[target]->put<std::uint8_t>(0x5d, 0xfe);
    const auto actor_before = objects[target]->bytes;
    f.run();
    windmill_check(windmill_activate_calls.size() == 1 &&
                       objects[target]->bytes == actor_before,
                   "unrelated actor flag bits are accepted and preserved");
  }
  {
    WindmillFixture f;
    f.owner.put(0x2f8, CityArray{});
    f.expect_no_repair("empty planner cannot activate a windmill mechanism");
  }
  for (std::size_t target : {0U, 1U}) {
    WindmillFixture f;
    std::array modules{&f.base, &f.roof};
    modules[target]->put<std::int32_t>(0x2c0, 20);
    f.expect_no_repair("both captured base and top models are required");
  }
  {
    WindmillFixture f;
    for (auto *module : {&f.base, &f.roof}) {
      module->put<float>(0x314, 0);
      module->put<float>(0x318, 0);
    }
    f.run();
    windmill_check(windmill_activate_calls.size() == 1,
                   "finite zero-work completed assemblies remain eligible");
  }
  {
    WindmillFixture f;
    f.other.put(0, city_image + 0x7f51448);
    f.other.put(0x2c8, f.owner.pointer());
    f.modules[2] = f.other.pointer();
    f.owner.put(0x2f8, CityArray{f.modules.data(), 3, 4});
    const auto auxiliary_before = f.other.bytes;
    f.run();
    windmill_check(windmill_activate_calls.size() == 1 &&
                       f.other.bytes == auxiliary_before,
                   "captured auxiliary model zero at zero work is preserved");
  }
  for (std::size_t target : {0U, 1U}) {
    for (float work : {-1.0F, 199.0F, std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::quiet_NaN()}) {
      WindmillFixture f;
      std::array modules{&f.base, &f.roof};
      modules[target]->put(0x314, work);
      f.expect_no_repair(
          "every windmill module must have finite completed work");
    }
    for (float required :
         {-1.0F, 201.0F, std::numeric_limits<float>::infinity(),
          std::numeric_limits<float>::quiet_NaN()}) {
      WindmillFixture f;
      std::array modules{&f.base, &f.roof};
      modules[target]->put(0x318, required);
      f.expect_no_repair("invalid or incomplete required work fails closed");
    }
    WindmillFixture f;
    std::array modules{&f.base, &f.roof};
    modules[target]->put(0x2c8, f.other.pointer());
    f.expect_no_repair("foreign module cannot qualify the windmill");
  }
  for (std::size_t target : {0U, 1U}) {
    WindmillFixture f;
    std::array modules{&f.base, &f.roof};
    modules[target]->put<std::uint8_t>(0x311, 1);
    f.expect_no_repair("modules being replaced cannot activate the mechanism");
  }
  for (std::size_t offset : {0x508U, 0x2f8U}) {
    for (CityArray invalid :
         {CityArray{nullptr, 1, 1},
          CityArray{reinterpret_cast<void *>(1), 1, 1},
          CityArray{nullptr, -1, 0}, CityArray{nullptr, 0, -1},
          CityArray{nullptr, 1, 0},
          CityArray{nullptr, 0, std::numeric_limits<std::int32_t>::max()}}) {
      WindmillFixture f;
      auto &object = offset == 0x508 ? f.master : f.owner;
      object.put(offset, invalid);
      f.expect_no_repair("malformed master and module arrays fail closed");
    }
  }
  {
    WindmillFixture f;
    f.master.put(0x508, CityArray{});
    f.master.put(0x560, CityArray{f.buildings.data(), 1, 4});
    f.expect_no_repair(
        "only the authoritative master building array is scanned");
  }
  {
    WindmillFixture f;
    f.buildings[0] = reinterpret_cast<void *>(1);
    f.buildings[1] = nullptr;
    f.buildings[2] = f.other.pointer();
    f.buildings[3] = f.owner.pointer();
    f.master.put(0x508, CityArray{f.buildings.data(), 4, 4});
    f.run();
    windmill_check(
        windmill_activate_calls.size() == 1,
        "unreadable and unrelated entries do not hide a valid owner");
  }
  {
    WindmillFixture f;
    f.buildings[1] = f.owner.pointer();
    f.master.put(0x508, CityArray{f.buildings.data(), 2, 4});
    f.run();
    windmill_check(windmill_activate_calls.size() == 1 &&
                       city_windmill_repair_count() == 1,
                   "duplicate owner entries cannot restart the native timer");
  }
  for (bool visible : {false, true}) {
    WindmillFixture f;
    if (visible) {
      f.shaft.put<std::uint8_t>(0x188, 0x61);
    } else {
      f.component.put<std::uint64_t>(0xb8, 0x100000001);
    }
    f.expect_no_repair("already visible or active mechanism remains untouched");
  }
  {
    WindmillFixture f;
    f.owner.put<std::uint8_t>(0x3c1, 0);
    f.run();
    f.owner.put<std::uint8_t>(0x3c1, 1);
    windmill_clock += 999;
    f.run();
    windmill_check(windmill_activate_calls.empty(),
                   "same-world scans are throttled below one second");
    ++windmill_clock;
    f.run();
    windmill_check(windmill_activate_calls.size() == 1,
                   "completed windmill is repaired at the next scan interval");
  }
  for (bool new_address : {false, true}) {
    WindmillFixture f;
    f.owner.put<std::uint8_t>(0x3c1, 0);
    f.run();
    f.owner.put<std::uint8_t>(0x3c1, 1);
    if (new_address) {
      f.other.bytes = f.master.bytes;
      f.player.put(0x340, f.other.pointer());
      f.owner.put(0x2e8, f.other.pointer());
      f.component.put(0xa0, f.other.pointer());
      city_repair_windmills(f.other.pointer());
    } else {
      f.master.put<std::int32_t>(12, 100);
      f.run();
    }
    windmill_check(
        windmill_activate_calls.size() == 1,
        "new master identity is scanned without old-world throttling");
  }
  {
    WindmillFixture f;
    f.owner.put<std::uint8_t>(0x3c1, 0);
    f.run();
    f.owner.put<std::uint8_t>(0x3c1, 1);
    windmill_clock = 10;
    f.run();
    windmill_check(windmill_activate_calls.size() == 1,
                   "clock rollback cannot suppress repair indefinitely");
  }
  {
    WindmillFixture f;
    windmill_activate_body = [&](void *, bool) {
      windmill_clock += 1000;
      f.run();
      f.shaft.put<std::uint8_t>(0x188, 0x61);
      f.component.put<std::uint64_t>(0xb8, 0x100000001);
    };
    f.run();
    windmill_check(windmill_activate_calls.size() == 1 &&
                       city_windmill_repair_count() == 1,
                   "reentrant native activation cannot recursively repair");
  }
  for (int failure : {0, 1, 2}) {
    WindmillFixture f;
    const auto owner_before = f.owner.bytes;
    windmill_activate_body = [&](void *, bool) {
      if (failure == 1) {
        f.shaft.put<std::uint8_t>(0x188, 0x61);
      } else if (failure == 2) {
        f.component.put<std::uint64_t>(0xb8, 0x100000001);
      }
    };
    f.run();
    windmill_check(
        windmill_activate_calls.size() == 1 &&
            city_windmill_repair_count() == 0 && f.owner.bytes == owner_before,
        "failed or partial native activation is not reported as repaired");
  }
  {
    WindmillFixture f;
    windmill_find_body = [&](void *building) {
      windmill_check(building == f.owner.pointer(),
                     "native lookup receives the authoritative windmill owner");
      f.owner.put<std::uint8_t>(0x388, 1);
    };
    f.run();
    windmill_check(
        windmill_find_calls == 1 && windmill_activate_calls.empty() &&
            city_windmill_repair_count() == 0,
        "eligibility is revalidated after native lookup changes state");
  }
  for (bool lookup_throws : {false, true}) {
    WindmillFixture f;
    if (lookup_throws) {
      windmill_find_body = [](void *) {
        throw std::runtime_error("native component lookup failed");
      };
    } else {
      windmill_activate_body = [](void *, bool) {
        throw std::runtime_error("native mechanism activation failed");
      };
    }
    bool escaped = false;
    try {
      f.run();
    } catch (...) {
      escaped = true;
    }
    windmill_check(
        !escaped && city_windmill_repair_count() == 0,
        "failed native callback does not escape the game command hook");
    windmill_find_body = {};
    windmill_activate_body = {};
    windmill_clock += 1000;
    f.run();
    windmill_check(city_windmill_repair_count() == 1,
                   "exception cleanup permits the next native repair attempt");
  }
  for (WindmillApi api :
       {WindmillApi{nullptr, mock_activate_windmill, mock_windmill_clock},
        WindmillApi{mock_find_windmill, nullptr, mock_windmill_clock},
        WindmillApi{}}) {
    WindmillFixture f;
    configure_windmill(api);
    f.expect_no_repair("incomplete API never invokes a null native target");
  }
  std::printf("Windmill: %d checks, %d failures\n", windmill_checks,
              windmill_failures);
  return windmill_failures;
}
