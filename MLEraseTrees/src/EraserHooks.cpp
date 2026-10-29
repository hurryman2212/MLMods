#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <Windows.h>

#include "EraserHooks.hpp"
#include "EraserRuntime.hpp"

auto eraser_fingerprints() -> std::span<const EraserFingerprint> {
  static const auto fingerprints = [] {
    constexpr std::array<std::pair<std::uintptr_t, std::string_view>, 6> raw{{
        {0x4b16840,
         "4c8bdc554155498dab08ffffff4881ece8010000488b05a51262044833c44889"},
        {0x4b16b63, "4c39310f84f00200004883c108483bca75ee"},
        {0x4b16e46,
         "498b06488d542440498bceff90a00b0000448b44244cf20f107c24684983c708"},
        {0x4b7bd00,
         "48895c24084889742410574883ec20488d99d8020000488bf248637b088d4701"},
        {0x4afe9a1, "80beec120000017508488bcee88e7e0100"},
        {0x392ff30, "488b014533c048ffa0980b0000cccccc"},
    }};
    std::array<EraserFingerprint, raw.size()> result;
    for (std::size_t i = 0; i < raw.size(); ++i) {
      result[i].rva = raw[i].first;
      const auto value = raw[i].second;
      for (std::size_t j = 0; j < value.size(); j += 2) {
        const auto digit = [](char c) {
          return static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
        };
        result[i].bytes.push_back(static_cast<std::uint8_t>(
            digit(value[j]) * 16 + digit(value[j + 1])));
      }
    }
    return result;
  }();
  return fingerprints;
}

auto eraser_validate_image(const std::uint8_t *image) -> bool {
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  SIZE_T copied{};
  if (image == nullptr ||
      !ReadProcessMemory(GetCurrentProcess(), image, &dos, sizeof(dos),
                         &copied) ||
      copied != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 0x1000 ||
      !ReadProcessMemory(GetCurrentProcess(), image + dos.e_lfanew, &nt,
                         sizeof(nt), &copied) ||
      copied != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.FileHeader.TimeDateStamp != 0xb47dcd3f ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.OptionalHeader.SizeOfImage != 0x09deb000 ||
      nt.OptionalHeader.CheckSum != 0x09a1d1f2) {
    return false;
  }
  // Steam 0.8.111: SHA256
  // 35cf516d8c161c8ea64e801e81a90c0e5cb69f41e5581fa50e0dc2f210d33f4b.
  for (const auto &fingerprint : eraser_fingerprints()) {
    std::array<std::uint8_t, 32> bytes{};
    if (fingerprint.bytes.size() > bytes.size() ||
        fingerprint.rva + fingerprint.bytes.size() >
            nt.OptionalHeader.SizeOfImage ||
        !ReadProcessMemory(GetCurrentProcess(), image + fingerprint.rva,
                           bytes.data(), fingerprint.bytes.size(), &copied) ||
        copied != fingerprint.bytes.size() ||
        std::memcmp(bytes.data(), fingerprint.bytes.data(), copied) != 0) {
      return false;
    }
  }
  return true;
}

static auto append_eraser_save(std::vector<std::uint8_t> &code) -> void {
  // Both midpoints have RSP%16==0. Save flags and every volatile GPR/XMM.
  // Native nonvolatile registers remain protected by the Windows x64 ABI.
  code.insert(code.end(), {0x9c, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41,
                           0x52, 0x41, 0x53, 0x48, 0x81, 0xec, 0x80, 0, 0, 0});
  for (std::uint8_t i = 0; i != 6; ++i) {
    code.insert(code.end(),
                {0xf3, 0x0f, 0x7f, static_cast<std::uint8_t>(0x44 + i * 8),
                 0x24, static_cast<std::uint8_t>(0x20 + i * 16)});
  }
}

static auto append_eraser_restore(std::vector<std::uint8_t> &code) -> void {
  for (std::uint8_t i = 0; i != 6; ++i) {
    code.insert(code.end(),
                {0xf3, 0x0f, 0x6f, static_cast<std::uint8_t>(0x44 + i * 8),
                 0x24, static_cast<std::uint8_t>(0x20 + i * 16)});
  }
  code.insert(code.end(),
              {0x48, 0x81, 0xc4, 0x80, 0, 0, 0, 0x41, 0x5b, 0x41, 0x5a, 0x41,
               0x59, 0x41, 0x58, 0x5a, 0x59, 0x58, 0x9d});
}

auto eraser_filter_bridge(const void *callback, const void *allow,
                          const void *skip, const void *native_non_tree)
    -> std::vector<std::uint8_t> {
  if (callback == nullptr || allow == nullptr || skip == nullptr ||
      native_non_tree == nullptr) {
    return {};
  }
  // ZF==0 means the original comparison did not match a tree component.
  // Do not invoke any helper or alter flags on that native branch.
  std::vector<std::uint8_t> code{0x0f, 0x85, 0, 0, 0, 0};
  append_eraser_save(code);
  code.insert(code.end(), {0x4c, 0x89, 0xe9, 0x4c, 0x89, 0xf2, 0x48, 0xb8});
  const auto callback_address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *address_bytes =
      reinterpret_cast<const std::uint8_t *>(&callback_address);
  code.insert(code.end(), address_bytes,
              address_bytes + sizeof(callback_address));
  code.insert(code.end(), {0xff, 0xd0, 0x84, 0xc0, 0x0f, 0x84});
  const auto skip_displacement = code.size();
  code.insert(code.end(), {0, 0, 0, 0});
  append_eraser_restore(code);
  append_eraser_jump(code, allow);
  const auto skip_offset = code.size();
  append_eraser_restore(code);
  append_eraser_jump(code, skip);
  const auto non_tree_offset = code.size();
  append_eraser_jump(code, native_non_tree);
  const auto skip_relative =
      static_cast<std::int32_t>(skip_offset - skip_displacement - 4);
  const auto non_tree_relative = static_cast<std::int32_t>(non_tree_offset - 6);
  std::memcpy(code.data() + skip_displacement, &skip_relative,
              sizeof(skip_relative));
  std::memcpy(code.data() + 2, &non_tree_relative, sizeof(non_tree_relative));
  return code;
}

auto eraser_cleanup_bridge(const void *callback, const void *resume)
    -> std::vector<std::uint8_t> {
  if (callback == nullptr || resume == nullptr) {
    return {};
  }
  std::vector<std::uint8_t> code;
  append_eraser_save(code);
  code.insert(code.end(), {0x4c, 0x89, 0xe9, // RCX=pawn R13.
                           0x4c, 0x89, 0xf2, // RDX=component R14.
                           0x4c, 0x8d, 0x84, 0x24, 0x10,
                           0x01, 0,    0, // R8=original RSP+0x50.
                           0x44, 0x0f, 0xb6, 0x8c, 0x24,
                           0xb0, 0,    0,    0, // R9D=original AL success.
                           0x48, 0xb8});
  const auto callback_address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *address_bytes =
      reinterpret_cast<const std::uint8_t *>(&callback_address);
  code.insert(code.end(), address_bytes,
              address_bytes + sizeof(callback_address));
  code.insert(code.end(), {0xff, 0xd0});
  append_eraser_restore(code);
  code.insert(code.end(),
              {0x44, 0x8b, 0x44, 0x24, 0x4c}); // Original displaced MOV.
  append_eraser_jump(code, resume);
  return code;
}

auto eraser_make_patches() -> std::vector<EraserCodePatch> {
  if (eraser_image == 0) {
    return {};
  }
  return {
      {0x4b16b66,
       {0x0f, 0x84, 0xf0, 0x02, 0, 0},
       eraser_filter_bridge(
           reinterpret_cast<const void *>(&eraser_allow_tree),
           reinterpret_cast<const void *>(eraser_image + 0x4b16b75),
           reinterpret_cast<const void *>(eraser_image + 0x4b16e5c),
           reinterpret_cast<const void *>(eraser_image + 0x4b16b6c)),
       false},
      {0x4b16e57,
       {0x44, 0x8b, 0x44, 0x24, 0x4c},
       eraser_cleanup_bridge(
           reinterpret_cast<const void *>(&eraser_cleanup),
           reinterpret_cast<const void *>(eraser_image + 0x4b16e5c)),
       false},
  };
}
