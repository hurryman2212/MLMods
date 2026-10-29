#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <Windows.h>

#include "EraserRuntime.hpp"

// Corrupt container bounds are rejected before extending native erasure.
static constexpr std::int32_t maximum_regions = 256;
static constexpr std::int32_t maximum_components = 65536;
static constexpr std::int32_t maximum_tree_records = 1000000;

std::uintptr_t eraser_image{};
static EraserApi eraser_api{};

static auto eraser_read(std::uintptr_t address, void *output,
                        std::size_t size) noexcept -> bool {
  SIZE_T copied{};
  return address != 0 && output != nullptr && size != 0 &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), output,
                           size, &copied) != 0 &&
         copied == size;
}

static auto eraser_span(std::uintptr_t address, std::size_t size,
                        bool writable) noexcept -> bool {
  if (size == 0) {
    return true;
  }
  if (address == 0 ||
      size > std::numeric_limits<std::uintptr_t>::max() - address) {
    return false;
  }
  const auto end = address + size;
  while (address < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void *>(address), &info,
                     sizeof(info)) == 0 ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        (info.Protect &
         (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
          PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0 ||
        (writable && (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                      PAGE_EXECUTE_READWRITE |
                                      PAGE_EXECUTE_WRITECOPY)) == 0)) {
      return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    if (address < begin ||
        info.RegionSize > std::numeric_limits<std::uintptr_t>::max() - begin ||
        begin + info.RegionSize <= address) {
      return false;
    }
    address = begin + info.RegionSize;
  }
  return true;
}

static auto eraser_object_valid(void *object) noexcept -> bool {
  const auto address = reinterpret_cast<std::uintptr_t>(object);
  std::array<std::uint8_t, 24> header{};
  if (eraser_image == 0 || address % alignof(void *) != 0 ||
      !eraser_read(address, header.data(), header.size())) {
    return false;
  }
  std::uintptr_t vtable{}, object_class{}, class_vtable{};
  std::uint32_t flags{};
  std::int32_t index{};
  std::memcpy(&vtable, header.data(), sizeof(vtable));
  std::memcpy(&flags, header.data() + 8, sizeof(flags));
  std::memcpy(&index, header.data() + 12, sizeof(index));
  std::memcpy(&object_class, header.data() + 16, sizeof(object_class));
  return vtable >= eraser_image && vtable - eraser_image < 0x09deb000 &&
         (flags & 0x40018000U) == 0 && index > 0 &&
         eraser_read(object_class, &class_vtable, sizeof(class_vtable)) &&
         class_vtable >= eraser_image &&
         class_vtable - eraser_image < 0x09deb000;
}

static auto eraser_array(std::uintptr_t address, std::size_t stride,
                         std::int32_t limit, bool writable,
                         EraserArray &array) noexcept -> bool {
  if (!eraser_read(address, &array, sizeof(array)) || array.count < 0 ||
      array.capacity < array.count || array.capacity > limit ||
      (writable && !eraser_span(address, sizeof(array), true))) {
    return false;
  }
  const auto data = reinterpret_cast<std::uintptr_t>(array.data);
  const auto count = writable ? array.capacity : array.count;
  return (count == 0 || (data != 0 && data % alignof(void *) == 0)) &&
         eraser_span(data, static_cast<std::size_t>(count) * stride, writable);
}

static auto eraser_array_contains(std::uintptr_t address, void *object,
                                  std::int32_t limit) noexcept -> bool {
  EraserArray array{};
  if (!eraser_array(address, sizeof(void *), limit, false, array)) {
    return false;
  }
  const auto *entries = static_cast<void *const *>(array.data);
  for (std::int32_t i = 0; i < array.count; ++i) {
    if (entries[i] == object) {
      return true;
    }
  }
  return false;
}

static auto eraser_context(void *pawn, void *component, void *&region,
                           void *&navigation, EraserArray &trees) noexcept
    -> bool {
  const auto pawn_address = reinterpret_cast<std::uintptr_t>(pawn);
  const auto component_address = reinterpret_cast<std::uintptr_t>(component);
  void *master{}, *owner{}, *region_master{}, *main_pawn{}, *outer{};
  std::array<std::uint8_t, 2> flags{};
  std::uint8_t ready{}, tool{};
  if (eraser_api.queue_tree_removal == nullptr || !eraser_object_valid(pawn) ||
      !eraser_object_valid(component) ||
      !eraser_read(pawn_address + 0x34c, flags.data(), flags.size()) ||
      flags[0] != 1 || flags[1] != 0 ||
      !eraser_read(pawn_address + 0x12ec, &tool, sizeof(tool)) || tool != 1 ||
      !eraser_read(pawn_address + 0x340, &master, sizeof(master)) ||
      !eraser_object_valid(master) ||
      !eraser_read(pawn_address + 0x978, &region, sizeof(region)) ||
      !eraser_object_valid(region)) {
    return false;
  }
  const auto region_address = reinterpret_cast<std::uintptr_t>(region);
  const auto master_address = reinterpret_cast<std::uintptr_t>(master);
  if (!eraser_read(region_address + 0x350, &owner, sizeof(owner)) ||
      owner != pawn ||
      !eraser_read(region_address + 0x318, &region_master,
                   sizeof(region_master)) ||
      region_master != master ||
      !eraser_read(master_address + 0x318, &ready, sizeof(ready)) ||
      ready != 1 ||
      !eraser_read(master_address + 0x5b0, &main_pawn, sizeof(main_pawn)) ||
      main_pawn != pawn ||
      !eraser_array_contains(master_address + 0x560, region, maximum_regions) ||
      !eraser_read(component_address + 0x20, &outer, sizeof(outer)) ||
      outer != region ||
      !eraser_array_contains(region_address + 0xf38, component,
                             maximum_components) ||
      !eraser_array_contains(region_address + 0xf48, component,
                             maximum_components) ||
      !eraser_array(region_address + 0xcd0, 40, maximum_tree_records, true,
                    trees) ||
      !eraser_read(region_address + 0x738, &navigation, sizeof(navigation))) {
    return false;
  }
  for (std::int32_t i = 0; i < trees.count; ++i) {
    EraserVector point{};
    std::memcpy(&point,
                static_cast<const std::uint8_t *>(trees.data) +
                    static_cast<std::size_t>(i) * 40,
                sizeof(point));
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(point.z)) {
      return false;
    }
  }
  if (navigation != nullptr) {
    EraserArray queued{};
    const auto navigation_address =
        reinterpret_cast<std::uintptr_t>(navigation);
    if (!eraser_object_valid(navigation) ||
        !eraser_read(navigation_address + 0x20, &outer, sizeof(outer)) ||
        outer != region ||
        !eraser_array(navigation_address + 0x2d8, sizeof(EraserVector),
                      maximum_tree_records, true, queued)) {
      return false;
    }
  }
  return true;
}

auto configure_eraser_api(const EraserApi &api) noexcept -> void {
  eraser_api = api;
}

auto eraser_allow_tree(void *pawn, void *component) noexcept -> bool {
  void *region{}, *navigation{};
  EraserArray trees{};
  return eraser_context(pawn, component, region, navigation, trees);
}

auto eraser_cleanup(void *pawn, void *component, const EraserArray *removed,
                    bool success) noexcept -> void {
  void *region{}, *navigation{};
  EraserArray trees{}, positions{};
  if (!success || !eraser_context(pawn, component, region, navigation, trees) ||
      !eraser_array(reinterpret_cast<std::uintptr_t>(removed),
                    sizeof(EraserVector), maximum_tree_records, false,
                    positions)) {
    return;
  }
  const auto *points = static_cast<const EraserVector *>(positions.data);
  for (std::int32_t i = 0; i < positions.count; ++i) {
    if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y) ||
        !std::isfinite(points[i].z)) {
      return;
    }
  }
  auto *records = static_cast<std::uint8_t *>(trees.data);
  std::int32_t index{};
  while (index < trees.count) {
    auto *record = records + static_cast<std::size_t>(index) * 40;
    EraserVector point{};
    std::memcpy(&point, record, sizeof(point));
    bool matched = false;
    for (std::int32_t i = 0; i < positions.count; ++i) {
      // Native road clearing uses this float-derived tolerance on all axes.
      if (std::abs(point.x - points[i].x) <= 9.999999747378752e-05 &&
          std::abs(point.y - points[i].y) <= 9.999999747378752e-05 &&
          std::abs(point.z - points[i].z) <= 9.999999747378752e-05) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      ++index;
      continue;
    }
    if (navigation != nullptr) {
      eraser_api.queue_tree_removal(navigation, record);
    }
    std::memmove(record, record + 40,
                 static_cast<std::size_t>(trees.count - index - 1) * 40);
    --trees.count;
    std::memcpy(reinterpret_cast<void *>(
                    reinterpret_cast<std::uintptr_t>(region) + 0xcd8),
                &trees.count, sizeof(trees.count));
  }
}
