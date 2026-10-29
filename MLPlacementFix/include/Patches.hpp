#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

struct CodePatch {
  std::uintptr_t rva{};
  std::vector<std::uint8_t> expected;
  std::vector<std::uint8_t> stub;
  bool call{};
};

// Only process-private executable pages are written. This code never opens the
// game executable or a save for writing. Installation happens before play
// starts.
class PatchSet {
  struct InstalledPatch {
    std::uint8_t *site{};
    std::vector<std::uint8_t> original;
    std::vector<std::uint8_t> replacement;
  };

  std::uint8_t *allocation_{};
  std::vector<InstalledPatch> installed_;

  static auto write(std::uint8_t *site, std::span<const std::uint8_t> bytes)
      -> bool {
    DWORD protection{};
    if (!VirtualProtect(site, bytes.size(), PAGE_EXECUTE_READWRITE,
                        &protection)) {
      return false;
    }
    std::memcpy(site, bytes.data(), bytes.size());
    const bool flushed =
        FlushInstructionCache(GetCurrentProcess(), site, bytes.size()) != 0;
    DWORD ignored{};
    const bool restored =
        VirtualProtect(site, bytes.size(), protection, &ignored) != 0;
    return flushed && restored;
  }

public:
  PatchSet() = default;
  PatchSet(const PatchSet &) = delete;
  auto operator=(const PatchSet &) -> PatchSet & = delete;

  auto installed() const -> bool { return !installed_.empty(); }

  // Callback destinations are pinned separately; keep executable stubs until
  // process exit even when the owning mod object is destroyed.
  auto retain_until_exit() -> void {
    allocation_ = nullptr;
    installed_.clear();
  }

  auto restore() -> bool {
    bool success = true;
    for (auto iterator = installed_.rbegin(); iterator != installed_.rend();
         ++iterator) {
      const auto &patch = *iterator;
      if (std::memcmp(patch.site, patch.original.data(),
                      patch.original.size()) == 0) {
        continue;
      }
      // Never overwrite a third party's subsequent change during removal.
      if (std::memcmp(patch.site, patch.replacement.data(),
                      patch.replacement.size()) != 0 ||
          !write(patch.site, patch.original)) {
        success = false;
      }
    }
    if (success) {
      installed_.clear();
      if (allocation_ != nullptr) {
        VirtualFree(allocation_, 0, MEM_RELEASE);
        allocation_ = nullptr;
      }
    }
    // On failure keep the stubs and the pinned DLL alive, avoiding stale jumps.
    return success;
  }

  auto install(std::uint8_t *image, std::span<const CodePatch> patches)
      -> bool {
    if (image == nullptr || patches.empty() || allocation_ != nullptr ||
        !installed_.empty()) {
      return false;
    }
    std::size_t required{};
    for (const auto &patch : patches) {
      if (patch.expected.size() < 5 || patch.stub.empty() ||
          std::memcmp(image + patch.rva, patch.expected.data(),
                      patch.expected.size()) != 0) {
        return false;
      }
      required += (patch.stub.size() + 15) & ~std::size_t{15};
    }
    if (required > 0x1000) {
      return false;
    }
    SYSTEM_INFO information{};
    GetSystemInfo(&information);
    const auto origin =
        reinterpret_cast<std::uintptr_t>(image + patches.front().rva) &
        ~(std::uintptr_t{information.dwAllocationGranularity} - 1);
    for (std::uintptr_t distance = information.dwAllocationGranularity;
         distance < 0x70000000 && allocation_ == nullptr;
         distance += information.dwAllocationGranularity) {
      for (const auto address : {origin + distance, origin - distance}) {
        allocation_ = static_cast<std::uint8_t *>(
            VirtualAlloc(reinterpret_cast<void *>(address), 0x1000,
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (allocation_ != nullptr) {
          break;
        }
      }
    }
    if (allocation_ == nullptr) {
      return false;
    }
    std::vector<InstalledPatch> prepared;
    std::size_t offset{};
    for (const auto &patch : patches) {
      auto *site = image + patch.rva;
      const auto displacement =
          reinterpret_cast<std::intptr_t>(allocation_ + offset) -
          reinterpret_cast<std::intptr_t>(site + 5);
      if (displacement < std::numeric_limits<std::int32_t>::min() ||
          displacement > std::numeric_limits<std::int32_t>::max()) {
        restore();
        return false;
      }
      std::memcpy(allocation_ + offset, patch.stub.data(), patch.stub.size());
      std::vector<std::uint8_t> replacement(patch.expected.size(), 0x90);
      replacement[0] = patch.call ? 0xe8 : 0xe9;
      const auto relative = static_cast<std::int32_t>(displacement);
      std::memcpy(replacement.data() + 1, &relative, sizeof(relative));
      prepared.push_back({site, patch.expected, std::move(replacement)});
      offset += (patch.stub.size() + 15) & ~std::size_t{15};
    }
    DWORD protection{};
    if (!VirtualProtect(allocation_, 0x1000, PAGE_EXECUTE_READ, &protection) ||
        !FlushInstructionCache(GetCurrentProcess(), allocation_, offset)) {
      restore();
      return false;
    }
    installed_.reserve(prepared.size());
    for (auto &patch : prepared) {
      installed_.push_back(std::move(patch));
      if (!write(installed_.back().site, installed_.back().replacement)) {
        restore();
        return false;
      }
    }
    return true;
  }
};

// Tail jump: preserve the CALL site's return address and Windows x64 ABI.
inline auto append_absolute_jump(std::vector<std::uint8_t> &code,
                                 const void *destination) -> void {
  const std::array<std::uint8_t, 6> jump{0xff, 0x25, 0, 0, 0, 0};
  code.insert(code.end(), jump.begin(), jump.end());
  const auto address = reinterpret_cast<std::uintptr_t>(destination);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
}
