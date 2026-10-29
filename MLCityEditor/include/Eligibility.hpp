#pragma once

#include <cstdint>

#include "CityRuntime.hpp"

struct EligibilityApi {
  bool (*can_upgrade)(void *, std::int32_t, CityArray *){};
  CityArray *(*options)(void *, CityArray *, std::uint8_t){};
  const void *(*upgrade_row)(void *, std::int32_t){};
  void (*resize_int_array)(CityArray *, std::int32_t){};
  void (*free_memory)(void *){};
};

auto configure_eligibility(const EligibilityApi &api) noexcept -> void;
auto reset_eligibility() noexcept -> void;
auto city_can_upgrade(void *building, std::int32_t upgrade_id,
                      CityArray *reasons) -> bool;
auto city_upgrade_options(void *building, CityArray *result,
                          std::uint8_t include_extensions) -> CityArray *;
