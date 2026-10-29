#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "CityRuntime.hpp"

struct CityCodePatch {
  std::uintptr_t rva{};
  std::vector<std::uint8_t> expected;
  std::vector<std::uint8_t> stub;
  bool call{};
  std::uintptr_t *trampoline{};
  std::size_t trampoline_offset{};
};

inline auto city_relative_displacement(std::uintptr_t from,
                                       std::uintptr_t destination)
    -> std::optional<std::int32_t> {
  if (destination >= from) {
    const auto distance = destination - from;
    if (distance >
        static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::int32_t>(distance);
  }
  const auto distance = from - destination;
  if (distance >
      static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max()) +
          1) {
    return std::nullopt;
  }
  return static_cast<std::int32_t>(-static_cast<std::int64_t>(distance));
}

inline auto append_city_jump(std::vector<std::uint8_t> &code,
                             const void *destination) -> void {
  code.insert(code.end(), {0xff, 0x25, 0, 0, 0, 0});
  const auto address = reinterpret_cast<std::uintptr_t>(destination);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
}

inline auto make_city_call_patch(std::uintptr_t rva,
                                 std::uintptr_t original_rva,
                                 const void *callback) -> CityCodePatch {
  if (callback == nullptr ||
      rva > std::numeric_limits<std::uintptr_t>::max() - 5) {
    return {};
  }
  const auto relative = city_relative_displacement(rva + 5, original_rva);
  if (!relative.has_value()) {
    return {};
  }
  CityCodePatch patch{rva, std::vector<std::uint8_t>(5), {}, true};
  patch.expected[0] = 0xe8;
  std::memcpy(patch.expected.data() + 1, &*relative, sizeof(*relative));
  append_city_jump(patch.stub, callback);
  return patch;
}

inline auto make_city_entry_patch(std::uintptr_t rva,
                                  std::span<const std::uint8_t> prefix,
                                  const void *callback,
                                  std::uintptr_t *trampoline) -> CityCodePatch {
  if (callback == nullptr || trampoline == nullptr || prefix.size() < 5 ||
      prefix.size() > 0x1000 - 28 || city_image == 0 ||
      rva > std::numeric_limits<std::uintptr_t>::max() - city_image ||
      prefix.size() >
          std::numeric_limits<std::uintptr_t>::max() - city_image - rva) {
    return {};
  }
  CityCodePatch patch{
      rva, {prefix.begin(), prefix.end()}, {}, false, trampoline, 14};
  append_city_jump(patch.stub, callback);
  // The caller provides an audited, whole-instruction, relocation-free prefix.
  patch.stub.insert(patch.stub.end(), prefix.begin(), prefix.end());
  append_city_jump(patch.stub, reinterpret_cast<const void *>(city_image + rva +
                                                              prefix.size()));
  return patch;
}

// The containing DLL must be pinned before installation. If somebody replaces
// a patched site, restoration retains this allocation and its trampoline slots
// rather than freeing memory still reachable through another hook.
class CityPatchSet final {
  struct InstalledPatch {
    std::uint8_t *site{};
    std::vector<std::uint8_t> original;
    std::vector<std::uint8_t> replacement;
    std::uintptr_t *trampoline{};
    std::uintptr_t previous_trampoline{};
    std::uintptr_t published_trampoline{};
    bool published{};
  };

  std::uint8_t *allocation_{};
  std::vector<InstalledPatch> installed_;

  static auto matches(const void *address,
                      std::span<const std::uint8_t> expected) -> bool {
    if (expected.empty() || expected.size() > 0x1000) {
      return false;
    }
    std::array<std::uint8_t, 0x1000> copy{};
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), address, copy.data(),
                             expected.size(), &copied) != 0 &&
           copied == expected.size() &&
           std::memcmp(copy.data(), expected.data(), expected.size()) == 0;
  }

  static auto writable_slot(const std::uintptr_t *slot) -> bool {
    MEMORY_BASIC_INFORMATION info{};
    const auto address = reinterpret_cast<std::uintptr_t>(slot);
    if (address == 0 || address % alignof(std::uintptr_t) != 0 ||
        VirtualQuery(slot, &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ==
            0) {
      return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    return address >= begin && address - begin <= info.RegionSize &&
           sizeof(*slot) <= info.RegionSize - (address - begin);
  }

  static auto write(std::uint8_t *site, std::span<const std::uint8_t> bytes)
      -> bool {
    MEMORY_BASIC_INFORMATION info{};
    const auto address = reinterpret_cast<std::uintptr_t>(site);
    if (bytes.empty() || VirtualQuery(site, &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
      return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    if (address < begin || address - begin > info.RegionSize ||
        bytes.size() > info.RegionSize - (address - begin)) {
      return false;
    }
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
  CityPatchSet() = default;
  ~CityPatchSet() { restore(); }
  CityPatchSet(const CityPatchSet &) = delete;
  auto operator=(const CityPatchSet &) -> CityPatchSet & = delete;

  auto installed() const noexcept -> bool { return !installed_.empty(); }

  auto restore() -> bool {
    bool success = true;
    for (auto it = installed_.rbegin(); it != installed_.rend(); ++it) {
      if (matches(it->site, it->original)) {
        continue;
      }
      if (!matches(it->site, it->replacement) ||
          !write(it->site, it->original)) {
        success = false;
      }
    }
    if (!success) {
      return false;
    }
    // Do not clear any slot until all sites are restored and all slots remain
    // accessible and owned by this installation (or already restored).
    for (const auto &patch : installed_) {
      if (!patch.published) {
        continue;
      }
      std::uintptr_t value{};
      SIZE_T copied{};
      if (!writable_slot(patch.trampoline) ||
          !ReadProcessMemory(GetCurrentProcess(), patch.trampoline, &value,
                             sizeof(value), &copied) ||
          copied != sizeof(value) ||
          (value != patch.published_trampoline &&
           value != patch.previous_trampoline)) {
        return false;
      }
    }
    for (auto &patch : installed_) {
      if (patch.published) {
        *patch.trampoline = patch.previous_trampoline;
        patch.published = false;
      }
    }
    installed_.clear();
    if (allocation_ != nullptr) {
      if (!VirtualFree(allocation_, 0, MEM_RELEASE)) {
        return false;
      }
      allocation_ = nullptr;
    }
    return true;
  }

  auto install(std::uint8_t *image, std::size_t image_size,
               std::span<const CityCodePatch> patches,
               void (*prepare_callbacks)() noexcept = nullptr) -> bool {
    const auto image_address = reinterpret_cast<std::uintptr_t>(image);
    if (image == nullptr || image_size == 0 || patches.empty() ||
        allocation_ != nullptr || !installed_.empty() ||
        image_size >
            std::numeric_limits<std::uintptr_t>::max() - image_address) {
      return false;
    }
    std::size_t required{};
    std::vector<InstalledPatch> prepared;
    prepared.reserve(patches.size());
    for (std::size_t index{}; index < patches.size(); ++index) {
      const auto &patch = patches[index];
      if (patch.expected.size() < 5 || patch.expected.size() > 0x1000 ||
          patch.stub.empty() || patch.stub.size() > 0x1000 ||
          patch.rva >= image_size ||
          patch.expected.size() > image_size - patch.rva ||
          (patch.call && patch.expected[0] != 0xe8) ||
          (patch.trampoline != nullptr &&
           (patch.trampoline_offset >= patch.stub.size() ||
            !writable_slot(patch.trampoline))) ||
          !matches(image + patch.rva, patch.expected)) {
        return false;
      }
      for (std::size_t prior{}; prior < index; ++prior) {
        const auto &other = patches[prior];
        if ((patch.rva < other.rva + other.expected.size() &&
             other.rva < patch.rva + patch.expected.size()) ||
            (patch.trampoline != nullptr &&
             patch.trampoline == other.trampoline)) {
          return false;
        }
      }
      const auto capacity = (patch.stub.size() + 15) & ~std::size_t{15};
      if (capacity > 0x1000 - required) {
        return false;
      }
      InstalledPatch record{
          image + patch.rva, patch.expected,
          std::vector<std::uint8_t>(patch.expected.size(), 0x90),
          patch.trampoline};
      if (patch.trampoline != nullptr) {
        SIZE_T copied{};
        if (!ReadProcessMemory(GetCurrentProcess(), patch.trampoline,
                               &record.previous_trampoline,
                               sizeof(record.previous_trampoline), &copied) ||
            copied != sizeof(record.previous_trampoline)) {
          return false;
        }
      }
      prepared.push_back(std::move(record));
      required += capacity;
    }

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto granularity =
        static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    if (granularity == 0 || (granularity & (granularity - 1)) != 0) {
      return false;
    }
    const auto origin =
        (image_address + patches.front().rva) & ~(granularity - 1);
    const auto minimum =
        reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const auto maximum =
        reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
    for (std::uintptr_t distance{};
         distance < 0x70000000 && allocation_ == nullptr;
         distance += granularity) {
      for (const bool above : {true, false}) {
        if ((above &&
             distance > std::numeric_limits<std::uintptr_t>::max() - origin) ||
            (!above && distance > origin)) {
          continue;
        }
        const auto address = above ? origin + distance : origin - distance;
        if (address < minimum || address > maximum ||
            maximum - address < 0x1000) {
          continue;
        }
        bool reachable = true;
        std::size_t offset{};
        for (const auto &patch : patches) {
          if (!city_relative_displacement(image_address + patch.rva + 5,
                                          address + offset)
                   .has_value()) {
            reachable = false;
            break;
          }
          offset += (patch.stub.size() + 15) & ~std::size_t{15};
        }
        if (!reachable) {
          continue;
        }
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
    std::size_t offset{};
    for (std::size_t index{}; index < patches.size(); ++index) {
      const auto &patch = patches[index];
      auto &record = prepared[index];
      const auto relative = city_relative_displacement(
          reinterpret_cast<std::uintptr_t>(record.site) + 5,
          reinterpret_cast<std::uintptr_t>(allocation_) + offset);
      if (!relative.has_value()) {
        restore();
        return false;
      }
      std::memcpy(allocation_ + offset, patch.stub.data(), patch.stub.size());
      record.replacement[0] = patch.call ? 0xe8 : 0xe9;
      std::memcpy(record.replacement.data() + 1, &*relative, sizeof(*relative));
      if (patch.trampoline != nullptr) {
        record.published_trampoline =
            reinterpret_cast<std::uintptr_t>(allocation_) + offset +
            patch.trampoline_offset;
      }
      offset += (patch.stub.size() + 15) & ~std::size_t{15};
    }
    DWORD previous{};
    if (!VirtualProtect(allocation_, 0x1000, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), allocation_, required)) {
      restore();
      return false;
    }
    installed_ = std::move(prepared);
    for (auto &record : installed_) {
      if (record.trampoline != nullptr) {
        *record.trampoline = record.published_trampoline;
        record.published = true;
      }
    }
    if (prepare_callbacks != nullptr) {
      prepare_callbacks();
    }
    for (const auto &record : installed_) {
      if (!matches(record.site, record.original) ||
          !write(record.site, record.replacement)) {
        restore();
        return false;
      }
    }
    return true;
  }
};
