#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>

#include "PlacementHooks.hpp"

static auto placement_read(std::uintptr_t address, void *output,
                           std::size_t size) noexcept -> bool {
  SIZE_T copied{};
  return address != 0 && output != nullptr && size != 0 &&
         size <= std::numeric_limits<std::uintptr_t>::max() - address &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), output,
                           size, &copied) != 0 &&
         copied == size;
}

auto placement_fingerprints() -> std::span<const PlacementFingerprint> {
  static const auto fingerprints = [] {
    // These include the owner-register definitions and native continuations.
    // CityEditor's assembly/extension entry detours are outside these ranges.
    constexpr std::array<std::pair<std::uintptr_t, std::string_view>, 29> raw{{
        {0x4c99947, "488bd0488bcee8dec80000488d8e08030000e872c4bcff"},
        {0x4d0b3ac, "488bf1e8fce4ffff488b86e00200004885c0"},
        {0x4d0ca5b, "440f288424a00200000f28bc24b00200000f28b424c0020000"},
        {0x4cb5712,
         "488bf24889bc24c82500004c89a424c02500004c89ac24b82500004c8be94c89"},
        {0x4cbc9b7, "488bcee8f11a090080be28050000004c8bac24b8250000740f488bce"},
        {0x4d2cdac, "895c2458488bf14489458048894d2085d2"},
        {0x4d2eb2c,
         "e8bf37b5ff4c8bbc24400300004c8bb424480300004c8bac2450030000"},
        {0x4d4e4b0, "488bc455564157488d68884881ec60010000440f29a048ffffff"},
        {0x37a9c60,
         "48895c2420555741564881ec00010000488b0589159a054833c448898424f000"},
        {0x4d098c2, "488bd9"},
        {0x4d09e0b, "80bba103000000"},
        {0x4e1c5b0, "4889742418574883ec200fb6f2488bf9"},
        {0x4cf9825, "e886630000"},
        {0x4d0460f, "e8dc340100"},
        {0x4d05b1f, "e8cc1f0100"},
        {0x4d087b0, "48895c2410488974241848897c242055"},
        {0x4cffbb0, "40555641554157488dac2408dfffffb8f8210000"},
        {0x4d16110, "4c8bdc5557498dab08ffffff4881ece8010000"},
        {0x4d17af0, "4055535657415441564157488d6c24e1"},
        {0x4df7390, "488bc4574881ec8000000048896818"},
        {0x4b1ef90, "488954241048894c240841554883ec50"},
        {0x4dba960, "48895c2408574883ec20488b99e0010000"},
        {0x4dade90, "48895c240848896c2410488974241857"},
        {0x4c11a20, "488b81a80200004885c0742c8b4008c1e81ef6d0a8017420"},
        // This accepted-candidate call is separate from ManorEditor's
        // point-containment hook at 0x4b35394. The lower-priority Manor
        // fallback and component-ray selection remain untouched.
        {0x4b353bf, "488bd6488bcbe8a69b0300"},
        {0x4b6ef70, "48895c2408574883ec20488b8110070000488bda488bf9"},
        {0x4c3e610, "488b81680100004c8bc1448b8958040000"},
        {0x4d40700, "40534883ec20488bd98b89b8030000e8ac2df8ff"},
        {0x4b4d1f0, "4883ec680f297424504c8bca0f297c2440b856555555"},
    }};
    std::array<PlacementFingerprint, raw.size()> result;
    for (std::size_t i{}; i < raw.size(); ++i) {
      result[i].rva = raw[i].first;
      for (std::size_t j{}; j < raw[i].second.size(); j += 2) {
        const auto digit = [](char c) {
          return static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
        };
        result[i].bytes.push_back(static_cast<std::uint8_t>(
            digit(raw[i].second[j]) * 16 + digit(raw[i].second[j + 1])));
      }
    }
    return result;
  }();
  return fingerprints;
}

auto placement_validate_image(const std::uint8_t *image) -> bool {
  const auto base = reinterpret_cast<std::uintptr_t>(image);
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  if (!placement_read(base, &dos, sizeof(dos)) ||
      dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 0x1000 ||
      !placement_read(base + dos.e_lfanew, &nt, sizeof(nt)) ||
      nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.FileHeader.TimeDateStamp != 0x6836c1ba ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.OptionalHeader.SizeOfImage != 0x09dfd000 ||
      nt.OptionalHeader.CheckSum != 0x09a2ba15)
    return false;
  // Audited Steam 0.8.117 SHA256:
  // 560451df38cd91134026c25b69539449c570f3163e33bc8c45b4dc20b6952cee.
  for (const auto &fingerprint : placement_fingerprints()) {
    std::array<std::uint8_t, 32> bytes{};
    if (fingerprint.bytes.size() > bytes.size() ||
        fingerprint.rva + fingerprint.bytes.size() >
            nt.OptionalHeader.SizeOfImage ||
        !placement_read(base + fingerprint.rva, bytes.data(),
                        fingerprint.bytes.size()) ||
        std::memcmp(bytes.data(), fingerprint.bytes.data(),
                    fingerprint.bytes.size()) != 0)
      return false;
  }
  return true;
}

static auto append_placement_frame(std::vector<std::uint8_t> &code,
                                   bool restore) -> void {
  if (!restore) {
    code.insert(code.end(), {0x9c, 0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57});
    for (unsigned reg = 8; reg < 16; ++reg)
      code.insert(code.end(),
                  {0x41, static_cast<std::uint8_t>(0x50 + reg % 8)});
    code.insert(code.end(), {0x48, 0x81, 0xec, 0x30, 0x01, 0, 0});
  }
  for (unsigned reg{}; reg < 16; ++reg) {
    code.push_back(0xf3);
    if (reg >= 8)
      code.push_back(0x44);
    code.insert(code.end(),
                {0x0f, static_cast<std::uint8_t>(restore ? 0x6f : 0x7f),
                 static_cast<std::uint8_t>(0x84 + (reg % 8) * 8), 0x24});
    const auto offset = static_cast<std::int32_t>(0x20 + reg * 16);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&offset);
    code.insert(code.end(), bytes, bytes + sizeof(offset));
  }
  code.insert(code.end(),
              {0x0f, 0xae, static_cast<std::uint8_t>(restore ? 0x94 : 0x9c),
               0x24, 0x20, 0x01, 0, 0, 0xd9,
               static_cast<std::uint8_t>(restore ? 0xac : 0xbc), 0x24, 0x24,
               0x01, 0, 0});
  if (restore) {
    code.insert(code.end(), {0x48, 0x8d, 0xa4, 0x24, 0x30, 0x01, 0, 0});
    for (unsigned reg = 16; reg-- > 8;)
      code.insert(code.end(),
                  {0x41, static_cast<std::uint8_t>(0x58 + reg % 8)});
    code.insert(code.end(), {0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58, 0x9d});
  }
}

auto placement_make_bridge(const void *callback, const void *continuation,
                           PlacementBridgeKind kind)
    -> std::vector<std::uint8_t> {
  if (callback == nullptr || continuation == nullptr)
    return {};
  if (kind != PlacementBridgeKind::Load &&
      kind != PlacementBridgeKind::Finish &&
      kind != PlacementBridgeKind::Extension &&
      kind != PlacementBridgeKind::Visual)
    return {};
  // Every midpoint has aligned RSP. Flags and fifteen GPRs consume 0x80;
  // 0x130 reserves shadow space, all sixteen XMMs and floating-point controls.
  std::vector<std::uint8_t> code;
  append_placement_frame(code, false);
  if (kind == PlacementBridgeKind::Visual)
    code.insert(code.end(), {0x48, 0x89, 0xd9}); // RCX = building RBX.
  else
    code.insert(
        code.end(),
        {0x48, 0x89, 0xf1}); // RCX = building or loaded-world master RSI.
  code.insert(code.end(), {0x48, 0xb8});
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
  code.insert(code.end(), {0xff, 0xd0});
  append_placement_frame(code, true);
  // Replay with the original RSP, after all callback state has been restored.
  switch (kind) {
  case PlacementBridgeKind::Load:
    code.insert(code.end(), {0x48, 0x8d, 0x8e, 0x08, 0x03, 0, 0});
    break;
  case PlacementBridgeKind::Finish:
    code.insert(code.end(), {0x44, 0x0f, 0x28, 0x84, 0x24, 0xa0, 0x02, 0, 0});
    break;
  case PlacementBridgeKind::Extension:
    code.insert(code.end(), {0x4c, 0x8b, 0xbc, 0x24, 0x40, 0x03, 0, 0});
    break;
  case PlacementBridgeKind::Visual:
    code.insert(code.end(), {0x80, 0xbb, 0xa1, 0x03, 0, 0, 0});
    break;
  }
  append_absolute_jump(code, continuation);
  return code;
}

PlacementHooks::~PlacementHooks() {
  if (retained_)
    patches_.retain_until_exit();
  else
    patches_.restore();
}

auto PlacementHooks::install(std::uint8_t *image, const void *world_callback,
                             const void *owner_callback,
                             const void *assembly_callback,
                             const void *artisan_callback,
                             const void *primary_callback,
                             const void *secondary_callback,
                             const void *hover_callback) -> bool {
  if (installed() || retained_ || world_callback == nullptr ||
      owner_callback == nullptr || assembly_callback == nullptr ||
      artisan_callback == nullptr || primary_callback == nullptr ||
      secondary_callback == nullptr || hover_callback == nullptr ||
      !placement_validate_image(image))
    return false;
  std::vector<CodePatch> patches;
  patches.push_back({0x4c99952,
                     {0x48, 0x8d, 0x8e, 0x08, 0x03, 0, 0},
                     placement_make_bridge(world_callback, image + 0x4c99959,
                                           PlacementBridgeKind::Load),
                     false});
  patches.push_back({0x4d0ca5b,
                     {0x44, 0x0f, 0x28, 0x84, 0x24, 0xa0, 0x02, 0, 0},
                     placement_make_bridge(owner_callback, image + 0x4d0ca64,
                                           PlacementBridgeKind::Finish),
                     false});
  std::vector<std::uint8_t> assembly;
  // The original is void(owner). The C++ callback calls 0x4d4e4b0 exactly once
  // before reconciling, including when the mod has been disabled.
  append_absolute_jump(assembly, assembly_callback);
  patches.push_back(
      {0x4cbc9ba, {0xe8, 0xf1, 0x1a, 0x09, 0}, std::move(assembly), true});
  patches.push_back({0x4d2eb31,
                     {0x4c, 0x8b, 0xbc, 0x24, 0x40, 0x03, 0, 0},
                     placement_make_bridge(owner_callback, image + 0x4d2eb39,
                                           PlacementBridgeKind::Extension),
                     false});
  patches.push_back({0x4d09e0b,
                     {0x80, 0xbb, 0xa1, 0x03, 0, 0, 0},
                     placement_make_bridge(owner_callback, image + 0x4d09e12,
                                           PlacementBridgeKind::Visual),
                     false});
  // Tail jumps retain the original CALL's return address, stack arguments,
  // register arguments, and EAX result. All three callbacks use the native ABI.
  const std::array<std::pair<std::uintptr_t, const void *>, 3> house_calls{{
      {0x4cf9825, artisan_callback},
      {0x4d0460f, primary_callback},
      {0x4d05b1f, secondary_callback},
  }};
  const std::array<std::array<std::uint8_t, 5>, 3> house_originals{{
      {0xe8, 0x86, 0x63, 0, 0},
      {0xe8, 0xdc, 0x34, 0x01, 0},
      {0xe8, 0xcc, 0x1f, 0x01, 0},
  }};
  for (std::size_t i{}; i < house_calls.size(); ++i) {
    std::vector<std::uint8_t> bridge;
    append_absolute_jump(bridge, house_calls[i].second);
    patches.push_back({house_calls[i].first,
                       {house_originals[i].begin(), house_originals[i].end()},
                       std::move(bridge),
                       true});
  }
  // Native ABI is void(pawn, candidate), already in RCX/RDX. The callback
  // forwards to the native hover setter exactly once, including when disabled.
  std::vector<std::uint8_t> hover;
  append_absolute_jump(hover, hover_callback);
  patches.push_back(
      {0x4b353c5, {0xe8, 0xa6, 0x9b, 0x03, 0}, std::move(hover), true});
  if (!patches_.install(image, patches))
    return false;
  retained_ = true;
  return true;
}
