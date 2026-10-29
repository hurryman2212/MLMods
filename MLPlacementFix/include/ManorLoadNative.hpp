#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

struct ManorLoadFingerprint {
  std::uintptr_t rva{};
  std::vector<std::uint8_t> bytes;
};

auto manor_load_fingerprints() -> std::span<const ManorLoadFingerprint>;
auto manor_load_validate_image(const std::uint8_t *image) -> bool;
auto manor_load_read(std::uintptr_t address, void *output,
                     std::size_t size) noexcept -> bool;

// Three Manor loop sites. No executable/save-file writes.
class ManorLoadHook final {
  struct Site {
    std::uint8_t *address{};
    std::vector<std::uint8_t> original;
    std::vector<std::uint8_t> replacement;
  };
  std::array<Site, 3> sites_{};
  std::uint8_t *stub_{};

public:
  ManorLoadHook() = default;
  ~ManorLoadHook();
  ManorLoadHook(const ManorLoadHook &) = delete;
  auto operator=(const ManorLoadHook &) -> ManorLoadHook & = delete;
  auto installed() const noexcept -> bool {
    return sites_[0].address != nullptr || sites_[1].address != nullptr ||
           sites_[2].address != nullptr;
  }
  auto retain_until_exit() noexcept -> void {
    sites_ = {};
    stub_ = nullptr;
  }
  auto install(std::uint8_t *image, const void *route_callback,
               const void *begin_callback, const void *end_callback) -> bool;
  auto restore() -> bool;
};
