#include <cstdint>

#include "CityRuntime.hpp"
#include "Demolition.hpp"

struct DemolitionScopeState {
  void *building{};
  void *game{};
};

static DemolitionApi demolition_api{};
static thread_local DemolitionScopeState demolition_scope{};

class DemolitionScope final {
public:
  DemolitionScope(void *building, void *game) : previous_{demolition_scope} {
    demolition_scope = {building, game};
  }

  ~DemolitionScope() { demolition_scope = previous_; }

  DemolitionScope(const DemolitionScope &) = delete;
  auto operator=(const DemolitionScope &) -> DemolitionScope & = delete;

private:
  DemolitionScopeState previous_{};
};

auto configure_demolition(const DemolitionApi &api) noexcept -> void {
  demolition_api = api;
}

auto city_demolish(void *building, bool refund, void *pawn) -> void {
  if (demolition_api.demolish == nullptr) {
    return;
  }

  void *game{};
  if (!city_player_pawn(pawn) || !city_player_building(building, pawn) ||
      !city_read(reinterpret_cast<std::uintptr_t>(building) + 0x2e8, &game,
                 sizeof(game)) ||
      !city_valid_object(game)) {
    demolition_api.demolish(building, refund);
    return;
  }

  // Only the explicit demolishRequests call enters this scope. Native
  // eligibility checks, worker/livestock reassignment and teardown still run.
  const DemolitionScope scope{building, game};
  demolition_api.demolish(building, false);
}

auto city_demolition_supplies(void *game, int type, const CityVector *position,
                              const CityVector *rotation, void *building)
    -> void * {
  if (demolition_scope.building != nullptr &&
      demolition_scope.building == building && demolition_scope.game == game &&
      type == 88) {
    // The exact callsite handles nullptr by continuing normal demolition.
    // Suppress creation before any actor, inventory or region is registered.
    return nullptr;
  }
  if (demolition_api.spawn_building == nullptr) {
    return nullptr;
  }
  return demolition_api.spawn_building(game, type, position, rotation);
}
