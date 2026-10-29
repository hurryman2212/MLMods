#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include <Windows.h>

#include "Eligibility.hpp"

static std::size_t eligibility_checks{};
static void check_eligibility(bool condition, const char *message) {
  ++eligibility_checks;
  if (!condition)
    throw std::runtime_error(message);
}

template <typename T, std::size_t N>
static void eligibility_put(std::array<std::uint8_t, N> &buffer,
                            std::size_t offset, const T &value) {
  if (offset + sizeof(value) > N)
    throw std::runtime_error("eligibility fixture bounds");
  std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

struct EligibilityFixture {
  alignas(16) std::array<std::uint8_t, 0x1500> building{};
  alignas(16) std::array<std::uint8_t, 0x400> pawn{};
  alignas(16) std::array<std::uint8_t, 0x30> game{}, region{}, object_class{};
  std::array<std::uint8_t, 0xd0> extension{};
  std::array<std::uint8_t, 0x88> upgrade_row{};
  std::array<std::int32_t, 2> applied{7, 40};
  std::array<std::uint64_t, 4> reason_names{11, 22, 33, 44};
  CityArray reasons{reason_names.data(), 3, 4};

  EligibilityFixture() {
    eligibility_put(object_class, 0, city_image + 0x1000);
    const auto initialize = [&](auto &object, int index) {
      eligibility_put(object, 0, city_image + 0x2000);
      eligibility_put(object, 0xc, index);
      eligibility_put(object, 0x10,
                      reinterpret_cast<std::uintptr_t>(object_class.data()));
    };
    initialize(building, 80);
    initialize(pawn, 81);
    initialize(game, 82);
    initialize(region, 83);
    eligibility_put(building, 0x2e0,
                    reinterpret_cast<std::uintptr_t>(pawn.data()));
    eligibility_put(building, 0x2e8,
                    reinterpret_cast<std::uintptr_t>(game.data()));
    eligibility_put(building, 0x2c8,
                    reinterpret_cast<std::uintptr_t>(region.data()));
    eligibility_put(pawn, 0x34c, std::uint8_t{1});
    eligibility_put(building, 0x3c1, std::uint8_t{1});
    eligibility_put(building, 0x3b8, std::int32_t{9});
    eligibility_put(building, 0xd48, std::int32_t{1});
    eligibility_put(building, 0x100, std::uint64_t{0x1122334455667788ULL});
    upgrade_row[0x7f] = 1;
  }
};

static EligibilityFixture *eligibility_fixture{};
static std::vector<std::int32_t> eligibility_base_options;
static std::unordered_set<void *> eligibility_allocations;
static std::size_t eligibility_original_calls{}, eligibility_options_calls{};
static std::size_t eligibility_row_calls{}, eligibility_free_calls{};
static bool eligibility_original_result{}, eligibility_throw_grow{};
static std::int32_t eligibility_last_id{};
static void *eligibility_last_building{};
static std::uint8_t eligibility_last_include{};

static bool eligibility_mock_can(void *building, std::int32_t id,
                                 CityArray *reasons) {
  ++eligibility_original_calls;
  eligibility_last_building = building;
  eligibility_last_id = id;
  if (reasons)
    reasons->count = 2;
  return eligibility_original_result;
}

static CityArray *eligibility_mock_options(void *building, CityArray *result,
                                           std::uint8_t include) {
  ++eligibility_options_calls;
  eligibility_last_building = building;
  eligibility_last_include = include;
  *result = {};
  if (!eligibility_base_options.empty()) {
    const auto bytes = eligibility_base_options.size() * sizeof(std::int32_t);
    result->data = HeapAlloc(GetProcessHeap(), 0, bytes);
    check_eligibility(result->data != nullptr,
                      "allocate native options fixture");
    eligibility_allocations.insert(result->data);
    std::memcpy(result->data, eligibility_base_options.data(), bytes);
    result->count = result->capacity =
        static_cast<std::int32_t>(eligibility_base_options.size());
  }
  return result;
}

static const void *eligibility_mock_row(void *game, std::int32_t id) {
  ++eligibility_row_calls;
  check_eligibility(game == eligibility_fixture->game.data(),
                    "upgrade row receives the building game instance");
  eligibility_last_id = id;
  return eligibility_fixture->upgrade_row.data();
}

static void eligibility_mock_grow(CityArray *array, std::int32_t old_count) {
  check_eligibility(array->count == old_count + 1,
                    "ResizeGrow receives updated Num and old Num");
  if (eligibility_throw_grow)
    throw std::runtime_error("synthetic grow failure");
  const auto capacity = array->count + 4;
  void *data{};
  if (array->data) {
    check_eligibility(eligibility_allocations.contains(array->data),
                      "grow operates on native-owned allocation");
    void *old = array->data;
    data =
        HeapReAlloc(GetProcessHeap(), 0, old,
                    static_cast<std::size_t>(capacity) * sizeof(std::int32_t));
    if (data)
      eligibility_allocations.erase(old);
  } else {
    data = HeapAlloc(GetProcessHeap(), 0,
                     static_cast<std::size_t>(capacity) * sizeof(std::int32_t));
  }
  if (!data)
    throw std::runtime_error("fixture allocation failed");
  eligibility_allocations.insert(data);
  array->data = data;
  array->capacity = capacity;
}

static void eligibility_mock_free(void *memory) {
  ++eligibility_free_calls;
  check_eligibility(eligibility_allocations.erase(memory) == 1,
                    "free exactly one native-owned allocation");
  check_eligibility(HeapFree(GetProcessHeap(), 0, memory) != 0,
                    "release options allocation");
}

static auto eligibility_test_api() -> EligibilityApi {
  return {eligibility_mock_can, eligibility_mock_options, eligibility_mock_row,
          eligibility_mock_grow, eligibility_mock_free};
}

static auto eligibility_contains(const CityArray &array, int wanted) -> bool {
  const auto *values = static_cast<const std::int32_t *>(array.data);
  for (int i = 0; i < array.count; ++i)
    if (values[i] == wanted)
      return true;
  return false;
}

int run_eligibility_tests() {
  const auto previous_image = city_image;
  try {
    city_image = 0x140000000;
    EligibilityFixture fixture;
    eligibility_fixture = &fixture;
    eligibility_base_options = {7};
    configure_eligibility(eligibility_test_api());
    const auto initial_building = fixture.building;
    const auto initial_pawn = fixture.pawn;
    const auto initial_region = fixture.region;
    const auto initial_names = fixture.reason_names;
    auto *building = fixture.building.data();
    check_eligibility(
        city_can_upgrade(building, 7, &fixture.reasons),
        "player eligible building bypasses original false prerequisite result");
    check_eligibility(eligibility_original_calls == 0 &&
                          eligibility_row_calls == 1 &&
                          eligibility_options_calls == 1,
                      "only structural native helpers run for bypass");
    check_eligibility(
        fixture.reasons.count == 0 && fixture.reasons.capacity == 4 &&
            fixture.reasons.data == fixture.reason_names.data() &&
            fixture.reason_names == initial_names,
        "reason allocation/capacity/elements preserved, only Num cleared");
    check_eligibility(
        fixture.building == initial_building && fixture.pawn == initial_pawn &&
            fixture.region == initial_region && eligibility_allocations.empty(),
        "no perk/level/family/inventory/location state changed and no "
        "temporary leak");
    eligibility_original_result = true;
    check_eligibility(!city_can_upgrade(building, 8, &fixture.reasons),
                      "wrong upgrade for this building remains rejected");
    for (int id : {-1, 0, 1, 3, 46, 99, 117, 1000})
      check_eligibility(!city_can_upgrade(building, id, &fixture.reasons),
                        "unknown or unsupported IDs fail closed");

    for (int invalid = 0; invalid < 7; ++invalid) {
      fixture.building = initial_building;
      if (invalid == 0)
        eligibility_put(fixture.building, 0x3c1, std::uint8_t{0});
      else if (invalid == 1)
        eligibility_put(fixture.building, 0x3a1, std::uint8_t{1});
      else if (invalid == 2)
        eligibility_put(fixture.building, 8, std::uint32_t{0x40000000});
      else if (invalid == 3)
        eligibility_put(fixture.building, 0x2c8, std::uintptr_t{0});
      else if (invalid == 4)
        eligibility_put(fixture.building, 0x2e8, std::uintptr_t{1});
      else if (invalid == 5)
        eligibility_put(fixture.building, 0x430, CityArray{nullptr, -1, 0});
      else
        eligibility_put(fixture.building, 0xb88,
                        CityArray{reinterpret_cast<void *>(1), 1, 1});
      const auto calls = eligibility_original_calls;
      check_eligibility(
          !city_can_upgrade(building, 7, &fixture.reasons) &&
              eligibility_original_calls == calls,
          "invalid structural state never enters unsafe original check");
    }
    fixture.building = initial_building;
    check_eligibility(!city_can_upgrade(nullptr, 7, &fixture.reasons) &&
                          !city_can_upgrade(building, 7, nullptr),
                      "null object or output reasons safe");
    eligibility_put(fixture.building, 0x430,
                    CityArray{fixture.applied.data(), 1, 2});
    check_eligibility(!city_can_upgrade(building, 7, &fixture.reasons),
                      "one-time upgrade cannot be applied twice");
    fixture.upgrade_row[0x7f] = 0;
    check_eligibility(
        city_can_upgrade(building, 7, &fixture.reasons),
        "native repeatable upgrade definition remains repeatable");
    fixture.upgrade_row[0x7f] = 1;
    fixture.building = initial_building;

    eligibility_put(fixture.pawn, 0x34d, std::uint8_t{1});
    auto calls = eligibility_original_calls;
    check_eligibility(city_can_upgrade(building, 7, &fixture.reasons) &&
                          eligibility_original_calls == calls + 1 &&
                          fixture.reasons.count == 2,
                      "AI result and original reasons are unchanged");
    CityArray options{};
    city_upgrade_options(building, &options, 1);
    check_eligibility(options.count == 1 && eligibility_contains(options, 7),
                      "AI option list unchanged");
    eligibility_mock_free(options.data);
    fixture.pawn = initial_pawn;
    for (int id : {13, 19, 21, 26, 27, 28, 100}) {
      calls = eligibility_original_calls;
      check_eligibility(city_can_upgrade(building, id, &fixture.reasons) &&
                            eligibility_original_calls == calls + 1 &&
                            eligibility_last_id == id,
                        "animal orders and none/removal retain original path");
    }

    eligibility_base_options.clear();
    for (const auto [type, id] :
         {std::pair{28, 40}, std::pair{32, 39}, std::pair{33, 41},
          std::pair{61, 36}, std::pair{69, 44}, std::pair{86, 38},
          std::pair{121, 45}}) {
      eligibility_put(fixture.building, 0x3b8, type);
      options = {};
      check_eligibility(
          city_upgrade_options(building, &options, 0) == &options &&
              options.count == 1 && eligibility_contains(options, id),
          "perk-locked option added only to exact native building type");
      eligibility_mock_free(options.data);
      check_eligibility(
          city_can_upgrade(building, id, &fixture.reasons),
          "matching newly visible upgrade passes structural eligibility");
    }
    eligibility_put(fixture.building, 0x3b8, std::int32_t{14});
    options = {};
    city_upgrade_options(building, &options, 0);
    check_eligibility(
        options.count == 0 && options.data == nullptr,
        "missing hunting-hound perk is not bypassed for animal purchase");

    fixture.building = initial_building;
    eligibility_put(fixture.building, 0xb88,
                    CityArray{fixture.extension.data(), 1, 1});
    eligibility_base_options = {109, 101};
    options = {};
    city_upgrade_options(building, &options, 1);
    check_eligibility(options.count == 16 && eligibility_last_include == 1,
                      "level-one house exposes exact 16 native extension "
                      "choices without duplicates");
    for (int id : {109, 106, 111, 110, 105, 115, 113, 103, 116, 104, 107, 108,
                   101, 102, 112, 114})
      check_eligibility(eligibility_contains(options, id),
                        "native extension list ID preserved");
    eligibility_mock_free(options.data);
    check_eligibility(
        city_can_upgrade(building, 104, &fixture.reasons),
        "level-three artisan option works at level one without changing level");
    eligibility_base_options.clear();
    options = {};
    city_upgrade_options(building, &options, 0);
    check_eligibility(options.count == 0,
                      "includeExtensions=false remains respected");

    for (const auto [type, id] :
         {std::pair{5, 37}, std::pair{13, 42}, std::pair{4, 43}}) {
      eligibility_put(fixture.extension, 0, type);
      options = {};
      city_upgrade_options(building, &options, 1);
      check_eligibility(
          options.count == 1 && eligibility_contains(options, id),
          "artisan-specific upgrade does not expose unrelated extensions");
      eligibility_mock_free(options.data);
    }
    eligibility_put(fixture.extension, 0, std::int32_t{8});
    eligibility_base_options = {29, 30, 31};
    options = {};
    city_upgrade_options(building, &options, 1);
    check_eligibility(options.count == 3 && !eligibility_contains(options, 104),
                      "native unplanted vegetable-choice branch preserved");
    eligibility_mock_free(options.data);
    eligibility_put(fixture.extension, 0, std::int32_t{2});
    eligibility_base_options = {27, 28};
    options = {};
    city_upgrade_options(building, &options, 1);
    check_eligibility(options.count == 2,
                      "native empty-livestock choice branch unchanged");
    eligibility_mock_free(options.data);

    fixture.building = initial_building;
    eligibility_put(fixture.building, 0x3b8, std::int32_t{28});
    eligibility_base_options = {7};
    eligibility_throw_grow = true;
    check_eligibility(!city_can_upgrade(building, 40, &fixture.reasons),
                      "failed native growth does not authorize missing option");
    check_eligibility(eligibility_allocations.empty(),
                      "temporary options freed after grow failure");
    eligibility_throw_grow = false;
    fixture.building = initial_building;
    auto partial = eligibility_test_api();
    partial.free_memory = nullptr;
    configure_eligibility(partial);
    calls = eligibility_original_calls;
    check_eligibility(city_can_upgrade(building, 7, &fixture.reasons) &&
                          eligibility_original_calls == calls + 1,
                      "incomplete configuration uses original behavior");
    configure_eligibility(eligibility_test_api());
    for (int i = 0; i < 100; ++i)
      check_eligibility(city_can_upgrade(building, 7, &fixture.reasons),
                        "repeat UI eligibility query succeeds");
    check_eligibility(
        eligibility_allocations.empty(),
        "all original/copy option buffers released after repeated UI queries");
    reset_eligibility();
    city_image = previous_image;
    std::printf("Eligibility tests PASS %zu checks\n", eligibility_checks);
    return 0;
  } catch (const std::exception &error) {
    for (void *memory : eligibility_allocations)
      HeapFree(GetProcessHeap(), 0, memory);
    eligibility_allocations.clear();
    reset_eligibility();
    city_image = previous_image;
    std::fprintf(stderr, "Eligibility tests FAIL after %zu checks: %s\n",
                 eligibility_checks, error.what());
    return 1;
  }
}
