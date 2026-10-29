#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>

#include "FenceLifecycle.hpp"

static constexpr std::int32_t FenceMaximumArrayCapacity = 1000000;
static constexpr std::size_t FenceMaximumNestedFailures = 64;
static constexpr std::size_t FenceReadProbeBytes = 4096;

struct FenceIdentity {
  std::uintptr_t object{};
  std::array<std::uint8_t, 24> header{};
};

struct FenceFailure {
  FenceIdentity owner{};
  const void *frame{};
};

static FenceLifecycleApi fence_api;
static std::atomic<bool> fence_enabled{};
static std::atomic<std::uint64_t> fence_cleanups{};
static thread_local std::array<FenceFailure, FenceMaximumNestedFailures>
    fence_failures{};

auto configure_fence_lifecycle_api(const FenceLifecycleApi &api) noexcept
    -> void {
  fence_enabled.store(false, std::memory_order_release);
  fence_api = api;
  fence_cleanups.store(0, std::memory_order_relaxed);
  fence_failures = {};
}

auto set_fence_lifecycle_enabled(bool enabled) noexcept -> void {
  fence_enabled.store(enabled, std::memory_order_release);
}

auto fence_lifecycle_cleanup_count() noexcept -> std::uint64_t {
  return fence_cleanups.load(std::memory_order_relaxed);
}

static auto fence_read(std::uintptr_t base, std::size_t offset, void *output,
                       std::size_t size) noexcept -> bool {
  return fence_api.read_memory != nullptr && base != 0 && output != nullptr &&
         size != 0 &&
         offset <= std::numeric_limits<std::uintptr_t>::max() - base &&
         size <= std::numeric_limits<std::uintptr_t>::max() - base - offset &&
         fence_api.read_memory(base + offset, output, size);
}

static auto fence_identity(std::uintptr_t object,
                           FenceIdentity &identity) noexcept -> bool {
  identity.object = object;
  if (object % alignof(void *) != 0 ||
      !fence_read(object, 0, identity.header.data(), identity.header.size()))
    return false;
  std::uintptr_t vtable{}, object_class{}, class_vtable{};
  std::uint32_t flags{};
  std::int32_t index{};
  std::memcpy(&vtable, identity.header.data(), sizeof(vtable));
  std::memcpy(&flags, identity.header.data() + 8, sizeof(flags));
  std::memcpy(&index, identity.header.data() + 12, sizeof(index));
  std::memcpy(&object_class, identity.header.data() + 16, sizeof(object_class));
  return fence_api.image != 0 && vtable >= fence_api.image &&
         vtable - fence_api.image < 0x09dfd000 && (flags & 0x40018010U) == 0 &&
         index > 0 && object_class != 0 &&
         object_class % alignof(void *) == 0 &&
         fence_read(object_class, 0, &class_vtable, sizeof(class_vtable)) &&
         class_vtable >= fence_api.image &&
         class_vtable - fence_api.image < 0x09dfd000;
}

static auto fence_owner(std::uintptr_t object, FenceIdentity &identity) noexcept
    -> bool {
  std::uintptr_t vtable{};
  return fence_identity(object, identity) &&
         fence_read(object, 0, &vtable, sizeof(vtable)) &&
         vtable == fence_api.image + 0x7f51b90;
}

static auto fence_same(const FenceIdentity &identity) noexcept -> bool {
  FenceIdentity current{};
  return fence_identity(identity.object, current) &&
         current.header == identity.header;
}

static auto fence_active() noexcept -> bool {
  return fence_enabled.load(std::memory_order_acquire) &&
         fence_api.on_game_thread != nullptr && fence_api.on_game_thread();
}

static auto fence_clear_old(const FenceIdentity &owner) noexcept -> void {
  if (fence_api.write_memory == nullptr ||
      fence_api.clear_instances == nullptr ||
      fence_api.remove_fence_refs == nullptr || !fence_same(owner))
    return;
  std::uintptr_t component{}, master{}, grid{}, outer{}, clear_function{},
      component_vtable{};
  FenceIdentity component_identity{}, master_identity{};
  std::array<FenceLifecycleArray, 3> arrays{};
  if (!fence_read(owner.object, 0xf38, &component, sizeof(component)) ||
      component == 0 || !fence_identity(component, component_identity) ||
      !fence_read(component, 0x20, &outer, sizeof(outer)) ||
      outer != owner.object ||
      !fence_read(component, 0, &component_vtable, sizeof(component_vtable)) ||
      !fence_read(component_vtable, 0xba8, &clear_function,
                  sizeof(clear_function)) ||
      clear_function < fence_api.image ||
      clear_function - fence_api.image >= 0x09dfd000 ||
      !fence_read(owner.object, 0x2e8, &master, sizeof(master)) ||
      !fence_identity(master, master_identity) ||
      !fence_read(master, 0x2c0, &grid, sizeof(grid)) || grid == 0 ||
      grid % alignof(void *) != 0 ||
      !fence_read(owner.object, 0xf40, arrays.data(), sizeof(arrays)))
    return;
  std::array<std::uint8_t, 8> grid_probe{};
  if (!fence_read(grid, 0, grid_probe.data(), grid_probe.size()))
    return;
  bool has_segments = false;
  std::array<std::uint8_t, FenceReadProbeBytes> probe{};
  for (std::size_t i{}; i < arrays.size(); ++i) {
    const auto &array = arrays[i];
    const std::size_t stride = i == 0 ? 32 : 4;
    if (array.count < 0 || array.capacity < array.count ||
        array.capacity > FenceMaximumArrayCapacity ||
        (array.capacity != 0 &&
         (array.data == 0 || array.data % (i == 0 ? 8 : 4) != 0)) ||
        static_cast<std::size_t>(array.capacity) >
            (std::numeric_limits<std::uintptr_t>::max() - array.data) / stride)
      return;
    const auto occupied = static_cast<std::size_t>(array.count) * stride;
    for (std::size_t offset{}; offset < occupied;) {
      const auto size = std::min(probe.size(), occupied - offset);
      if (!fence_read(array.data, offset, probe.data(), size))
        return;
      offset += size;
    }
    has_segments = has_segments || array.count != 0;
  }
  if (!has_segments)
    return;
  FenceIdentity owner_now{};
  std::array<FenceLifecycleArray, 3> unchanged{};
  if (!fence_owner(owner.object, owner_now) ||
      owner_now.header != owner.header || !fence_same(component_identity) ||
      !fence_same(master_identity) ||
      !fence_read(owner.object, 0xf40, unchanged.data(), sizeof(unchanged)) ||
      std::memcmp(arrays.data(), unchanged.data(), sizeof(arrays)) != 0)
    return;

  // Exact reset phase of native 0x144d51bf0: clear HISM instances, remove
  // grid references, then Empty() the three transient fence arrays. Their
  // positive capacities need no reallocations, exactly as in the native code.
  // Do not run its segment builder on a fabricated zero-length polyline.
  fence_api.clear_instances(reinterpret_cast<void *>(component));
  fence_api.remove_fence_refs(reinterpret_cast<void *>(grid),
                              reinterpret_cast<void *>(owner.object));
  if (!fence_same(owner) || !fence_same(component_identity) ||
      !fence_read(owner.object, 0xf40, unchanged.data(), sizeof(unchanged)) ||
      std::memcmp(arrays.data(), unchanged.data(), sizeof(arrays)) != 0)
    return;
  for (auto &array : arrays)
    array.count = 0;
  if (fence_api.write_memory(owner.object + 0xf40, arrays.data(),
                             sizeof(arrays)))
    fence_cleanups.fetch_add(1, std::memory_order_relaxed);
}

auto fence_lifecycle_observe_intersection(
    FenceLifecycleArray *output, const FenceLifecycleArray *boundary,
    const FenceLifecycleVec3 *line, const FenceLifecycleVec3 *reference,
    std::uint8_t prune_to_two, std::uint8_t extend) -> FenceLifecycleArray * {
  if (fence_api.intersect == nullptr)
    return output;
  auto *result = fence_api.intersect(output, boundary, line, reference,
                                     prune_to_two, extend);
  if (!fence_active() || result != output || output == nullptr ||
      prune_to_two != 1 || extend != 1)
    return result;
  const auto polygon = reinterpret_cast<std::uintptr_t>(boundary);
  FenceIdentity owner{};
  FenceLifecycleArray intersection{};
  if (polygon < 0x660 || !fence_owner(polygon - 0x660, owner) ||
      !fence_read(reinterpret_cast<std::uintptr_t>(output), 0, &intersection,
                  sizeof(intersection)) ||
      intersection.count < 0 || intersection.capacity < intersection.count)
    return result;
  FenceFailure *available = nullptr;
  for (auto &failure : fence_failures) {
    if (failure.owner.object == owner.object && failure.frame == output) {
      failure = {};
      available = &failure;
    } else if (failure.owner.object == 0 && available == nullptr) {
      available = &failure;
    }
  }
  if (intersection.count != 2 && available != nullptr)
    *available = {owner, output};
  return result;
}

auto fence_lifecycle_slice(const FenceLifecycleArray *boundary,
                           const FenceLifecycleArray *corners, const void *line,
                           FenceLifecycleArray *first,
                           FenceLifecycleArray *second,
                           FenceLifecycleArray *third,
                           FenceLifecycleArray *output_corners,
                           const void *reference) -> void {
  if (fence_api.slice == nullptr)
    return;
  fence_api.slice(boundary, corners, line, first, second, third, output_corners,
                  reference);
  if (!fence_active())
    return;
  const auto polygon = reinterpret_cast<std::uintptr_t>(boundary);
  FenceIdentity owner{};
  FenceLifecycleArray result{};
  if (polygon < 0x660 || !fence_owner(polygon - 0x660, owner) ||
      reinterpret_cast<std::uintptr_t>(corners) != owner.object + 0x6a8 ||
      !fence_read(reinterpret_cast<std::uintptr_t>(output_corners), 0, &result,
                  sizeof(result)) ||
      result.count < 0 || result.count >= 4 || result.capacity < result.count)
    return;
  fence_clear_old(owner);
}

auto fence_lifecycle_finish(void *owner, const void *frame_cookie) noexcept
    -> void {
  const auto address = reinterpret_cast<std::uintptr_t>(owner);
  for (auto &failure : fence_failures) {
    if (failure.owner.object == address && failure.frame == frame_cookie) {
      const auto identity = failure.owner;
      failure = {};
      if (fence_active())
        fence_clear_old(identity);
    }
  }
}
