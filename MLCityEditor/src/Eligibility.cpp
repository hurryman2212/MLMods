#include <array>
#include <cstddef>
#include <cstdint>

#include <Windows.h>

#include "Eligibility.hpp"

static constexpr std::int32_t maximum_eligibility_entries = 4096;
static EligibilityApi eligibility_api{};

auto configure_eligibility(const EligibilityApi &api) noexcept -> void {
  eligibility_api = api;
}

auto reset_eligibility() noexcept -> void { eligibility_api = {}; }

static auto eligibility_ready() noexcept -> bool {
  return eligibility_api.can_upgrade && eligibility_api.options &&
         eligibility_api.upgrade_row && eligibility_api.resize_int_array &&
         eligibility_api.free_memory;
}

static auto eligibility_array(const CityArray &array) noexcept -> bool {
  return array.count >= 0 && array.capacity >= array.count &&
         array.capacity <= maximum_eligibility_entries &&
         (array.capacity == 0 || array.data != nullptr);
}

static auto eligibility_write(void *address, const void *value,
                              std::size_t size) noexcept -> bool {
  SIZE_T written{};
  return address != nullptr &&
         WriteProcessMemory(GetCurrentProcess(), address, value, size,
                            &written) != 0 &&
         written == size;
}

static auto clear_eligibility_reasons(CityArray *reasons) noexcept -> bool {
  CityArray array{};
  if (!city_read(reinterpret_cast<std::uintptr_t>(reasons), &array,
                 sizeof(array)) ||
      !eligibility_array(array))
    return false;
  // FName elements are trivial. Native canUpgrade also resets Num and keeps
  // the caller-owned allocation/capacity; freeing or replacing it would leak.
  const std::int32_t zero{};
  return eligibility_write(reinterpret_cast<std::uint8_t *>(reasons) + 8, &zero,
                           sizeof(zero));
}

static auto append_upgrade_option(CityArray *result, std::int32_t id) -> bool {
  CityArray array{};
  if (!city_read(reinterpret_cast<std::uintptr_t>(result), &array,
                 sizeof(array)) ||
      !eligibility_array(array))
    return false;
  for (std::int32_t i = 0; i < array.count; ++i) {
    std::int32_t existing{};
    if (!city_read(reinterpret_cast<std::uintptr_t>(array.data) +
                       static_cast<std::size_t>(i) * sizeof(existing),
                   &existing, sizeof(existing)))
      return false;
    if (existing == id)
      return true;
  }
  if (array.count == maximum_eligibility_entries)
    return false;
  const std::int32_t old_count = array.count;
  const std::int32_t new_count = old_count + 1;
  auto *count_address = reinterpret_cast<std::uint8_t *>(result) + 8;
  if (new_count > array.capacity) {
    // TArray<int32>::ResizeGrow expects Num already updated and old Num in EDX.
    if (!eligibility_write(count_address, &new_count, sizeof(new_count)))
      return false;
    try {
      eligibility_api.resize_int_array(result, old_count);
    } catch (...) {
      eligibility_write(count_address, &old_count, sizeof(old_count));
      return false;
    }
    if (!city_read(reinterpret_cast<std::uintptr_t>(result), &array,
                   sizeof(array)) ||
        !eligibility_array(array) || array.capacity < new_count) {
      eligibility_write(count_address, &old_count, sizeof(old_count));
      return false;
    }
  }
  if (!eligibility_write(static_cast<std::uint8_t *>(array.data) +
                             static_cast<std::size_t>(old_count) * sizeof(id),
                         &id, sizeof(id))) {
    eligibility_write(count_address, &old_count, sizeof(old_count));
    return false;
  }
  return eligibility_write(count_address, &new_count, sizeof(new_count));
}

auto city_upgrade_options(void *building, CityArray *result,
                          std::uint8_t include_extensions) -> CityArray * {
  if (result == nullptr || !eligibility_api.options)
    return result;
  if (!city_valid_object(building)) {
    const CityArray empty{};
    eligibility_write(result, &empty, sizeof(empty));
    return result;
  }
  if (!eligibility_ready() || !city_player_building(building))
    return eligibility_api.options(building, result, include_extensions);
  const auto address = reinterpret_cast<std::uintptr_t>(building);
  CityArray extensions{};
  void *game{}, *region{};
  std::int32_t type{}, artisan{};
  if (!city_read(address + 0x2e8, &game, sizeof(game)) ||
      !city_valid_object(game) ||
      !city_read(address + 0x2c8, &region, sizeof(region)) ||
      !city_valid_object(region) ||
      !city_read(address + 0xb88, &extensions, sizeof(extensions)) ||
      !eligibility_array(extensions) ||
      !city_read(address + 0x3b8, &type, sizeof(type)) ||
      !city_read(address + 0x14c4, &artisan, sizeof(artisan))) {
    const CityArray empty{};
    eligibility_write(result, &empty, sizeof(empty));
    return result;
  }
  std::int32_t extension_type{};
  if (extensions.count > 0 &&
      !city_read(reinterpret_cast<std::uintptr_t>(extensions.data),
                 &extension_type, sizeof(extension_type))) {
    const CityArray empty{};
    eligibility_write(result, &empty, sizeof(empty));
    return result;
  }
  CityArray *returned =
      eligibility_api.options(building, result, include_extensions);
  if (returned != result)
    return returned;
  if (extensions.count == 0) {
    // Same building-specific branches as native getAllPossibleUpgrades, with
    // only the perk gates omitted. Hunting-hound ordering is deliberately
    // absent.
    std::int32_t unlocked{};
    switch (type) {
    case 28:
      unlocked = 40;
      break;
    case 32:
      unlocked = 39;
      break;
    case 33:
      unlocked = 41;
      break;
    case 61:
      unlocked = 36;
      break;
    case 69:
      unlocked = 44;
      break;
    case 86:
      unlocked = 38;
      break;
    case 121:
      unlocked = 45;
      break;
    default:
      break;
    }
    if (unlocked != 0)
      append_upgrade_option(result, unlocked);
    return returned;
  }
  std::int32_t livestock{};
  std::uint8_t crop{};
  if (!city_read(address + 0x4a8, &livestock, sizeof(livestock)) ||
      !city_read(address + 0x4c8, &crop, sizeof(crop)))
    return returned;
  // Preserve native crop/livestock-choice and existing artisan structural
  // branches.
  if ((extension_type == 2 && livestock == 0) ||
      ((extension_type == 8 || extension_type == 12) && crop == 0))
    return returned;
  if (extension_type == 5 || extension_type == 13 || extension_type == 4) {
    append_upgrade_option(result, extension_type == 5    ? 37
                                  : extension_type == 13 ? 42
                                                         : 43);
    return returned;
  }
  if (artisan != 0)
    return returned;
  if (extension_type != 0)
    append_upgrade_option(result, 24);
  if (include_extensions != 0) {
    // Original level>1 and level>2 lists, retaining exact native type domains.
    for (const auto id :
         {109, 106, 111, 110, 105, 115, 113, 103, 116, 104, 107})
      if (!append_upgrade_option(result, id))
        return returned;
    if (extension_type == 0)
      for (const auto id : {108, 101, 102, 112, 114})
        if (!append_upgrade_option(result, id))
          return returned;
  }
  return returned;
}

auto city_can_upgrade(void *building, std::int32_t id, CityArray *reasons)
    -> bool {
  if (!city_valid_object(building))
    return false;
  const bool animal =
      id == 13 || id == 19 || id == 21 || id == 26 || id == 27 || id == 28;
  if (!city_player_building(building) || animal || id == 100 ||
      !eligibility_ready())
    return eligibility_api.can_upgrade
               ? eligibility_api.can_upgrade(building, id, reasons)
               : false;
  if (!clear_eligibility_reasons(reasons))
    return false;
  if (id != 2 && (id < 4 || id > 45) && (id < 101 || id > 116))
    return false;
  const auto address = reinterpret_cast<std::uintptr_t>(building);
  void *game{}, *region{};
  std::uint8_t constructed{}, upgrading{};
  CityArray applied{};
  if (!city_read(address + 0x2e8, &game, sizeof(game)) ||
      !city_valid_object(game) ||
      !city_read(address + 0x2c8, &region, sizeof(region)) ||
      !city_valid_object(region) ||
      !city_read(address + 0x3c1, &constructed, sizeof(constructed)) ||
      !city_read(address + 0x3a1, &upgrading, sizeof(upgrading)) ||
      constructed != 1 || upgrading != 0 ||
      !city_read(address + 0x430, &applied, sizeof(applied)) ||
      !eligibility_array(applied))
    return false;
  const auto row =
      reinterpret_cast<std::uintptr_t>(eligibility_api.upgrade_row(game, id));
  std::uint8_t once{};
  if (!city_read(row + 0x7f, &once, sizeof(once)) || once > 1)
    return false;
  if (once != 0)
    for (std::int32_t i = 0; i < applied.count; ++i) {
      std::int32_t existing{};
      if (!city_read(reinterpret_cast<std::uintptr_t>(applied.data) +
                         static_cast<std::size_t>(i) * sizeof(existing),
                     &existing, sizeof(existing)) ||
          existing == id)
        return false;
    }
  CityArray options{};
  const auto *returned = city_upgrade_options(building, &options, 1);
  bool allowed = false;
  if (returned == &options && eligibility_array(options))
    for (std::int32_t i = 0; i < options.count; ++i) {
      std::int32_t candidate{};
      if (!city_read(reinterpret_cast<std::uintptr_t>(options.data) +
                         static_cast<std::size_t>(i) * sizeof(candidate),
                     &candidate, sizeof(candidate)))
        break;
      if (candidate == id) {
        allowed = true;
        break;
      }
    }
  if (options.data)
    eligibility_api.free_memory(options.data);
  return allowed;
}
