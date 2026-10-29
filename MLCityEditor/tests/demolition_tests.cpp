#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "CityRuntime.hpp"
#include "Demolition.hpp"

// Test callbacks need shared fixtures; keep their symbols private to this TU.
namespace {
struct DemolitionTestObject {
  alignas(16) std::array<std::uint8_t, 0x1500> bytes{};
};

struct DemolitionTestCall {
  void *building{};
  bool refund{};
};

enum class DemolitionTestMode { Normal, NestedOther, NestedPlayer, Throwing };

struct DemolitionTestFixture {
  std::uintptr_t class_vtable{};
  DemolitionTestObject pawn{};
  DemolitionTestObject ai_pawn{};
  DemolitionTestObject game{};
  DemolitionTestObject other_game{};
  DemolitionTestObject building{};
  DemolitionTestObject second_building{};
  DemolitionTestObject ai_building{};
  CityVector position{1.0, 2.0, 3.0};
  CityVector rotation{4.0, 5.0, 6.0};
  std::vector<DemolitionTestCall> calls{};
  DemolitionTestMode mode{DemolitionTestMode::Normal};
  int native_spawns{};
  int native_cleanup_continued{};
  int suppressed_returns{};
  void *last_game{};
  int last_type{};
  const CityVector *last_position{};
  const CityVector *last_rotation{};
};

DemolitionTestFixture *demolition_fixture{};
int demolition_failures{};
int demolition_checks{};

auto demolition_expect(bool condition, const char *message) -> void {
  ++demolition_checks;
  if (!condition) {
    ++demolition_failures;
    std::fprintf(stderr, "demolition: %s\n", message);
  }
}

auto demolition_object(DemolitionTestObject &object,
                       DemolitionTestFixture &fixture) -> void {
  const std::uintptr_t vtable = city_image + 0x1000;
  const std::uintptr_t object_class =
      reinterpret_cast<std::uintptr_t>(&fixture.class_vtable);
  const std::int32_t index = 1;
  std::memcpy(object.bytes.data(), &vtable, sizeof(vtable));
  std::memcpy(object.bytes.data() + 12, &index, sizeof(index));
  std::memcpy(object.bytes.data() + 16, &object_class, sizeof(object_class));
}

auto demolition_owned(DemolitionTestObject &object, void *pawn, void *game)
    -> void {
  std::memcpy(object.bytes.data() + 0x2e0, &pawn, sizeof(pawn));
  std::memcpy(object.bytes.data() + 0x2e8, &game, sizeof(game));
}

auto demolition_spawn(void *game, int type, const CityVector *position,
                      const CityVector *rotation) -> void * {
  auto &fixture = *demolition_fixture;
  ++fixture.native_spawns;
  fixture.last_game = game;
  fixture.last_type = type;
  fixture.last_position = position;
  fixture.last_rotation = rotation;
  return &fixture.other_game;
}

auto demolition_native(void *building, bool refund) -> void {
  auto &fixture = *demolition_fixture;
  fixture.calls.push_back({building, refund});
  if (fixture.mode == DemolitionTestMode::Throwing) {
    throw std::runtime_error("native test exception");
  }
  if (fixture.mode == DemolitionTestMode::NestedOther) {
    const int before = fixture.native_spawns;
    city_demolition_supplies(&fixture.game, 88, &fixture.position,
                             &fixture.rotation, &fixture.second_building);
    city_demolition_supplies(&fixture.other_game, 88, &fixture.position,
                             &fixture.rotation, building);
    city_demolition_supplies(&fixture.game, 89, &fixture.position,
                             &fixture.rotation, building);
    demolition_expect(fixture.native_spawns == before + 3,
                      "nested different building/game/type must forward");
  }
  if (fixture.mode == DemolitionTestMode::NestedPlayer &&
      building == &fixture.building) {
    city_demolish(&fixture.second_building, true, &fixture.pawn);
  }
  if (city_demolition_supplies(&fixture.game, 88, &fixture.position,
                               &fixture.rotation, building) == nullptr) {
    ++fixture.suppressed_returns;
  }
  // Native demolish has this null-return continuation rather than aborting.
  ++fixture.native_cleanup_continued;
}
} // namespace

auto run_demolition_tests() -> int {
  DemolitionTestFixture fixture{};
  demolition_fixture = &fixture;
  demolition_failures = 0;
  demolition_checks = 0;
  const auto previous_image = city_image;
  city_image = 0x140000000;
  fixture.class_vtable = city_image + 0x2000;
  for (auto *object :
       {&fixture.pawn, &fixture.ai_pawn, &fixture.game, &fixture.other_game,
        &fixture.building, &fixture.second_building, &fixture.ai_building}) {
    demolition_object(*object, fixture);
  }
  fixture.pawn.bytes[0x34c] = 1;
  fixture.ai_pawn.bytes[0x34d] = 1;
  demolition_owned(fixture.building, &fixture.pawn, &fixture.game);
  demolition_owned(fixture.second_building, &fixture.pawn, &fixture.game);
  demolition_owned(fixture.ai_building, &fixture.ai_pawn, &fixture.game);
  const auto building_before = fixture.building.bytes;
  const auto pawn_before = fixture.pawn.bytes;
  configure_demolition({demolition_native, demolition_spawn});

  demolition_expect(city_player_pawn(&fixture.pawn), "player fixture valid");
  demolition_expect(city_player_building(&fixture.building, &fixture.pawn),
                    "building fixture valid");
  city_demolish(&fixture.building, true, &fixture.pawn);
  demolition_expect(fixture.calls.size() == 1 && !fixture.calls.back().refund,
                    "explicit player demolition disables refund");
  demolition_expect(fixture.native_spawns == 0 &&
                        fixture.suppressed_returns == 1,
                    "player supplies not created");
  demolition_expect(fixture.native_cleanup_continued == 1,
                    "native cleanup continues after suppressed supplies");
  demolition_expect(fixture.building.bytes == building_before &&
                        fixture.pawn.bytes == pawn_before,
                    "wrapper does not mutate building, inventory or pawn");

  auto *spawn = city_demolition_supplies(&fixture.game, 88, &fixture.position,
                                         &fixture.rotation, &fixture.building);
  demolition_expect(spawn == &fixture.other_game && fixture.native_spawns == 1,
                    "no explicit demolition scope forwards supplies");
  demolition_expect(fixture.last_game == &fixture.game &&
                        fixture.last_type == 88 &&
                        fixture.last_position == &fixture.position &&
                        fixture.last_rotation == &fixture.rotation,
                    "all native spawn arguments preserved");

  city_demolish(&fixture.ai_building, true, &fixture.ai_pawn);
  demolition_expect(fixture.calls.back().refund && fixture.native_spawns == 2,
                    "AI demolition refund and supplies unchanged");
  city_demolish(&fixture.ai_building, false, &fixture.pawn);
  demolition_expect(!fixture.calls.back().refund && fixture.native_spawns == 3,
                    "foreign building preserves original false refund");
  city_demolish(&fixture.building, true, nullptr);
  demolition_expect(fixture.calls.back().refund && fixture.native_spawns == 4,
                    "missing explicit player context forwards");

  fixture.pawn.bytes[0x34d] = 1;
  city_demolish(&fixture.building, true, &fixture.pawn);
  demolition_expect(fixture.calls.back().refund && fixture.native_spawns == 5,
                    "AI flag prevents main-player-like pawn override");
  fixture.pawn.bytes[0x34d] = 0;
  demolition_owned(fixture.building, &fixture.pawn, nullptr);
  city_demolish(&fixture.building, true, &fixture.pawn);
  demolition_expect(fixture.calls.back().refund && fixture.native_spawns == 6,
                    "missing game pointer fails closed to native");
  demolition_owned(fixture.building, &fixture.pawn, &fixture.game);

  fixture.mode = DemolitionTestMode::NestedOther;
  const int nested_before = fixture.native_spawns;
  city_demolish(&fixture.building, true, &fixture.pawn);
  demolition_expect(fixture.native_spawns == nested_before + 3 &&
                        !fixture.calls.back().refund,
                    "exact target suppression survives unrelated nested calls");
  fixture.mode = DemolitionTestMode::NestedPlayer;
  const int suppressed_before = fixture.suppressed_returns;
  const int cleanup_before = fixture.native_cleanup_continued;
  city_demolish(&fixture.building, true, &fixture.pawn);
  demolition_expect(fixture.suppressed_returns == suppressed_before + 2 &&
                        fixture.native_cleanup_continued == cleanup_before + 2,
                    "nested explicit scopes restore outer target and cleanup");
  demolition_expect(fixture.native_spawns == nested_before + 3,
                    "neither nested explicit demolition creates supplies");

  fixture.mode = DemolitionTestMode::Throwing;
  bool propagated{};
  try {
    city_demolish(&fixture.building, true, &fixture.pawn);
  } catch (const std::runtime_error &) {
    propagated = true;
  }
  demolition_expect(propagated, "native C++ exception propagates normally");
  const int after_exception = fixture.native_spawns;
  city_demolition_supplies(&fixture.game, 88, &fixture.position,
                           &fixture.rotation, &fixture.building);
  demolition_expect(fixture.native_spawns == after_exception + 1,
                    "RAII clears scope during exception unwind");
  demolition_expect(fixture.building.bytes == building_before &&
                        fixture.pawn.bytes == pawn_before,
                    "all scenarios leave authoritative object data unchanged");

  configure_demolition({});
  const auto call_count = fixture.calls.size();
  city_demolish(&fixture.building, true, &fixture.pawn);
  demolition_expect(fixture.calls.size() == call_count,
                    "unconfigured callback is not invoked");
  demolition_expect(
      city_demolition_supplies(&fixture.game, 88, &fixture.position,
                               &fixture.rotation, &fixture.building) == nullptr,
      "unconfigured spawn callback is not invoked");
  city_image = previous_image;
  demolition_fixture = nullptr;
  std::printf("demolition: %d checks, %d failures\n", demolition_checks,
              demolition_failures);
  return demolition_failures;
}
