#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <Windows.h>

#include "TreeRuntime.hpp"

// Bounds reject corrupt runtime containers without any allocation in hooks.
static constexpr std::int32_t maximum_regions = 256;
static constexpr std::int32_t maximum_foliage_components = 65536;

std::uintptr_t tree_image{};
static TreeApi tree_api{};

static auto tree_read(std::uintptr_t address, void *output,
                      std::size_t size) noexcept -> bool {
  SIZE_T copied{};
  return address != 0 && output != nullptr && size != 0 &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), output,
                           size, &copied) != 0 &&
         copied == size;
}

static auto tree_object_valid(void *object) noexcept -> bool {
  const auto address = reinterpret_cast<std::uintptr_t>(object);
  std::array<std::uint8_t, 24> header{};
  if (tree_image == 0 || address % alignof(void *) != 0 ||
      !tree_read(address, header.data(), header.size())) {
    return false;
  }
  std::uintptr_t vtable{}, object_class{}, class_vtable{};
  std::uint32_t flags{};
  std::int32_t index{};
  std::memcpy(&vtable, header.data(), sizeof(vtable));
  std::memcpy(&flags, header.data() + 8, sizeof(flags));
  std::memcpy(&index, header.data() + 12, sizeof(index));
  std::memcpy(&object_class, header.data() + 16, sizeof(object_class));
  return vtable >= tree_image && vtable - tree_image < 0x09dfd000 &&
         (flags & 0x40018000U) == 0 && index > 0 &&
         tree_read(object_class, &class_vtable, sizeof(class_vtable)) &&
         class_vtable >= tree_image && class_vtable - tree_image < 0x09dfd000;
}

static auto tree_array_contains(std::uintptr_t address, void *object,
                                std::int32_t limit) noexcept -> bool {
  TreeArray array{};
  if (!tree_read(address, &array, sizeof(array)) || array.count <= 0 ||
      array.count > limit || array.capacity < array.count ||
      array.capacity > std::numeric_limits<std::int32_t>::max() /
                           static_cast<std::int32_t>(sizeof(void *))) {
    return false;
  }
  const auto data = reinterpret_cast<std::uintptr_t>(array.data);
  const auto bytes = static_cast<std::size_t>(array.count) * sizeof(void *);
  if (data == 0 || data % alignof(void *) != 0 ||
      data > std::numeric_limits<std::uintptr_t>::max() - bytes) {
    return false;
  }
  for (std::int32_t i = 0; i < array.count; ++i) {
    void *entry{};
    if (!tree_read(data + static_cast<std::size_t>(i) * sizeof(void *), &entry,
                   sizeof(entry))) {
      return false;
    }
    if (entry == object) {
      return true;
    }
  }
  return false;
}

static auto tree_context(void *unit, void *original, void *&master) noexcept
    -> bool {
  const auto unit_address = reinterpret_cast<std::uintptr_t>(unit);
  const auto region_address = reinterpret_cast<std::uintptr_t>(original);
  void *owner{}, *assigned{}, *region_master{}, *region_owner{}, *main_pawn{};
  std::array<std::uint8_t, 2> owner_flags{};
  std::uint8_t ready{};
  return tree_api.region_at != nullptr && tree_object_valid(unit) &&
         tree_object_valid(original) &&
         tree_read(unit_address + 0x318, &master, sizeof(master)) &&
         tree_object_valid(master) &&
         tree_read(unit_address + 0x320, &owner, sizeof(owner)) &&
         tree_object_valid(owner) &&
         tree_read(unit_address + 0x328, &assigned, sizeof(assigned)) &&
         assigned == original &&
         tree_read(region_address + 0x318, &region_master,
                   sizeof(region_master)) &&
         region_master == master &&
         tree_read(region_address + 0x350, &region_owner,
                   sizeof(region_owner)) &&
         region_owner == owner &&
         tree_read(reinterpret_cast<std::uintptr_t>(owner) + 0x34c,
                   owner_flags.data(), owner_flags.size()) &&
         owner_flags[0] == 1 && owner_flags[1] == 0 &&
         tree_read(reinterpret_cast<std::uintptr_t>(master) + 0x318, &ready,
                   sizeof(ready)) &&
         ready == 1 &&
         tree_read(reinterpret_cast<std::uintptr_t>(master) + 0x5b0, &main_pawn,
                   sizeof(main_pawn)) &&
         main_pawn == owner &&
         tree_array_contains(reinterpret_cast<std::uintptr_t>(master) + 0x560,
                             original, maximum_regions);
}

static auto tree_world_region(void *region, void *master) noexcept -> bool {
  void *region_master{};
  const auto address = reinterpret_cast<std::uintptr_t>(region);
  return tree_object_valid(region) &&
         tree_read(address + 0x318, &region_master, sizeof(region_master)) &&
         region_master == master &&
         tree_array_contains(reinterpret_cast<std::uintptr_t>(master) + 0x560,
                             region, maximum_regions);
}

auto configure_tree_api(const TreeApi &api) noexcept -> void { tree_api = api; }

auto tree_select_region(void *unit, void *original) noexcept -> void * {
  void *master{};
  TreeVector point{};
  if (!tree_context(unit, original, master) ||
      !tree_read(reinterpret_cast<std::uintptr_t>(unit) + 0x4e8, &point.x,
                 2 * sizeof(double)) ||
      !std::isfinite(point.x) || !std::isfinite(point.y)) {
    return original;
  }
  // The original planting-position getter only adjusts Z. Query its exact XY
  // without invoking that stateful getter an additional time.
  try {
    void *region = tree_api.region_at(master, &point);
    // Registration follows geography even across an ownership boundary. The
    // native planting-task eligibility check has already selected the target.
    return tree_world_region(region, master) ? region : original;
  } catch (...) {
    return original;
  }
}

auto tree_component_region(void *unit, void *component, void *original) noexcept
    -> void * {
  void *master{}, *region{};
  if (!tree_context(unit, original, master) || !tree_object_valid(component) ||
      !tree_read(reinterpret_cast<std::uintptr_t>(component) + 0x20, &region,
                 sizeof(region)) ||
      !tree_world_region(region, master) ||
      !tree_array_contains(reinterpret_cast<std::uintptr_t>(region) + 0xf38,
                           component, maximum_foliage_components)) {
    return original;
  }
  // NewObject uses the Region as Outer, and native creation registers this
  // exact component in regionalFoliage. Use that owner, not a new position
  // query or a temporary mutation of the worker's assigned Region.
  return region;
}
