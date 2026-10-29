#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

struct PlacementApi {
  bool (*read_memory)(std::uintptr_t, void *, std::size_t) noexcept {};
  void (*set_visibility)(void *, bool, std::uint8_t) noexcept {};
  void (*set_module_collision)(void *, bool) noexcept {};
  void (*retire_helper)(void *) noexcept {};
};

struct PlacementBounds {
  std::array<double, 3> center{};
  std::array<double, 3> extent{};
};

static_assert(sizeof(PlacementBounds) == 48);

struct PlacementStats {
  std::uint64_t buildings{};
  std::uint64_t candidates{};
  std::uint64_t hidden_priority{};
  std::uint64_t shown_priority{};
  std::uint64_t hidden_marked{};
  std::uint64_t shown_marked{};
  std::uint64_t shown_general{};
  std::uint64_t collisions_restored{};
  std::uint64_t helpers_retired{};
  std::uint64_t skipped{};
};

extern std::uintptr_t placement_image;

auto configure_placement_api(const PlacementApi &api) noexcept -> void;
auto placement_bounds_intersect(const PlacementBounds &first,
                                const PlacementBounds &second) noexcept -> bool;
auto placement_reconcile_building(void *building) noexcept -> PlacementStats;
auto placement_reconcile_world(void *master) noexcept -> PlacementStats;
