#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include <Windows.h>

#include "ConnectionBridges.hpp"

static auto connection_nav_arguments(void *nav, void *point, float radius,
                                     void *owner) noexcept -> std::uint8_t {
  return nav == reinterpret_cast<void *>(1) &&
                 point == reinterpret_cast<void *>(2) && radius == 17.25F &&
                 owner == reinterpret_cast<void *>(0x12345678)
             ? 0xa7
             : 0;
}

static auto connection_mask_arguments(void *point, float radius,
                                      void *triangles, void *owner) noexcept
    -> std::uint8_t {
  return point == reinterpret_cast<void *>(1) && radius == 31.5F &&
                 triangles == reinterpret_cast<void *>(0x12345d00) &&
                 owner == reinterpret_cast<void *>(0x12345678)
             ? 0xd9
             : 0;
}

static auto connection_restore_arguments(void *cache, void *point,
                                         std::int32_t radius,
                                         std::uint64_t ignored,
                                         void *owner) noexcept -> std::uint8_t {
  return cache == reinterpret_cast<void *>(1) &&
                 point == reinterpret_cast<void *>(2) && radius == 213 &&
                 ignored == 0xfedcba9876543210ULL &&
                 owner == reinterpret_cast<void *>(0x87654321)
             ? 0xb3
             : 0;
}

struct ConnectionCpuSnapshot {
  std::array<std::uint64_t, 16> registers{};
  std::array<std::array<std::uint8_t, 16>, 16> vectors{};
  std::uint64_t flags{};
  std::uint32_t mxcsr{};
  std::uint16_t x87{};
};

struct ConnectionPostloadState {
  ConnectionCpuSnapshot before{}, after{};
  std::array<std::uint8_t, 0x60> native_frame{};
  std::array<std::uint8_t, 0x320> master{};
  std::array<std::uint8_t, 256> vector_pattern{};
  std::uintptr_t argument{}, callback_rsp{};
  std::uint32_t calls{}, observed_ready{}, clobber_mxcsr{0x5f80};
  std::uint16_t clobber_x87{0x077f};
};

auto run_connection_bridge_tests(std::uint8_t *image) -> int {
  int checks{}, failed{};
  const auto expect = [&](bool ok, const char *message) {
    ++checks;
    if (!ok) {
      ++failed;
      std::fprintf(stderr, "connection bridge: %s\n", message);
    }
  };
  const auto *placeholder = reinterpret_cast<const void *>(1);
  expect(make_connection_nav_bridge(nullptr).empty() &&
             make_connection_mask_bridge(nullptr).empty() &&
             make_connection_restore_bridge(nullptr).empty() &&
             make_connection_postload_bridge(nullptr, placeholder).empty() &&
             make_connection_postload_bridge(placeholder, nullptr).empty(),
         "incomplete bridge callbacks/continuations rejected");
  expect(connection_profile_matches(image),
         "actual executable context matches");
  expect(!connection_profile_matches(nullptr), "null profile rejected");
  if (image != nullptr) {
    for (const auto &fingerprint : connection_fingerprints()) {
      for (std::size_t i{}; i < fingerprint.bytes.size(); ++i) {
        image[fingerprint.rva + i] ^= 1;
        expect(!connection_profile_matches(image),
               "each changed call-context byte fails closed");
        image[fingerprint.rva + i] ^= 1;
      }
    }
  }

  const auto append32 = [](std::vector<std::uint8_t> &code,
                           std::int32_t value) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
    code.insert(code.end(), bytes, bytes + sizeof(value));
  };
  const auto immediate = [](std::vector<std::uint8_t> &code, unsigned reg,
                            std::uintptr_t value) {
    code.push_back(reg >= 8 ? 0x49 : 0x48);
    code.push_back(static_cast<std::uint8_t>(0xb8 + reg % 8));
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
    code.insert(code.end(), bytes, bytes + sizeof(value));
  };

  for (unsigned kind{}; kind < 3; ++kind) {
    auto *memory = static_cast<std::uint8_t *>(VirtualAlloc(
        nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (memory == nullptr)
      return failed + 1;
    // Save context registers and provide an aligned native CALL frame.
    std::vector<std::uint8_t> caller{0x55, 0x41, 0x57, 0x48, 0x83, 0xec, 0x28};
    immediate(caller, 5, 0x12345678);
    immediate(caller, 15, 0x87654321);
    caller.push_back(0xe8);
    append32(caller, static_cast<std::int32_t>(0x100 - caller.size() - 4));
    caller.insert(caller.end(),
                  {0x48, 0x83, 0xc4, 0x28, 0x41, 0x5f, 0x5d, 0xc3});
    const auto bridge =
        kind == 0 ? make_connection_nav_bridge(reinterpret_cast<const void *>(
                        &connection_nav_arguments))
        : kind == 1
            ? make_connection_mask_bridge(
                  reinterpret_cast<const void *>(&connection_mask_arguments))
            : make_connection_restore_bridge(reinterpret_cast<const void *>(
                  &connection_restore_arguments));
    std::memcpy(memory, caller.data(), caller.size());
    std::memcpy(memory + 0x100, bridge.data(), bridge.size());
    DWORD old{};
    const bool executable =
        VirtualProtect(memory, 0x1000, PAGE_EXECUTE_READ, &old) != 0 &&
        FlushInstructionCache(GetCurrentProcess(), memory, 0x1000) != 0;
    expect(executable, "argument harness executable and cache flushed");
    if (executable && kind == 0) {
      const auto call =
          reinterpret_cast<std::uint8_t (*)(void *, void *, float)>(memory);
      expect(call(reinterpret_cast<void *>(1), reinterpret_cast<void *>(2),
                  17.25F) == 0xa7,
             "C78 preserves XMM2 radius and appends original RBP owner");
    } else if (executable && kind == 1) {
      const auto call =
          reinterpret_cast<std::uint8_t (*)(void *, float, void *)>(memory);
      expect(call(reinterpret_cast<void *>(1), 31.5F,
                  reinterpret_cast<void *>(0x12345d00)) == 0xd9,
             "CD8 preserves XMM1 and derives owner from triangles minus 0x688");
    } else if (executable) {
      const auto call = reinterpret_cast<std::uint8_t (*)(
          void *, void *, std::int32_t, std::uint64_t)>(memory);
      expect(call(reinterpret_cast<void *>(1), reinterpret_cast<void *>(2), 213,
                  0xfedcba9876543210ULL) == 0xb3,
             "C98 retains full original weak ignore and appends R15 on stack");
    }
    expect(VirtualFree(memory, 0, MEM_RELEASE) != 0,
           "argument harness released");
  }

  for (const bool zero : {false, true}) {
    auto *memory = static_cast<std::uint8_t *>(VirtualAlloc(
        nullptr, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (memory == nullptr)
      return failed + 1;
    auto *state = new (memory + 0x2000) ConnectionPostloadState{};
    const std::uintptr_t loaded = zero ? 0 : 0x1122334455667788ULL;
    std::memcpy(state->native_frame.data() + 0x50, &loaded, sizeof(loaded));
    state->master[0x318] = zero ? 0 : 1;
    const auto original_frame = state->native_frame;
    for (std::size_t i{}; i < state->vector_pattern.size(); ++i)
      state->vector_pattern[i] = static_cast<std::uint8_t>(i * 7 + 3);
    const auto relative = [&](std::vector<std::uint8_t> &code,
                              std::size_t origin, const void *target) {
      const auto difference = reinterpret_cast<const std::uint8_t *>(target) -
                              (memory + origin + code.size() + 4);
      expect(difference >= INT32_MIN && difference <= INT32_MAX,
             "harness RIP displacement fits");
      append32(code, static_cast<std::int32_t>(difference));
    };
    const auto vectors_stack = [&](std::vector<std::uint8_t> &code, bool load) {
      for (unsigned reg = 6; reg < 16; ++reg) {
        code.push_back(0xf3);
        if (reg >= 8)
          code.push_back(0x44);
        code.insert(code.end(),
                    {0x0f, static_cast<std::uint8_t>(load ? 0x6f : 0x7f),
                     static_cast<std::uint8_t>(0x84 + (reg % 8) * 8), 0x24});
        append32(code, static_cast<std::int32_t>((reg - 6) * 16));
      }
    };
    const auto snapshot = [&](std::vector<std::uint8_t> &code,
                              std::size_t origin, ConnectionCpuSnapshot *out) {
      for (unsigned reg{}; reg < 16; ++reg) {
        code.insert(code.end(),
                    {static_cast<std::uint8_t>(reg >= 8 ? 0x4c : 0x48), 0x89,
                     static_cast<std::uint8_t>(0x05 + (reg % 8) * 8)});
        relative(code, origin, &out->registers[reg]);
      }
      for (unsigned reg{}; reg < 16; ++reg) {
        code.push_back(0xf3);
        if (reg >= 8)
          code.push_back(0x44);
        code.insert(
            code.end(),
            {0x0f, 0x7f, static_cast<std::uint8_t>(0x05 + (reg % 8) * 8)});
        relative(code, origin, &out->vectors[reg]);
      }
      code.insert(code.end(), {0x9c, 0x8f, 0x05});
      relative(code, origin, &out->flags);
      code.insert(code.end(), {0x0f, 0xae, 0x1d});
      relative(code, origin, &out->mxcsr);
      code.insert(code.end(), {0xd9, 0x3d});
      relative(code, origin, &out->x87);
    };
    std::vector<std::uint8_t> entry{0x53, 0x55, 0x56, 0x57, 0x41,
                                    0x54, 0x41, 0x55, 0x41, 0x56,
                                    0x41, 0x57, 0x48, 0x81, 0xec};
    append32(entry, 0x128);
    vectors_stack(entry, false);
    for (unsigned reg{}; reg < 16; ++reg) {
      if (reg == 4)
        continue;
      immediate(
          entry, reg,
          reg == 5
              ? reinterpret_cast<std::uintptr_t>(state->native_frame.data())
          : reg == 6 ? reinterpret_cast<std::uintptr_t>(state->master.data())
                     : 0xabc0000000000000ULL + reg);
    }
    for (unsigned reg{}; reg < 16; ++reg) {
      entry.push_back(0xf3);
      if (reg >= 8)
        entry.push_back(0x44);
      entry.insert(
          entry.end(),
          {0x0f, 0x6f, static_cast<std::uint8_t>(0x05 + (reg % 8) * 8)});
      relative(entry, 0, state->vector_pattern.data() + reg * 16);
    }
    snapshot(entry, 0, &state->before);
    append_absolute_jump(entry, memory + 0x800);

    std::vector<std::uint8_t> callback{0x48, 0x89, 0x0d};
    relative(callback, 0xc00, &state->argument);
    callback.insert(callback.end(),
                    {0x0f, 0xb6, 0x81, 0x18, 0x03, 0, 0, 0x89, 0x05});
    relative(callback, 0xc00, &state->observed_ready);
    callback.insert(callback.end(), {0x48, 0x89, 0x25});
    relative(callback, 0xc00, &state->callback_rsp);
    callback.insert(callback.end(), {0xff, 0x05});
    relative(callback, 0xc00, &state->calls);
    for (std::uint8_t offset = 8; offset <= 36; offset += 4) {
      callback.insert(callback.end(), {0xc7, 0x44, 0x24, offset});
      append32(callback, static_cast<std::int32_t>(0xdeadbeefU));
    }
    callback.insert(callback.end(), {0x0f, 0xae, 0x15});
    relative(callback, 0xc00, &state->clobber_mxcsr);
    callback.insert(callback.end(), {0xd9, 0x2d});
    relative(callback, 0xc00, &state->clobber_x87);
    for (unsigned reg{}; reg < 16; ++reg) {
      callback.push_back(0x66);
      if (reg >= 8)
        callback.push_back(0x45);
      callback.insert(
          callback.end(),
          {0x0f, 0xef, static_cast<std::uint8_t>(0xc0 + (reg % 8) * 9)});
      if (reg != 4)
        immediate(callback, reg, 0xdef0000000000000ULL + reg);
    }
    callback.push_back(0xc3);
    const auto bridge =
        make_connection_postload_bridge(memory + 0xc00, memory + 0x1000);
    std::vector<std::uint8_t> resume;
    snapshot(resume, 0x1000, &state->after);
    vectors_stack(resume, true);
    resume.insert(resume.end(), {0x48, 0x81, 0xc4});
    append32(resume, 0x128);
    resume.insert(resume.end(), {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c,
                                 0x5f, 0x5e, 0x5d, 0x5b, 0x31, 0xc0, 0xc3});
    const bool layout = entry.size() < 0x800 && bridge.size() < 0x400 &&
                        callback.size() < 0x400 && resume.size() < 0x1000;
    expect(layout, "postload code sections do not overlap");
    if (layout) {
      std::memcpy(memory, entry.data(), entry.size());
      std::memcpy(memory + 0x800, bridge.data(), bridge.size());
      std::memcpy(memory + 0xc00, callback.data(), callback.size());
      std::memcpy(memory + 0x1000, resume.data(), resume.size());
      DWORD old{};
      const bool executable =
          VirtualProtect(memory, 0x2000, PAGE_EXECUTE_READ, &old) != 0 &&
          FlushInstructionCache(GetCurrentProcess(), memory, 0x2000) != 0;
      expect(executable,
             "postload code RX, state RW, instruction cache flushed");
      if (executable) {
        expect(reinterpret_cast<int (*)()>(memory)() == 0,
               "postload continuation returns");
        expect(state->calls == 1 &&
                   state->argument == state->before.registers[6] &&
                   state->observed_ready == 1 && state->master[0x318] == 1,
               "outer postload receives RSI master after native ready store");
        expect(state->callback_rsp % 16 == 8,
               "postload callback Windows alignment");
        for (unsigned reg{}; reg < 16; ++reg) {
          expect(state->after.registers[reg] == state->before.registers[reg],
                 "outer postload preserves every GPR including prepared RCX");
          expect(state->after.vectors[reg] == state->before.vectors[reg],
                 "postload preserves every XMM register");
        }
        expect(state->after.flags == state->before.flags,
               "ready store and recovery preserve native flags");
        expect(state->after.mxcsr == state->before.mxcsr &&
                   state->after.x87 == state->before.x87,
               "postload preserves floating-point control");
        expect(state->native_frame == original_frame,
               "native saved locals remain untouched");
      }
    }
    expect(VirtualFree(memory, 0, MEM_RELEASE) != 0,
           "postload harness released");
  }
  std::printf("connection bridge/profile: %d checks, %d failures\n", checks,
              failed);
  return failed;
}
