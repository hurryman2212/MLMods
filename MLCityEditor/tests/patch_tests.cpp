#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "CityPatches.hpp"

namespace {
std::uintptr_t patch_original{};
std::uint8_t *patch_image{};
bool patch_prepared{};
bool patch_prepared_before_write{};
bool patch_conflict_ready{};
constexpr std::array<std::uint8_t, 5> PATCH_TEST_PREFIX{0x8b, 0xc1, 0x83, 0xc0,
                                                        0x07};

auto patch_entry_callback(int value) -> int {
  return reinterpret_cast<int (*)(int)>(patch_original)(value) + 100;
}

auto patch_call_callback(int value) -> int { return value * 3; }

auto patch_prepare() noexcept -> void {
  patch_prepared = patch_original != 0;
  patch_prepared_before_write =
      std::memcmp(patch_image + 0x100, PATCH_TEST_PREFIX.data(),
                  PATCH_TEST_PREFIX.size()) == 0;
}

auto patch_test_write(void *destination, std::span<const std::uint8_t> bytes)
    -> bool {
  DWORD old{};
  if (!VirtualProtect(destination, bytes.size(), PAGE_READWRITE, &old)) {
    return false;
  }
  std::memcpy(destination, bytes.data(), bytes.size());
  const bool flushed = FlushInstructionCache(GetCurrentProcess(), destination,
                                             bytes.size()) != 0;
  DWORD ignored{};
  return VirtualProtect(destination, bytes.size(), old, &ignored) != 0 &&
         flushed;
}

auto patch_prepare_with_conflict() noexcept -> void {
  patch_prepare();
  const std::array<std::uint8_t, 5> foreign{0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
  patch_conflict_ready = patch_test_write(patch_image + 0x204, foreign);
}
} // namespace

auto run_patch_tests() -> int {
  int failed{};
  int checks{};
  const auto check = [&](bool condition, const char *description) {
    ++checks;
    if (!condition) {
      ++failed;
      std::fprintf(stderr, "patch: %s\n", description);
    }
  };
  auto *image = static_cast<std::uint8_t *>(
      VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (image == nullptr) {
    return 1;
  }
  const auto previous_image = city_image;
  city_image = reinterpret_cast<std::uintptr_t>(image);
  patch_image = image;
  patch_original = 0;
  std::memset(image, 0xcc, 0x1000);
  for (const auto offset : {0x100, 0x180}) {
    std::memcpy(image + offset, PATCH_TEST_PREFIX.data(),
                PATCH_TEST_PREFIX.size());
    image[offset + PATCH_TEST_PREFIX.size()] = 0xc3;
  }
  auto call_patch = make_city_call_patch(
      0x204, 0x100, reinterpret_cast<const void *>(&patch_call_callback));
  const std::array<std::uint8_t, 4> caller_prefix{0x48, 0x83, 0xec, 0x28};
  const std::array<std::uint8_t, 5> caller_suffix{0x48, 0x83, 0xc4, 0x28, 0xc3};
  std::memcpy(image + 0x200, caller_prefix.data(), caller_prefix.size());
  std::memcpy(image + 0x204, call_patch.expected.data(),
              call_patch.expected.size());
  std::memcpy(image + 0x209, caller_suffix.data(), caller_suffix.size());
  const std::vector<std::uint8_t> original(image, image + 0x1000);
  DWORD old{};
  check(VirtualProtect(image, 0x1000, PAGE_EXECUTE_READ, &old) != 0,
        "test code made executable without persistent write access");
  check(FlushInstructionCache(GetCurrentProcess(), image, 0x1000) != 0,
        "test code instruction cache flushed");
  const auto entry = reinterpret_cast<int (*)(int)>(image + 0x100);
  const auto caller = reinterpret_cast<int (*)(int)>(image + 0x200);
  check(entry(4) == 11 && caller(4) == 11, "synthetic original code executes");

  std::vector<std::uint8_t> jump;
  append_city_jump(jump, reinterpret_cast<const void *>(&patch_call_callback));
  std::uintptr_t jump_destination{};
  std::memcpy(&jump_destination, jump.data() + 6, sizeof(jump_destination));
  check(jump.size() == 14 && jump[0] == 0xff && jump[1] == 0x25 &&
            jump_destination ==
                reinterpret_cast<std::uintptr_t>(&patch_call_callback),
        "absolute jump encoding preserves registers");
  check(city_relative_displacement(0x80000000, 0) ==
            std::numeric_limits<std::int32_t>::min(),
        "negative rel32 boundary accepted without overflow");
  check(!city_relative_displacement(0, 0x80000000).has_value(),
        "positive rel32 overflow rejected");

  auto entry_patch = make_city_entry_patch(
      0x100, PATCH_TEST_PREFIX,
      reinterpret_cast<const void *>(&patch_entry_callback), &patch_original);
  std::array<CityCodePatch, 2> patches{entry_patch, call_patch};
  CityPatchSet set;
  check(set.install(image, 0x1000, patches, patch_prepare),
        "entry and CALL install");
  check(set.installed() && patch_original != 0,
        "installed state and trampoline published");
  check(patch_prepared && patch_prepared_before_write,
        "all trampolines and callback API ready before first site write");
  check(entry(4) == 111, "entry callback invokes original prefix trampoline");
  check(caller(4) == 12,
        "rel32 CALL stub preserves Windows ABI and return address");
  MEMORY_BASIC_INFORMATION stub_info{};
  check(VirtualQuery(reinterpret_cast<void *>(patch_original), &stub_info,
                     sizeof(stub_info)) != 0 &&
            stub_info.State == MEM_COMMIT &&
            stub_info.Protect == PAGE_EXECUTE_READ,
        "stub page is RX, not RWX");
  check(!set.install(image, 0x1000, patches), "second installation rejected");
  check(set.restore(), "owned code restored");
  check(!set.installed() && patch_original == 0,
        "successful restore clears trampoline");
  check(entry(4) == 11 && caller(4) == 11 &&
            std::memcmp(image, original.data(), original.size()) == 0,
        "all original executable bytes and behavior restored");

  patches[1].expected[4] ^= 1;
  patch_prepared = false;
  check(!set.install(image, 0x1000, patches, patch_prepare),
        "second-site mismatch rejects whole installation");
  check(!set.installed() && !patch_prepared && patch_original == 0 &&
            std::memcmp(image, original.data(), original.size()) == 0,
        "prevalidation mismatch makes no first-site or callback writes");
  patches[1] = call_patch;
  auto overlapping = entry_patch;
  overlapping.rva = 0x102;
  overlapping.expected.assign(image + 0x102, image + 0x107);
  overlapping.trampoline = nullptr;
  patches[1] = overlapping;
  check(!set.install(image, 0x1000, patches),
        "overlapping patch ranges rejected");
  patches[1] = make_city_entry_patch(
      0x180, PATCH_TEST_PREFIX,
      reinterpret_cast<const void *>(&patch_entry_callback), &patch_original);
  check(!set.install(image, 0x1000, patches),
        "shared trampoline output slot rejected");
  patches[1] = call_patch;
  patches[1].stub.resize(0x1000, 0x90);
  check(!set.install(image, 0x1000, patches),
        "combined stub capacity overflow rejected");
  patches[1] = call_patch;
  patches[1].rva = std::numeric_limits<std::uintptr_t>::max() - 1;
  check(!set.install(image, 0x1000, patches),
        "RVA arithmetic overflow rejected");
  patches[1] = call_patch;
  patches[1].rva = 0xffd;
  check(!set.install(image, 0x1000, patches),
        "patch crossing image end rejected");
  patches[1] = call_patch;
  patches[0].trampoline_offset = patches[0].stub.size();
  check(!set.install(image, 0x1000, patches),
        "trampoline outside stub rejected");
  patches[0] = entry_patch;
  check(!set.install(nullptr, 0x1000, patches) &&
            !set.install(image, 0, patches),
        "invalid image ranges rejected");
  check(std::memcmp(image, original.data(), original.size()) == 0 &&
            patch_original == 0,
        "all rejected installations leave code and trampoline unchanged");

  check(set.install(image, 0x1000, std::span{&entry_patch, 1}),
        "entry install for ownership-conflict test");
  const auto retained_trampoline = patch_original;
  const std::array<std::uint8_t, 5> third_party{0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
  check(patch_test_write(image + 0x100, third_party),
        "simulate third-party replacement");
  check(!set.restore(),
        "third-party replacement makes restoration fail closed");
  check(set.installed() && patch_original == retained_trampoline &&
            std::memcmp(image + 0x100, third_party.data(),
                        third_party.size()) == 0,
        "conflicting code not clobbered and trampoline retained");
  check(VirtualQuery(reinterpret_cast<void *>(retained_trampoline), &stub_info,
                     sizeof(stub_info)) != 0 &&
            stub_info.State == MEM_COMMIT,
        "stub allocation retained when pinned callback may remain reachable");
  check(patch_test_write(image + 0x100, PATCH_TEST_PREFIX),
        "simulate external restoration of original site");
  check(set.restore() && patch_original == 0 && !set.installed(),
        "subsequent clean restore safely clears retained installation");

  check(set.install(image, 0x1000, std::span{&entry_patch, 1}),
        "entry install for trampoline ownership conflict");
  const auto slot_value = patch_original;
  patch_original = 1;
  check(!set.restore() && patch_original == 1,
        "third-party trampoline slot is not overwritten");
  patch_original = slot_value;
  check(set.restore() && patch_original == 0,
        "owned trampoline slot can be restored afterward");
  patches = {entry_patch, call_patch};
  check(!set.install(image, 0x1000, patches, patch_prepare_with_conflict) &&
            patch_conflict_ready,
        "late second-site conflict triggers installation rollback");
  check(std::memcmp(image + 0x100, PATCH_TEST_PREFIX.data(),
                    PATCH_TEST_PREFIX.size()) == 0 &&
            std::memcmp(image + 0x204, third_party.data(),
                        third_party.size()) == 0 &&
            patch_original != 0 && set.installed(),
        "rollback restores first owned site but preserves foreign code and "
        "stubs");
  check(patch_test_write(image + 0x204, call_patch.expected) && set.restore() &&
            patch_original == 0,
        "conflict resolution releases partially installed hook safely");
  check(std::memcmp(image, original.data(), original.size()) == 0,
        "final synthetic image byte-identical to original");
  city_image = previous_image;
  patch_image = nullptr;
  check(VirtualFree(image, 0, MEM_RELEASE) != 0, "synthetic image released");
  std::printf("patch: %d checks, %d failures\n", checks, failed);
  return failed;
}
