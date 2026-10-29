#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "Patches.hpp"

struct FenceLifecycleFingerprint {
  std::uintptr_t rva{};
  std::vector<std::uint8_t> bytes;
};

auto fence_lifecycle_fingerprints()
    -> std::span<const FenceLifecycleFingerprint>;
auto fence_lifecycle_validate_image(const std::uint8_t *image) -> bool;
auto fence_lifecycle_make_finish_bridge(const void *callback,
                                        const void *continuation)
    -> std::vector<std::uint8_t>;

class FenceLifecycleHooks {
  PatchSet patches_;
  bool retained_{};

public:
  FenceLifecycleHooks() = default;
  FenceLifecycleHooks(const FenceLifecycleHooks &) = delete;
  auto operator=(const FenceLifecycleHooks &) -> FenceLifecycleHooks & = delete;
  ~FenceLifecycleHooks();
  auto install(std::uint8_t *image, const void *intersection_callback,
               const void *slice_callback, const void *finish_callback) -> bool;
  auto restore() -> bool;
  auto installed() const -> bool;
  auto retain_until_exit() -> void;
};
