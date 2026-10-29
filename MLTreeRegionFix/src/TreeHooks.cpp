#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <Windows.h>

#include "TreeHooks.hpp"
#include "TreeRuntime.hpp"

auto tree_fingerprints() -> std::span<const TreeFingerprint> {
  static const auto fingerprints = [] {
    constexpr std::array<std::pair<std::uintptr_t, std::string_view>, 6> raw{{
        {0x4dc8cde,
         "498bbe28030000488b9010080000488b1a488d1522da1c03e8454830fc4533c9"},
        {0x4dc8e4d,
         "498b9e28030000e837bcefff4863b3700f0000448bf88d4e01898b700f00003b"},
        {0x4cc9ab0,
         "48895c240848896c24104889742418574883ec20f20f104208488beaf20f100a"},
        {0x4db8860,
         "48895c2408574883ec20488b8118030000488bfaf30f1005e0653402488bd90f"},
        {0x4c5e4e0,
         "48895c241048896c241848897c24204154415641574883ec50440fb6bc249000"},
        {0x4dc5270,
         "4c8bdc554156498dabf8fdffff4881ecf8020000488b05755f38044833c44889"},
    }};
    std::array<TreeFingerprint, raw.size()> result;
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

auto tree_validate_image(const std::uint8_t *image) -> bool {
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
      nt.FileHeader.TimeDateStamp != 0x6836c1ba ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.OptionalHeader.SizeOfImage != 0x09dfd000 ||
      nt.OptionalHeader.CheckSum != 0x09a2ba15) {
    return false;
  }
  // Exact Steam 0.8.117 build 26185246 executable SHA256:
  // 560451df38cd91134026c25b69539449c570f3163e33bc8c45b4dc20b6952cee.
  for (const auto &fingerprint : tree_fingerprints()) {
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

auto tree_region_bridge(bool component, const void *callback,
                        const void *resume) -> std::vector<std::uint8_t> {
  if (callback == nullptr || resume == nullptr) {
    return {};
  }
  // Both native midpoints have RSP%16==0. Eight pushes preserve flags and
  // volatile GPRs; 128 bytes supply shadow space plus six saved XMM registers.
  std::vector<std::uint8_t> code{0x9c, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41,
                                 0x51, 0x41, 0x52, 0x41, 0x53, 0x48, 0x81,
                                 0xec, 0x80, 0,    0,    0};
  for (std::uint8_t i = 0; i != 6; ++i) {
    code.insert(code.end(),
                {0xf3, 0x0f, 0x7f, static_cast<std::uint8_t>(0x44 + i * 8),
                 0x24, static_cast<std::uint8_t>(0x20 + i * 16)});
  }
  code.insert(code.end(), {0x4c, 0x89, 0xf1}); // RCX=unit R14.
  if (component) {
    code.insert(code.end(), {0x48, 0x89, 0xfa, // RDX=actual component RDI.
                             0x4d, 0x8b, 0x86, 0x28, 0x03, 0, 0});
  } else {
    code.insert(code.end(), {0x49, 0x8b, 0x96, 0x28, 0x03, 0, 0});
  }
  code.insert(code.end(), {0x48, 0xb8});
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
  code.insert(code.end(), {0xff, 0xd0, 0x48, 0x89,
                           static_cast<std::uint8_t>(component ? 0xc3 : 0xc7)});
  for (std::uint8_t i = 0; i != 6; ++i) {
    code.insert(code.end(),
                {0xf3, 0x0f, 0x6f, static_cast<std::uint8_t>(0x44 + i * 8),
                 0x24, static_cast<std::uint8_t>(0x20 + i * 16)});
  }
  code.insert(code.end(),
              {0x48, 0x81, 0xc4, 0x80, 0, 0, 0, 0x41, 0x5b, 0x41, 0x5a, 0x41,
               0x59, 0x41, 0x58, 0x5a, 0x59, 0x58, 0x9d});
  append_tree_jump(code, resume);
  return code;
}

auto tree_make_patches() -> std::vector<TreeCodePatch> {
  if (tree_image == 0) {
    return {};
  }
  return {
      {0x4dc8cde,
       {0x49, 0x8b, 0xbe, 0x28, 0x03, 0, 0},
       tree_region_bridge(
           false, reinterpret_cast<const void *>(&tree_select_region),
           reinterpret_cast<const void *>(tree_image + 0x4dc8ce5)),
       false},
      {0x4dc8e4d,
       {0x49, 0x8b, 0x9e, 0x28, 0x03, 0, 0},
       tree_region_bridge(
           true, reinterpret_cast<const void *>(&tree_component_region),
           reinterpret_cast<const void *>(tree_image + 0x4dc8e54)),
       false},
  };
}
