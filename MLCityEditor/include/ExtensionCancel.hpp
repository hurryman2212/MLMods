#pragma once

#include <array>
#include <cstdint>
#include <vector>

struct ExtensionCancelApi {
  void (*release_family)(void *, std::int32_t){};
  void (*disable_recipe)(void *, std::int32_t){};
  void (*remove_pointer)(void *, void **){};
  void (*refresh_resources)(void *){};
  void (*refresh_region)(void *, void *, std::int32_t, void *, std::uint8_t){};
  const void *(*building_stats)(std::int32_t){};
  void (*grow_pointers)(void *, std::int32_t){};
  std::int32_t (*remove_upgrade)(void *, const std::int32_t *){};
  void (*apply_upgrade_visuals)(void *){};
  void (*empty_production_entries)(void *, std::int32_t){};
  void (*free_memory)(void *){};
};

struct ExtensionCancelSnapshot {
  void *region{};
  std::int32_t extension_count{};
  std::vector<std::int32_t> families;
  std::vector<std::vector<void *>> members;
  std::vector<std::int32_t> upgrades;
  std::vector<std::array<std::uint8_t, 16>> visual_upgrades;
};

enum class ExtensionCancelResult { not_applicable, rejected, handled };

auto configure_extension_cancel(const ExtensionCancelApi &api) noexcept -> void;
auto reset_extension_cancel() noexcept -> void;
auto extension_cancel_declined_count() noexcept -> std::uint64_t;
auto city_can_cancel_artisan(void *building) noexcept -> bool;
auto prepare_artisan_cancel(void *building,
                            ExtensionCancelSnapshot &snapshot) noexcept -> bool;
auto begin_artisan_cancel(void *building,
                          const ExtensionCancelSnapshot &snapshot) -> bool;
auto prepare_artisan_upgrade_visuals(void *building) noexcept -> bool;
auto finish_artisan_cancel(void *building,
                           const ExtensionCancelSnapshot &snapshot) -> bool;
auto decline_artisan_cancel() noexcept -> void;
auto city_try_cancel_artisan(void *building, std::int32_t index,
                             std::int32_t type) -> ExtensionCancelResult;
