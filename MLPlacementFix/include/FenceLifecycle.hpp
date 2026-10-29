#pragma once

#include <cstddef>
#include <cstdint>

struct FenceLifecycleVec3 {
  double x{}, y{}, z{};
};

struct FenceLifecycleArray {
  std::uintptr_t data{};
  std::int32_t count{}, capacity{};
};

static_assert(sizeof(FenceLifecycleVec3) == 24);
static_assert(sizeof(FenceLifecycleArray) == 16);

struct FenceLifecycleApi {
  std::uintptr_t image{};
  bool (*read_memory)(std::uintptr_t, void *, std::size_t) noexcept {};
  bool (*write_memory)(std::uintptr_t, const void *, std::size_t) noexcept {};
  bool (*on_game_thread)() noexcept {};
  FenceLifecycleArray *(*intersect)(FenceLifecycleArray *,
                                    const FenceLifecycleArray *,
                                    const FenceLifecycleVec3 *,
                                    const FenceLifecycleVec3 *, std::uint8_t,
                                    std::uint8_t){};
  void (*slice)(const FenceLifecycleArray *, const FenceLifecycleArray *,
                const void *, FenceLifecycleArray *, FenceLifecycleArray *,
                FenceLifecycleArray *, FenceLifecycleArray *, const void *){};
  void (*clear_instances)(void *){};
  void (*remove_fence_refs)(void *, void *){};
};

auto configure_fence_lifecycle_api(const FenceLifecycleApi &api) noexcept
    -> void;
auto set_fence_lifecycle_enabled(bool enabled) noexcept -> void;
auto fence_lifecycle_cleanup_count() noexcept -> std::uint64_t;

// Observe the original result only. Geometry, allocation and ordering stay
// entirely native; no intersection or placement inputs are substituted.
auto fence_lifecycle_observe_intersection(
    FenceLifecycleArray *output, const FenceLifecycleArray *boundary,
    const FenceLifecycleVec3 *line, const FenceLifecycleVec3 *reference,
    std::uint8_t prune_to_two, std::uint8_t extend) -> FenceLifecycleArray *;
auto fence_lifecycle_slice(const FenceLifecycleArray *boundary,
                           const FenceLifecycleArray *corners, const void *line,
                           FenceLifecycleArray *first,
                           FenceLifecycleArray *second,
                           FenceLifecycleArray *third,
                           FenceLifecycleArray *output_corners,
                           const void *reference) -> void;
// The common native house tail executes even when fence generation is skipped.
// Its frame cookie is the original second-intersection output at RBP+0x340.
auto fence_lifecycle_finish(void *owner, const void *frame_cookie) noexcept
    -> void;
