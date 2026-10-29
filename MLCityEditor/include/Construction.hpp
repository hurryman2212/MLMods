#pragma once

#include <cstdint>

struct ConstructionApi {
  void (*add_building)(void *, bool){};
  void (*change_silver)(void *, int){};
  void (*initialize)(void *){};
};

auto configure_construction(const ConstructionApi &api) -> void;
auto city_place_building(void *pawn, bool construct_immediately) -> void;
auto city_construction_treasury(void *pawn, int amount) -> void;
auto city_initialize_plot(void *building, void *pawn) -> void;
auto city_enable_manor_instant(void *building, std::uint8_t *stack_flag)
    -> void;
