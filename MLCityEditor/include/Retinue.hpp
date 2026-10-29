#pragma once

#include <cstdint>

#include "CityRuntime.hpp"

struct RetinueApi {
  void (*process_commands)(void *){};
  void (*initialize_empty_squad)(void *, void *){};
  void (*grow_pointer_array)(CityArray *, std::int32_t){};
  std::uint64_t (*clock_ms)(){};
};

auto configure_retinue(const RetinueApi &api) -> void;
auto city_process_retinue_requests(void *master) -> void;
auto city_retinue_repair_count() noexcept -> std::uint64_t;
auto city_retinue_conflict_count() noexcept -> std::uint64_t;
