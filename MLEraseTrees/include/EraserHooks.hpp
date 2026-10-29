#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "EraserPatches.hpp"

struct EraserFingerprint {
  std::uintptr_t rva{};
  std::vector<std::uint8_t> bytes;
};

auto eraser_fingerprints() -> std::span<const EraserFingerprint>;
auto eraser_validate_image(const std::uint8_t *image) -> bool;
auto eraser_filter_bridge(const void *callback, const void *allow,
                          const void *skip, const void *native_non_tree)
    -> std::vector<std::uint8_t>;
auto eraser_cleanup_bridge(const void *callback, const void *resume)
    -> std::vector<std::uint8_t>;
auto eraser_make_patches() -> std::vector<EraserCodePatch>;
