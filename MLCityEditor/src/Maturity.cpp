#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <Windows.h>

#include "CityRuntime.hpp"
#include "Maturity.hpp"

// Defensive limits, not gameplay limits; invalid containers fail unchanged.
static constexpr std::int32_t maximum_extensions = 1024;
static constexpr std::int32_t maximum_orchard_plants = 1048576;
static MaturityApi maturity_api{};

auto configure_maturity(const MaturityApi &api) noexcept -> void {
  maturity_api = api;
}

static auto maturity_array(void *building, std::size_t offset,
                           std::int32_t limit, CityArray &header) -> bool {
  if (!city_read(reinterpret_cast<std::uintptr_t>(building) + offset, &header,
                 sizeof(header)) ||
      header.count < 0 || header.count > limit ||
      header.capacity < header.count ||
      header.capacity > std::numeric_limits<std::int32_t>::max() / 0xd0)
    return false;
  const auto data = reinterpret_cast<std::uintptr_t>(header.data);
  const auto size = static_cast<std::size_t>(header.count) * 0xd0;
  return (header.count == 0 || data != 0) && data % alignof(void *) == 0 &&
         data <= std::numeric_limits<std::uintptr_t>::max() - size;
}

auto city_mature_new_orchard(void *building, std::int32_t upgrade_id) -> bool {
  if (upgrade_id != 112 && (upgrade_id < 32 || upgrade_id > 34))
    return true;
  if (!city_player_building(building))
    return true;
  const auto address = reinterpret_cast<std::uintptr_t>(building);
  std::uint8_t constructed{}, upgrading{}, crop{};
  if (!city_read(address + 0x3c1, &constructed, sizeof(constructed)) ||
      !city_read(address + 0x3a1, &upgrading, sizeof(upgrading)) ||
      !city_read(address + 0x4c8, &crop, sizeof(crop)) || constructed != 1 ||
      upgrading != 0)
    return false;
  // Extension 112 starts with crop=0 until the player chooses a variety.
  // Never choose one automatically, or mature a later unrelated crop.
  if ((crop != 9 && crop != 13 && crop != 14) ||
      (upgrade_id == 32 && crop != 14) || (upgrade_id == 33 && crop != 9) ||
      (upgrade_id == 34 && crop != 13))
    return true;
  CityArray extensions{};
  if (!maturity_array(building, 0xb88, maximum_extensions, extensions))
    return false;
  bool orchard{};
  for (std::int32_t i = 0; i < extensions.count; ++i) {
    std::int32_t type{};
    if (!city_read(reinterpret_cast<std::uintptr_t>(extensions.data) +
                       static_cast<std::size_t>(i) * 0xd0,
                   &type, sizeof(type)))
      return false;
    orchard = orchard || type == 12;
  }
  if (!orchard)
    return true;
  if (!maturity_api.refresh_visuals || !maturity_api.refresh_progress)
    return false;
  CityArray plants{};
  if (!maturity_array(building, 0x530, maximum_orchard_plants, plants))
    return false;
  if (plants.count == 0)
    return true;
  const auto data = reinterpret_cast<std::uintptr_t>(plants.data);
  const auto size = static_cast<std::size_t>(plants.count) * 0xd0;
  // Validate the entire allocation and every plant before changing any. This
  // runs inside the native game-thread callback after assembly has completed.
  for (auto cursor = data; cursor < data + size;) {
    MEMORY_BASIC_INFORMATION page{};
    if (VirtualQuery(reinterpret_cast<void *>(cursor), &page, sizeof(page)) ==
            0 ||
        page.State != MEM_COMMIT ||
        (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        (page.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
      return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(page.BaseAddress);
    if (begin > cursor ||
        page.RegionSize > std::numeric_limits<std::uintptr_t>::max() - begin ||
        begin + page.RegionSize <= cursor)
      return false;
    cursor = begin + page.RegionSize;
  }
  std::vector<std::array<float, 2>> previous;
  try {
    previous.resize(static_cast<std::size_t>(plants.count));
  } catch (...) {
    return false;
  }
  bool changed{};
  for (std::int32_t i = 0; i < plants.count; ++i) {
    const auto entry = data + static_cast<std::size_t>(i) * 0xd0;
    std::uint8_t type{};
    auto &progress = previous[static_cast<std::size_t>(i)];
    if (!city_read(entry, &type, sizeof(type)) || type != crop ||
        !city_read(entry + 0x20, progress.data(), sizeof(progress)))
      return false;
    for (const float value : progress) {
      if (!std::isfinite(value) || value < 0.0F || value > 1.0F)
        return false;
      changed = changed || value < 1.0F;
    }
  }
  if (!changed)
    return true;
  CityArray current{};
  if (!city_player_building(building) ||
      !city_read(address + 0x530, &current, sizeof(current)) ||
      current.data != plants.data || current.count != plants.count ||
      current.capacity != plants.capacity)
    return false;
  constexpr std::array<float, 2> mature{1.0F, 1.0F};
  for (std::int32_t i = 0; i < plants.count; ++i) {
    SIZE_T written{};
    if (!WriteProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<void *>(data + static_cast<std::size_t>(i) * 0xd0 +
                                     0x20),
            mature.data(), sizeof(mature), &written) ||
        written != sizeof(mature)) {
      // Restore the original progress if a write unexpectedly fails after the
      // page preflight. No fruit, date, transform or inventory fields are
      // copied.
      for (std::int32_t undo = 0; undo <= i; ++undo) {
        WriteProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<void *>(
                data + static_cast<std::size_t>(undo) * 0xd0 + 0x20),
            previous[static_cast<std::size_t>(undo)].data(), sizeof(mature),
            &written);
      }
      return false;
    }
  }
  // Native progress caches (not harvest state), then native mature-tree
  // rendering. Existing yield at FPlant+0x4c and all inventory remain
  // unchanged.
  maturity_api.refresh_progress(building, 1, 1, 1, 0);
  maturity_api.refresh_visuals(building);
  return true;
}
