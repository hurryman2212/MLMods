#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "TreePatches.hpp"

struct TreeFingerprint {
  std::uintptr_t rva{};
  std::vector<std::uint8_t> bytes;
};

auto tree_fingerprints() -> std::span<const TreeFingerprint>;
auto tree_validate_image(const std::uint8_t *image) -> bool;
auto tree_region_bridge(bool component, const void *callback,
                        const void *resume) -> std::vector<std::uint8_t>;
auto tree_make_patches() -> std::vector<TreeCodePatch>;
