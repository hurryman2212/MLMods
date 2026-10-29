#pragma once

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

#include "Patches.hpp"

inline auto make_connection_nav_bridge(const void *callback)
    -> std::vector<std::uint8_t> {
  if (callback == nullptr)
    return {};
  // Native (nav, point, float radius in XMM2); fourth argument is owner RBP.
  std::vector<std::uint8_t> code{0x49, 0x89, 0xe9};
  append_absolute_jump(code, callback);
  return code;
}

inline auto make_connection_mask_bridge(const void *callback)
    -> std::vector<std::uint8_t> {
  if (callback == nullptr)
    return {};
  // Native (point, float radius in XMM1, triangles). R8 points to owner+688.
  // RSI was already reassigned to that array header at 0x144d1ca19.
  std::vector<std::uint8_t> code{0x4d, 0x8d, 0x88, 0x78, 0xf9, 0xff, 0xff};
  append_absolute_jump(code, callback);
  return code;
}

inline auto make_connection_restore_bridge(const void *callback)
    -> std::vector<std::uint8_t> {
  if (callback == nullptr)
    return {};
  // Preserve cache/point/radius/weak-ignore in RCX/RDX/R8/R9 and append R15.
  // RSP%16==8 at this CALL stub. 0x28 reserves shadow space plus argument 5.
  std::vector<std::uint8_t> code{0x48, 0x83, 0xec, 0x28, 0x4c, 0x89,
                                 0x7c, 0x24, 0x20, 0x48, 0xb8};
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
  code.insert(code.end(), {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x28, 0xc3});
  return code;
}

inline auto make_connection_postload_bridge(const void *callback,
                                            const void *continuation)
    -> std::vector<std::uint8_t> {
  if (callback == nullptr || continuation == nullptr)
    return {};
  // Outer loader's final ready store, after native obstacle queue draining.
  // Replay the store before recovery; RSI is the master. Preserve RCX, which
  // already points at the following QueryPerformanceCounter output local.
  std::vector<std::uint8_t> code{0xc6, 0x86, 0x18, 0x03, 0,    0,    1,   0x9c,
                                 0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57};
  for (unsigned reg = 8; reg < 16; ++reg)
    code.insert(code.end(), {0x41, static_cast<std::uint8_t>(0x50 + reg % 8)});
  code.insert(code.end(), {0x48, 0x81, 0xec, 0x30, 0x01, 0, 0});
  const auto vectors = [&](bool load) {
    for (unsigned reg{}; reg < 16; ++reg) {
      code.push_back(0xf3);
      if (reg >= 8)
        code.push_back(0x44);
      code.insert(code.end(),
                  {0x0f, static_cast<std::uint8_t>(load ? 0x6f : 0x7f),
                   static_cast<std::uint8_t>(0x84 + (reg % 8) * 8), 0x24});
      const auto offset = static_cast<std::int32_t>(0x20 + reg * 16);
      const auto *bytes = reinterpret_cast<const std::uint8_t *>(&offset);
      code.insert(code.end(), bytes, bytes + sizeof(offset));
    }
  };
  vectors(false);
  code.insert(code.end(),
              {0x0f, 0xae, 0x9c, 0x24, 0x20, 0x01, 0,    0,    0xd9, 0xbc,
               0x24, 0x24, 0x01, 0,    0,    0x48, 0x89, 0xf1, 0x48, 0xb8});
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
  code.insert(code.end(), {0xff, 0xd0});
  vectors(true);
  code.insert(code.end(), {0x0f, 0xae, 0x94, 0x24, 0x20, 0x01, 0, 0,
                           0xd9, 0xac, 0x24, 0x24, 0x01, 0,    0, 0x48,
                           0x8d, 0xa4, 0x24, 0x30, 0x01, 0,    0});
  for (unsigned reg = 16; reg-- > 8;)
    code.insert(code.end(), {0x41, static_cast<std::uint8_t>(0x58 + reg % 8)});
  code.insert(code.end(), {0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58, 0x9d});
  append_absolute_jump(code, continuation);
  return code;
}

struct ConnectionFingerprint {
  std::uintptr_t rva{};
  std::vector<std::uint8_t> bytes;
};

inline auto connection_fingerprints()
    -> std::span<const ConnectionFingerprint> {
  static const auto fingerprints = [] {
    constexpr std::array<std::pair<std::uintptr_t, std::string_view>, 9> raw{{
        {0x4d56600,
         "488b85e8020000488bd7f30f101586767204488b88c0020000e80205efff"},
        {0x4c2e9c0, "4c8b030f28ce4981c088060000488bcee81b83eeff84c0"},
        {0x4d1ca26, "4c8ba3d80c00004c8bc6f30f100d601276044863c7488d0440"},
        {0x4d1ca3f, "498d0cc448894510e8a4a2dfff84c0"},
        {0x4cff82b, "f3440f2c0564e477044c8b4c2478488bd7488bcbe84ccde9ff84c0"},
        // The final outer ready flag, not the earlier child-load delegate.
        {0x4c84b8f, "488d4c2450c6861803000001ff15874f4102"},
        {0x4cff790,
         "4c8bdc49894b0841574883ec60488b81c80200004c8bf94885c00f84f5020000"},
        {0x4ca71a0,
         "40555356574154415541564157488bec4883ec784533e40f29742460488bf1f2"},
        {0x4c821cb, "488bf1"},
    }};
    std::array<ConnectionFingerprint, raw.size()> result;
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

inline auto connection_profile_matches(const std::uint8_t *image) -> bool {
  const auto base = reinterpret_cast<std::uintptr_t>(image);
  if (base == 0 ||
      base > std::numeric_limits<std::uintptr_t>::max() - 0x09dfd000)
    return false;
  for (const auto &fingerprint : connection_fingerprints()) {
    std::array<std::uint8_t, 32> bytes{};
    SIZE_T copied{};
    if (fingerprint.bytes.size() > bytes.size() ||
        ReadProcessMemory(GetCurrentProcess(), image + fingerprint.rva,
                          bytes.data(), fingerprint.bytes.size(),
                          &copied) == 0 ||
        copied != fingerprint.bytes.size() ||
        std::memcmp(bytes.data(), fingerprint.bytes.data(), copied) != 0)
      return false;
  }
  return true;
}
