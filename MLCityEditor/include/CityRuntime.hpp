#pragma once

#include <cstddef>
#include <cstdint>

struct CityVector {
  double x{};
  double y{};
  double z{};
};

struct CityArray {
  void *data{};
  std::int32_t count{};
  std::int32_t capacity{};
};

static_assert(sizeof(CityArray) == 16);
static_assert(sizeof(CityVector) == 24);

extern std::uintptr_t city_image;

auto city_read(std::uintptr_t address, void *output, std::size_t size) noexcept
    -> bool;
auto city_valid_object(void *object) noexcept -> bool;
auto city_player_pawn(void *pawn) noexcept -> bool;
auto city_building_owner(void *building) noexcept -> void *;
auto city_player_building(void *building,
                          void *expected_pawn = nullptr) noexcept -> bool;
auto city_edited_manor(void *building) noexcept -> bool;
