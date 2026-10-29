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

inline auto make_record_guard(const void *callback, std::uint8_t index_register,
                              std::span<const std::uint8_t> displaced,
                              const void *continuation, const void *skip)
    -> std::vector<std::uint8_t> {
  // Both 0.8.117 query-loop sites have RSP aligned to 16. Save flags, all
  // general registers and all ABI-volatile vector registers. The callback is
  // noexcept and reads only; neither original cache nor query is modified.
  std::vector<std::uint8_t> code{0x9c, 0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,
                                 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
                                 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                                 0x48, 0x81, 0xec, 0x80, 0,    0,    0};
  for (std::uint8_t reg = 0; reg < 6; ++reg) {
    const std::array<std::uint8_t, 6> save{
        0xf3, 0x0f,
        0x7f, static_cast<std::uint8_t>(0x44 + reg * 8),
        0x24, static_cast<std::uint8_t>(0x20 + reg * 16)};
    code.insert(code.end(), save.begin(), save.end());
  }
  const std::array<std::uint8_t, 3> cache_to_rcx{0x4c, 0x89, 0xf9};
  code.insert(code.end(), cache_to_rcx.begin(), cache_to_rcx.end());
  if (index_register == 8 || index_register == 9) {
    const std::array<std::uint8_t, 3> index_to_edx{
        0x44, 0x89,
        static_cast<std::uint8_t>(index_register == 8 ? 0xc2 : 0xca)};
    code.insert(code.end(), index_to_edx.begin(), index_to_edx.end());
  } else {
    code.insert(code.end(), {0x89, 0xf2});
  }
  code.insert(code.end(), {0x48, 0xb8});
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
  code.insert(code.end(), {0xff, 0xd0, 0x84, 0xc0, 0x0f, 0x85, 0, 0, 0, 0});
  const auto branch = code.size() - 4;
  std::vector<std::uint8_t> restore;
  for (std::uint8_t reg = 0; reg < 6; ++reg) {
    const std::array<std::uint8_t, 6> load{
        0xf3, 0x0f,
        0x6f, static_cast<std::uint8_t>(0x44 + reg * 8),
        0x24, static_cast<std::uint8_t>(0x20 + reg * 16)};
    restore.insert(restore.end(), load.begin(), load.end());
  }
  const std::array<std::uint8_t, 31> pops{
      0x48, 0x81, 0xc4, 0x80, 0,    0,    0,    0x41, 0x5f, 0x41, 0x5e,
      0x41, 0x5d, 0x41, 0x5c, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41,
      0x58, 0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58, 0x9d};
  restore.insert(restore.end(), pops.begin(), pops.end());
  code.insert(code.end(), restore.begin(), restore.end());
  // Replay the seven displaced bytes on the original path only.
  code.insert(code.end(), displaced.begin(), displaced.end());
  append_absolute_jump(code, continuation);
  const auto relative = static_cast<std::int32_t>(code.size() - branch - 4);
  std::memcpy(code.data() + branch, &relative, sizeof(relative));
  code.insert(code.end(), restore.begin(), restore.end());
  append_absolute_jump(code, skip);
  return code;
}

inline auto snap_graph_profile_matches(const std::uint8_t *image) -> bool {
  struct Context {
    std::uintptr_t rva;
    std::size_t size;
    std::array<std::uint8_t, 22> bytes;
  };
  // These definitions establish the extra node arguments at all four sites,
  // including RDI's changing role during the second pruning probe.
  static constexpr std::array<Context, 7> contexts{{
      {0x4b8abb7, 22, {0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57,
                       0x48, 0x8d, 0xa8, 0x78, 0xfe, 0xff, 0xff, 0x48,
                       0x81, 0xec, 0x60, 0x02, 0,    0}},
      {0x4b8adb1, 7, {0x4c, 0x6b, 0xe8, 0x78, 0x4c, 0x03, 0x2b}},
      {0x4b8ae30,
       15,
       {0x4c, 0x8b, 0x23, 0xf2, 0x0f, 0x10, 0x26, 0x48, 0x8b, 0x5b, 0x08, 0x48,
        0x89, 0x5d, 0x98}},
      {0x4b8b38a, 7, {0x48, 0x6b, 0xf8, 0x78, 0x49, 0x03, 0x3e}},
      {0x4b8b3c7, 7, {0x4c, 0x6b, 0xf0, 0x78, 0x4d, 0x03, 0x37}},
      {0x4b8b429,
       16,
       {0x48, 0x85, 0xff, 0x4c, 0x8d, 0x7f, 0x32, 0xb9, 0x1a, 0, 0, 0, 0x4c,
        0x0f, 0x44, 0xf9}},
      {0x4b8b5aa,
       16,
       {0x4d, 0x85, 0xf6, 0x49, 0x8d, 0x7e, 0x32, 0xb8, 0x1a, 0, 0, 0, 0x48,
        0x0f, 0x44, 0xf8}},
  }};
  if (!image)
    return false;
  for (const auto &context : contexts)
    if (std::memcmp(image + context.rva, context.bytes.data(), context.size) !=
        0)
      return false;
  return true;
}

inline auto make_snap_graph_bridge(const void *callback, std::uintptr_t site)
    -> std::vector<std::uint8_t> {
  // Keep the native cache/start/end in RCX/RDX/R8. Both extra arguments are
  // FFieldSnapPoint values, not their enclosing position/map records. The side
  // and pruning probes inherit the endpoints of the candidate being checked.
  std::vector<std::uint8_t> code;
  switch (site) {
  case 0x4b8b01f:
    // R13 = first map entry, RBX = second value node.
    code = {0x4d, 0x8d, 0x4d, 0x18, 0x48, 0x8b, 0xc3};
    break;
  case 0x4b8b1b5:
    // R13 = first map entry, [RBP-0x68] = second value node.
    code = {0x4d, 0x8d, 0x4d, 0x18, 0x48, 0x8b, 0x45, 0x98};
    break;
  case 0x4b8b598:
    // RDI/R14 = the two map entries being pruned.
    code = {0x4c, 0x8d, 0x4f, 0x18, 0x49, 0x8d, 0x46, 0x18};
    break;
  case 0x4b8b714:
    // R15 = first entry's suppression flag (+0x32), R14 = second entry.
    code = {0x4d, 0x8d, 0x4f, 0xe6, 0x49, 0x8d, 0x46, 0x18};
    break;
  default:
    return {};
  }
  // A CALL entered with RSP mod 16 == 8. Allocate aligned shadow space plus
  // the fifth argument, without modifying the native caller's stack frame.
  code.insert(code.end(), {0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0x44, 0x24, 0x20,
                           0x48, 0xb8});
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
  code.insert(code.end(), {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x28, 0xc3});
  return code;
}

inline auto make_castle_road_bridge(const void *callback)
    -> std::vector<std::uint8_t> {
  // RoadTick's sibling rebuild at 0x4b69eb9 passes road in RCX and true in DL.
  // Add the pawn (RDI), current sibling iterator (RSI), and end (R14).
  // These identify a complete pass so slow multi-road batches can reuse it.
  std::vector<std::uint8_t> code{0x48, 0x83, 0xec, 0x28, 0x49, 0x89,
                                 0xf8, 0x49, 0x89, 0xf1, 0x4c, 0x89,
                                 0x74, 0x24, 0x20, 0x48, 0xb8};
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
  code.insert(code.end(), {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x28, 0xc3});
  return code;
}

inline auto make_collision_bridge(const void *callback)
    -> std::vector<std::uint8_t> {
  // Original five-argument call: copy its fifth argument from the caller's
  // stack, then append RDI (the acting pawn) as argument six.
  std::vector<std::uint8_t> code{0x48, 0x83, 0xec, 0x38, 0x0f, 0xb6, 0x44,
                                 0x24, 0x60, 0x48, 0x89, 0x44, 0x24, 0x20,
                                 0x48, 0x89, 0x7c, 0x24, 0x28, 0x48, 0xb8};
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
  code.insert(code.end(), {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x38, 0xc3});
  return code;
}

inline auto make_road_bridge(const void *callback, bool segment)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> code;
  if (segment) {
    // Six native arguments become seven. Copy the original pointer and byte
    // from the caller frame; XMM3 (the road width) is left untouched.
    code = {0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x44, 0x24, 0x70, 0x48,
            0x89, 0x44, 0x24, 0x20, 0x0f, 0xb6, 0x44, 0x24, 0x78, 0x48,
            0x89, 0x44, 0x24, 0x28, 0x48, 0x89, 0x7c, 0x24, 0x30};
  } else {
    // Four native arguments become five; preserves XMM2 for endpoint radius.
    code = {0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0x7c, 0x24, 0x20};
  }
  code.insert(code.end(), {0x48, 0xb8});
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
  code.insert(code.end(),
              {0xff, 0xd0, 0x48, 0x83, 0xc4,
               static_cast<std::uint8_t>(segment ? 0x48 : 0x28), 0xc3});
  return code;
}
