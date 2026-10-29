#include <array>
#include <cstring>
#include <string_view>
#include <utility>

#include "FenceLifecycleHooks.hpp"

auto fence_lifecycle_fingerprints()
    -> std::span<const FenceLifecycleFingerprint> {
  static const auto fingerprints = [] {
    constexpr std::array<std::pair<std::uintptr_t, std::string_view>, 8> raw{{
        {0x4cd12e0,
         "48897c24184c896424205541564157488d6c24c94881ece00000008b4208"},
        {0x4d038a7,
         "488d9660060000c6442428014c8d8d90110000c6442420010f1185901100004c8d85c"
         "8070000f20f104810488d8d40030000f20f118da0110000e8fad9fcff"},
        {0x4cddec0, "40555356574154415541564157488dac24b8feffff4881ec48020000"},
        {0x4d1eca2, "498d96a80600004c8bc3f20f104810498d8e60060000488d8580000000"
                    "f20f118d9000000048894424384c8d8d10010000488d85a00000004889"
                    "442430488d85000100004889442428488d85c000000048894424200f11"
                    "8580000000e8bdf1fbff83bda8000000047d56"},
        {0x4d073ec, "8b4de84438742468750583f9037e38443875a87432440fb64c2460488d"
                    "95f0030000440fb64424664180f1014889542428488d95f80000004889"
                    "5424208bd1488bcee8cd740100488b8d800100004885c97405"},
        {0x4d51cfe, "488b11ff92a80b0000498b8fe8020000498bd7488b89c0020000e8a3a8"
                    "efff498d8f400f00008b510c4489610885d2790733d2e8fab4b2ff498d"
                    "8f500f00008b410c4489610885c0790733d2e8d1c638fc498d8f600f00"
                    "008b410c4489610885c0790733d2e8b8c638fc"},
        {0x4c4c5c0, "488bc4535741554881ec900000004c8be933ff488d480848897808"},
        {0x4cffbb0, "40555641554157488dac2408dfffffb8f8210000"},
    }};
    std::array<FenceLifecycleFingerprint, raw.size()> result;
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

auto fence_lifecycle_validate_image(const std::uint8_t *image) -> bool {
  const auto read = [](const void *source, void *target, std::size_t size) {
    SIZE_T copied{};
    return source != nullptr &&
           ReadProcessMemory(GetCurrentProcess(), source, target, size,
                             &copied) != 0 &&
           copied == size;
  };
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  if (!read(image, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 0x1000 ||
      !read(image + dos.e_lfanew, &nt, sizeof(nt)) ||
      nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.FileHeader.TimeDateStamp != 0x6836c1ba ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.OptionalHeader.SizeOfImage != 0x09dfd000 ||
      nt.OptionalHeader.CheckSum != 0x09a2ba15)
    return false;
  for (const auto &fingerprint : fence_lifecycle_fingerprints()) {
    std::array<std::uint8_t, 128> observed{};
    if (fingerprint.bytes.size() > observed.size() ||
        fingerprint.rva > nt.OptionalHeader.SizeOfImage ||
        fingerprint.bytes.size() >
            nt.OptionalHeader.SizeOfImage - fingerprint.rva ||
        !read(image + fingerprint.rva, observed.data(),
              fingerprint.bytes.size()) ||
        std::memcmp(observed.data(), fingerprint.bytes.data(),
                    fingerprint.bytes.size()) != 0)
      return false;
  }
  return true;
}

auto fence_lifecycle_make_finish_bridge(const void *callback,
                                        const void *continuation)
    -> std::vector<std::uint8_t> {
  if (callback == nullptr || continuation == nullptr)
    return {};
  // Midpoint RSP is 16-byte aligned. Eight pushes save flags plus all seven
  // Windows-x64 volatile GPRs; 0x90 keeps alignment and holds shadow space,
  // XMM0..5, MXCSR and the x87 control word. The ABI preserves the other regs.
  std::vector<std::uint8_t> code{0x9c, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41,
                                 0x51, 0x41, 0x52, 0x41, 0x53, 0x48, 0x81,
                                 0xec, 0x90, 0,    0,    0};
  for (unsigned reg{}; reg < 6; ++reg) {
    code.insert(code.end(), {0xf3, 0x0f, 0x7f,
                             static_cast<std::uint8_t>(0x84 + reg * 8), 0x24});
    const auto offset = static_cast<std::uint32_t>(0x20 + reg * 16);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&offset);
    code.insert(code.end(), bytes, bytes + sizeof(offset));
  }
  code
      .insert(
          code.end(), {0x0f, 0xae, 0x9c, 0x24, 0x80, 0, 0,
                       0,    0xd9, 0xbc, 0x24, 0x84, 0, 0,
                       0,    0x48, 0x89, 0xf1, // RCX = native house owner RSI.
                       0x48, 0x8d, 0x95, 0x40, 0x03, 0, 0, // RDX = RBP+340.
                       0x48, 0xb8});
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
  code.insert(code.end(), {0xff, 0xd0});
  code.insert(code.end(), {0x0f, 0xae, 0x94, 0x24, 0x80, 0, 0, 0, 0xd9, 0xac,
                           0x24, 0x84, 0, 0, 0});
  for (unsigned reg{}; reg < 6; ++reg) {
    code.insert(code.end(), {0xf3, 0x0f, 0x6f,
                             static_cast<std::uint8_t>(0x84 + reg * 8), 0x24});
    const auto offset = static_cast<std::uint32_t>(0x20 + reg * 16);
    const auto *offset_bytes = reinterpret_cast<const std::uint8_t *>(&offset);
    code.insert(code.end(), offset_bytes, offset_bytes + sizeof(offset));
  }
  code.insert(code.end(),
              {0x48, 0x8d, 0xa4, 0x24, 0x90, 0, 0, 0, 0x41, 0x5b, 0x41, 0x5a,
               0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58, 0x9d,
               // Replay native instruction overwritten at common tail.
               0x48, 0x8b, 0x8d, 0x80, 0x01, 0, 0});
  append_absolute_jump(code, continuation);
  return code;
}

FenceLifecycleHooks::~FenceLifecycleHooks() {
  if (retained_)
    patches_.retain_until_exit();
  else
    patches_.restore();
}

auto FenceLifecycleHooks::restore() -> bool { return patches_.restore(); }

auto FenceLifecycleHooks::installed() const -> bool {
  return patches_.installed();
}

auto FenceLifecycleHooks::retain_until_exit() -> void { retained_ = true; }

auto FenceLifecycleHooks::install(std::uint8_t *image,
                                  const void *intersection_callback,
                                  const void *slice_callback,
                                  const void *finish_callback) -> bool {
  if (intersection_callback == nullptr || slice_callback == nullptr ||
      finish_callback == nullptr || !fence_lifecycle_validate_image(image))
    return false;
  std::vector<std::uint8_t> intersection_stub, slice_stub;
  // Tail jumps preserve both original call frames and all stack arguments.
  append_absolute_jump(intersection_stub, intersection_callback);
  append_absolute_jump(slice_stub, slice_callback);
  const std::array<CodePatch, 3> patches{{
      {0x4d038e1, {0xe8, 0xfa, 0xd9, 0xfc, 0xff}, intersection_stub, true},
      {0x4d1ecfe, {0xe8, 0xbd, 0xf1, 0xfb, 0xff}, slice_stub, true},
      {0x4d07433,
       {0x48, 0x8b, 0x8d, 0x80, 0x01, 0, 0},
       fence_lifecycle_make_finish_bridge(finish_callback, image + 0x4d0743a),
       false},
  }};
  return patches_.install(image, patches);
}
