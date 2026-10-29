#pragma once

#include <cstddef>
#include <cstdint>

#include "CityRuntime.hpp"

struct UpgradeApi {
  void (*begin)(void *, std::int32_t){};
  void (*setup)(void *, std::int32_t, std::uint8_t){};
  void (*type_callback)(void *){};
  void (*extension)(void *, std::int32_t, std::int32_t){};
  void (*artisan)(void *, std::int32_t){};
  void (*weak_assign)(std::uint64_t *, void *){};
  void *(*weak_get)(const std::uint64_t *){};
  float (*work_done)(void *){};
  float (*work_required)(void *){};
  void (*add_work)(void *, float){};
  void (*visual)(void *, float, std::uint8_t){};
  void (*finish)(void *){};
  CityArray *(*resource_cost)(void *, CityArray *, std::int32_t){};
  std::int32_t (*wealth_cost)(void *, std::int32_t){};
  void (*artisan_callback)(void *, void **){};
  void (*assembly_request)(void *, void *, void *, void *, std::uint8_t,
                           CityArray *){};
  void (*assembly_callback)(void *, void *, void *, void *, std::uint8_t,
                            CityArray *){};
  bool (*mature)(void *, std::int32_t){};
  CityArray *(*reconstruction_cost)(void *, CityArray *, std::int32_t){};
  void (*initialize_type)(void *){};
};

auto configure_upgrade(const UpgradeApi &api) noexcept -> void;
auto reset_upgrades() noexcept -> void;
auto upgrade_pending_count() noexcept -> std::size_t;
auto upgrade_failed_count() noexcept -> std::uint64_t;
auto city_can_rebuild_house(void *building) noexcept -> bool;
auto city_upgrade_cancel_ready(void *building) noexcept -> bool;
auto city_begin_upgrade(void *building, std::int32_t upgrade_id) -> void;
auto city_upgrade_cost_setup(void *building, std::int32_t upgrade_id,
                             std::uint8_t play_sound) -> void;
auto city_upgrade_type_callback(void *capture) -> void;
auto city_change_extension(void *building, std::int32_t index,
                           std::int32_t type) -> void;
auto city_change_artisan(void *building, std::int32_t type) -> void;
auto city_upgrade_artisan_callback(void *capture, void **replacement) -> void;
auto city_upgrade_assembly_request(void *master, void *building, void *path,
                                   void *transform, std::uint8_t instant,
                                   CityArray *tags) -> void;
auto city_upgrade_assembly_callback(void *master, void *building, void *path,
                                    void *transform, std::uint8_t instant,
                                    CityArray *tags) -> void;
auto city_upgrade_resource_cost(void *building, CityArray *result,
                                std::int32_t upgrade_id) -> CityArray *;
auto city_reconstruction_resource_cost(void *building, CityArray *result,
                                       std::int32_t building_type)
    -> CityArray *;
auto city_initialize_reconstruction(void *building) -> void;
auto city_upgrade_wealth_cost(void *building, std::int32_t upgrade_id)
    -> std::int32_t;
