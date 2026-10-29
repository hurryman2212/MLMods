#include <cstdint>

#include <Windows.h>

#include "CityRuntime.hpp"
#include "Construction.hpp"

static ConstructionApi construction_api{};
static thread_local void *construction_pawn{};

auto configure_construction(const ConstructionApi &api) -> void {
  construction_api = api;
}

auto city_place_building(void *pawn, bool construct_immediately) -> void {
  if (construction_api.add_building == nullptr) {
    return;
  }
  const bool editing = city_player_pawn(pawn);
  struct PlacementScope {
    void *previous;
    explicit PlacementScope(void *active) : previous(construction_pawn) {
      construction_pawn = active;
    }
    ~PlacementScope() { construction_pawn = previous; }
  } scope{editing ? pawn : nullptr};

  // Only the queued placement CALL enters this wrapper. The original still
  // performs terrain, collision and ownership checks before creating anything.
  construction_api.add_building(pawn, construct_immediately || editing);
}

auto city_construction_treasury(void *pawn, int amount) -> void {
  if (construction_api.change_silver == nullptr) {
    return;
  }
  // This callback replaces only addBuilding's construction-cost CALL, not
  // changeSilver globally. Nested operations for another pawn remain intact.
  if (amount < 0 && construction_pawn == pawn && pawn != nullptr &&
      city_player_pawn(pawn)) {
    amount = 0;
  }
  construction_api.change_silver(pawn, amount);
}

auto city_initialize_plot(void *building, void *pawn) -> void {
  if (construction_api.initialize == nullptr) {
    return;
  }
  if (city_player_pawn(pawn) && city_player_building(building, pawn)) {
    // The two hooked commitField CALLs initialize fresh polygon buildings.
    // This is the same pre-initialization constructed flag used by the
    // native instant-building path, not a shortcut for existing work sites.
    const std::uint8_t constructed = 1;
    SIZE_T written{};
    WriteProcessMemory(GetCurrentProcess(),
                       reinterpret_cast<void *>(
                           reinterpret_cast<std::uintptr_t>(building) + 0x3c1),
                       &constructed, sizeof(constructed), &written);
  }
  construction_api.initialize(building);
}

auto city_enable_manor_instant(void *building, std::uint8_t *stack_flag)
    -> void {
  if (stack_flag == nullptr || !city_edited_manor(building)) {
    return;
  }
  // This is the native conversion call's local instaBuild flag. Do not add
  // the cheat tag to shared game settings or complete any building on load.
  const std::uint8_t enabled = 1;
  SIZE_T written{};
  WriteProcessMemory(GetCurrentProcess(), stack_flag, &enabled, sizeof(enabled),
                     &written);
}
