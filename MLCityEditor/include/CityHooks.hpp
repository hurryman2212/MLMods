#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "CityPatches.hpp"

struct CityFingerprint {
  std::uintptr_t rva;
  std::string_view bytes;
};

auto city_fingerprints() noexcept -> std::span<const CityFingerprint>;
auto city_validate_image(const std::uint8_t *image, std::size_t size) -> bool;
auto city_make_patches() -> std::vector<CityCodePatch>;
auto city_prepare_callbacks() noexcept -> void;
auto city_clear_callbacks() noexcept -> void;
auto city_manor_bridge(const void *callback, const void *resume)
    -> std::vector<std::uint8_t>;
auto city_supplies_bridge(const void *callback) -> std::vector<std::uint8_t>;
