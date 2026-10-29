#pragma once

#include <cstdint>

struct WindmillApi {
  void *(*find_component)(void *){};
  void (*activate)(void *, bool){};
  std::uint64_t (*clock_ms)(){};
};

auto configure_windmill(const WindmillApi &api) -> void;
auto city_repair_windmills(void *master) -> void;
auto city_windmill_repair_count() noexcept -> std::uint64_t;
