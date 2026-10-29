#pragma once

#include "CityRuntime.hpp"

struct DemolitionApi {
  void (*demolish)(void *building, bool refund){};
  void *(*spawn_building)(void *game, int type, const CityVector *position,
                          const CityVector *rotation){};
};

auto configure_demolition(const DemolitionApi &api) noexcept -> void;
auto city_demolish(void *building, bool refund, void *pawn) -> void;
auto city_demolition_supplies(void *game, int type, const CityVector *position,
                              const CityVector *rotation, void *building)
    -> void *;
