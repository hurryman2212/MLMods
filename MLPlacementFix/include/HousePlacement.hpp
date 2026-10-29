#pragma once

#include <cstddef>
#include <cstdint>

struct HouseOptionalModel {
  std::int32_t model{};
  std::uint8_t valid{};
  std::uint8_t padding[3]{};
};

static_assert(sizeof(HouseOptionalModel) == 8);

enum class HousePlacementFailure : std::uint8_t {
  None,
  NativeContext,
  AssemblyBarrier,
  OwnerArrays,
  ModuleOwner,
  ExtensionMembers,
  ResourceOwner,
  HelperArray,
  VisualCache,
  Identity,
  Exception,
};

struct HousePlacementDiagnostic {
  std::uintptr_t owner{};
  HousePlacementFailure failure{};
};

struct HousePlacementApi {
  bool (*read_memory)(std::uintptr_t, void *, std::size_t) noexcept {};
  void (*rebuild)(void *, std::int32_t, std::uint8_t, std::uint8_t,
                  HouseOptionalModel *){};
  std::int32_t (*choose)(void *, float, float *, std::int32_t, std::uint8_t){};
  std::uintptr_t image{};
  bool (*can_rebuild_house)(void *) noexcept {};
  void (*destroy_actor)(void *, std::uint8_t){};
  std::int32_t (*remove_actor)(void *, void *const *){};
  bool (*is_field)(void *){};
  void (*cleanup_module)(void *){};
};

auto configure_house_placement_api(const HousePlacementApi &api) noexcept
    -> void;
auto set_house_placement_enabled(bool enabled) noexcept -> void;
auto house_placement_repair_count() noexcept -> std::uint64_t;
auto house_placement_skip_count() noexcept -> std::uint64_t;
auto house_placement_diagnostic() noexcept -> HousePlacementDiagnostic;
auto house_placement_failure_name(HousePlacementFailure failure) noexcept
    -> const wchar_t *;
auto house_placement_rebuild(void *owner, std::int32_t level,
                             std::uint8_t primary, std::uint8_t secondary,
                             HouseOptionalModel *model) -> void;
auto house_placement_choose_primary(void *owner, float width, float *scale,
                                    std::int32_t level, std::uint8_t variant)
    -> std::int32_t;
auto house_placement_choose_secondary(void *owner, float width, float *scale,
                                      std::int32_t level, std::uint8_t variant)
    -> std::int32_t;
