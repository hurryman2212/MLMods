#pragma once

#include <cstdint>

struct MaturityApi {
  void (*refresh_visuals)(void *){};
  void (*refresh_progress)(void *, std::uint8_t, std::uint8_t, std::uint8_t,
                           std::uint8_t){};
};

auto configure_maturity(const MaturityApi &api) noexcept -> void;
auto city_mature_new_orchard(void *building, std::int32_t upgrade_id) -> bool;
