#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "Patches.hpp"

enum class PlacementBridgeKind { Load, Finish, Extension, Visual };

struct PlacementFingerprint {
  std::uintptr_t rva{};
  std::vector<std::uint8_t> bytes;
};

auto placement_fingerprints() -> std::span<const PlacementFingerprint>;
auto placement_validate_image(const std::uint8_t *image) -> bool;
auto placement_make_bridge(const void *callback, const void *continuation,
                           PlacementBridgeKind kind)
    -> std::vector<std::uint8_t>;

class PlacementHooks {
  PatchSet patches_;
  bool retained_{};

public:
  PlacementHooks() = default;
  PlacementHooks(const PlacementHooks &) = delete;
  auto operator=(const PlacementHooks &) -> PlacementHooks & = delete;
  ~PlacementHooks();

  auto install(std::uint8_t *image, const void *world_callback,
               const void *owner_callback, const void *assembly_callback,
               const void *artisan_callback, const void *primary_callback,
               const void *secondary_callback, const void *hover_callback)
      -> bool;
  auto installed() const -> bool { return patches_.installed(); }
  auto retain_until_exit() -> void { retained_ = true; }
};
