#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include <Windows.h>

#include "EraserHooks.hpp"

struct EraserBridgeSnapshot {
  std::array<std::uint64_t, 16> registers{};
  std::array<std::array<std::uint8_t, 16>, 16> vectors{};
  std::uint64_t flags{};
};

struct EraserBridgeState {
  EraserBridgeSnapshot before{}, after{};
  std::array<std::uint8_t, 0x80> native_stack{};
  std::array<std::uintptr_t, 4> arguments{};
  std::uintptr_t callback_rsp{};
  std::uint32_t calls{}, route{};
};

auto run_eraser_bridge_tests() -> int {
  int checks{}, failures{};
  const auto expect = [&](bool success, const char *message) {
    ++checks;
    if (!success) {
      ++failures;
      std::fprintf(stderr, "eraser bridge: %s\n", message);
    }
  };
  const auto *placeholder = reinterpret_cast<const void *>(1);
  expect(eraser_filter_bridge(nullptr, placeholder, placeholder, placeholder)
             .empty(),
         "filter rejects missing callback");
  expect(eraser_filter_bridge(placeholder, nullptr, placeholder, placeholder)
             .empty(),
         "filter rejects missing allowed-tree continuation");
  expect(eraser_filter_bridge(placeholder, placeholder, nullptr, placeholder)
             .empty(),
         "filter rejects missing skipped-tree continuation");
  expect(eraser_filter_bridge(placeholder, placeholder, placeholder, nullptr)
             .empty(),
         "filter rejects missing native non-tree continuation");
  expect(eraser_cleanup_bridge(nullptr, placeholder).empty() &&
             eraser_cleanup_bridge(placeholder, nullptr).empty(),
         "cleanup rejects incomplete callback and continuation");

  for (bool cleanup : {false, true}) {
    for (bool zero : {false, true}) {
      for (bool result : {false, true}) {
        auto *memory = static_cast<std::uint8_t *>(VirtualAlloc(
            nullptr, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (memory == nullptr)
          return failures + 1;
        auto *state = new (memory + 0x2000) EraserBridgeState{};
        auto *patterns = memory + 0x2400;
        for (std::size_t i{}; i < 256; ++i)
          patterns[i] = static_cast<std::uint8_t>(i * 7 + 3);
        alignas(16) std::array<std::uint8_t, 0x1300> pawn{};
        alignas(16) std::array<std::uint8_t, 0x80> component{};
        pawn.fill(0x69);
        component.fill(0x53);
        const auto original_pawn = pawn;
        const auto original_component = component;

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
        const auto jump = [](std::vector<std::uint8_t> &code,
                             const void *destination) {
          code.insert(code.end(), {0xff, 0x25, 0, 0, 0, 0});
          const auto address = reinterpret_cast<std::uintptr_t>(destination);
          const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
          code.insert(code.end(), bytes, bytes + sizeof(address));
        };
        const auto relative = [&](std::vector<std::uint8_t> &code,
                                  std::size_t origin, const void *target) {
          const auto displacement =
              reinterpret_cast<const std::uint8_t *>(target) -
              (memory + origin + code.size() + 4);
          expect(displacement >= INT32_MIN && displacement <= INT32_MAX,
                 "synthetic RIP displacement fits");
          append32(code, static_cast<std::int32_t>(displacement));
        };
        const auto vector_stack = [&](std::vector<std::uint8_t> &code,
                                      unsigned reg, bool load) {
          code.push_back(0xf3);
          if (reg >= 8)
            code.push_back(0x44);
          code.insert(code.end(),
                      {0x0f, static_cast<std::uint8_t>(load ? 0x6f : 0x7f),
                       static_cast<std::uint8_t>(0x84 + (reg % 8) * 8), 0x24});
          append32(code, static_cast<std::int32_t>(0x80 + (reg - 6) * 16));
        };
        const auto snapshot = [&](std::vector<std::uint8_t> &code,
                                  std::size_t origin,
                                  EraserBridgeSnapshot *destination) {
          for (unsigned reg{}; reg < 16; ++reg) {
            code.insert(code.end(),
                        {static_cast<std::uint8_t>(reg >= 8 ? 0x4c : 0x48),
                         0x89,
                         static_cast<std::uint8_t>(0x05 + (reg % 8) * 8)});
            relative(code, origin, &destination->registers[reg]);
          }
          for (unsigned reg{}; reg < 16; ++reg) {
            code.push_back(0xf3);
            if (reg >= 8)
              code.push_back(0x44);
            code.insert(
                code.end(),
                {0x0f, 0x7f, static_cast<std::uint8_t>(0x05 + (reg % 8) * 8)});
            relative(code, origin, &destination->vectors[reg]);
          }
          code.insert(code.end(), {0x9c, 0x8f, 0x05});
          relative(code, origin, &destination->flags);
        };

        // Preserve the C++ caller, then reproduce the native RSP%16==0
        // midpoint.
        std::vector<std::uint8_t> entry{0x53, 0x55, 0x56, 0x57, 0x41,
                                        0x54, 0x41, 0x55, 0x41, 0x56,
                                        0x41, 0x57, 0x48, 0x81, 0xec};
        append32(entry, 0x188);
        for (unsigned reg = 6; reg < 16; ++reg)
          vector_stack(entry, reg, false);
        for (std::int32_t offset{}; offset < 0x80; offset += 4) {
          entry.insert(entry.end(), {0xc7, 0x84, 0x24});
          append32(entry, offset);
          append32(entry, static_cast<std::int32_t>(0xa5a5a5a5U));
        }
        for (unsigned reg{}; reg < 16; ++reg) {
          if (reg == 4)
            continue;
          std::uintptr_t value = 0xabc0000000000000ULL + reg;
          if (reg == 0)
            value = 0xabc0000000000000ULL + static_cast<unsigned>(result);
          if (reg == 13)
            value = reinterpret_cast<std::uintptr_t>(pawn.data());
          if (reg == 14)
            value = reinterpret_cast<std::uintptr_t>(component.data());
          immediate(entry, reg, value);
        }
        for (unsigned reg{}; reg < 16; ++reg) {
          entry.push_back(0xf3);
          if (reg >= 8)
            entry.push_back(0x44);
          entry.insert(
              entry.end(),
              {0x0f, 0x6f, static_cast<std::uint8_t>(0x05 + (reg % 8) * 8)});
          relative(entry, 0, patterns + reg * 16);
        }
        entry.insert(entry.end(),
                     {0x39, static_cast<std::uint8_t>(zero ? 0xc9 : 0xd1)});
        snapshot(entry, 0, &state->before);
        jump(entry, memory + 0x800);

        // The callback deliberately overwrites all ABI-volatile registers,
        // flags, XMM0..5, and every byte of its caller-provided shadow space.
        std::vector<std::uint8_t> callback;
        for (unsigned i{}; i < 4; ++i) {
          const unsigned reg = i == 0 ? 1 : i == 1 ? 2 : i == 2 ? 8 : 9;
          callback.insert(callback.end(),
                          {static_cast<std::uint8_t>(reg >= 8 ? 0x4c : 0x48),
                           0x89,
                           static_cast<std::uint8_t>(0x05 + (reg % 8) * 8)});
          relative(callback, 0xa00, &state->arguments[i]);
        }
        callback.insert(callback.end(), {0x48, 0x89, 0x25});
        relative(callback, 0xa00, &state->callback_rsp);
        callback.insert(callback.end(), {0xff, 0x05});
        relative(callback, 0xa00, &state->calls);
        for (std::uint8_t offset = 8; offset <= 36; offset += 4) {
          callback.insert(callback.end(), {0xc7, 0x44, 0x24, offset});
          append32(callback, static_cast<std::int32_t>(0xdeadbeefU));
        }
        callback.insert(callback.end(), {0x31, 0xc0});
        for (unsigned reg{}; reg < 6; ++reg)
          callback.insert(
              callback.end(),
              {0x66, 0x0f, 0xef, static_cast<std::uint8_t>(0xc0 + reg * 9)});
        for (const unsigned reg : {0U, 1U, 2U, 8U, 9U, 10U, 11U})
          immediate(callback, reg, 0xdef0000000000000ULL + reg);
        immediate(callback, 0,
                  0xdef0000000000000ULL + static_cast<unsigned>(result));
        callback.push_back(0xc3);

        const auto bridge =
            cleanup ? eraser_cleanup_bridge(memory + 0xa00, memory + 0xc00)
                    : eraser_filter_bridge(memory + 0xa00, memory + 0xc00,
                                           memory + 0xc40, memory + 0xc80);
        std::array<std::vector<std::uint8_t>, 3> routes;
        for (std::size_t i{}; i < routes.size(); ++i) {
          auto &route = routes[i];
          route.insert(route.end(), {0xc7, 0x05});
          const auto displacement =
              reinterpret_cast<std::uint8_t *>(&state->route) -
              (memory + 0xc00 + i * 0x40 + 10);
          append32(route, static_cast<std::int32_t>(displacement));
          append32(route, static_cast<std::int32_t>(i + 1));
          jump(route, memory + 0xd00);
        }
        std::vector<std::uint8_t> resume;
        snapshot(resume, 0xd00, &state->after);
        for (std::int32_t offset{}; offset < 0x80; offset += 8) {
          resume.insert(resume.end(), {0x48, 0x8b, 0x84, 0x24});
          append32(resume, offset);
          resume.insert(resume.end(), {0x48, 0x89, 0x05});
          relative(resume, 0xd00, state->native_stack.data() + offset);
        }
        for (unsigned reg = 6; reg < 16; ++reg)
          vector_stack(resume, reg, true);
        resume.insert(resume.end(), {0x48, 0x81, 0xc4});
        append32(resume, 0x188);
        resume.insert(resume.end(),
                      {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5f,
                       0x5e, 0x5d, 0x5b, 0x31, 0xc0, 0xc3});
        const bool layout = entry.size() <= 0x800 && bridge.size() <= 0x200 &&
                            callback.size() <= 0x200 && resume.size() <= 0x1300;
        expect(layout, "bridge harness code sections do not overlap");
        if (!layout) {
          VirtualFree(memory, 0, MEM_RELEASE);
          continue;
        }
        std::memcpy(memory, entry.data(), entry.size());
        std::memcpy(memory + 0x800, bridge.data(), bridge.size());
        std::memcpy(memory + 0xa00, callback.data(), callback.size());
        for (std::size_t i{}; i < routes.size(); ++i)
          std::memcpy(memory + 0xc00 + i * 0x40, routes[i].data(),
                      routes[i].size());
        std::memcpy(memory + 0xd00, resume.data(), resume.size());
        DWORD old{};
        const bool executable =
            VirtualProtect(memory, 0x2000, PAGE_EXECUTE_READ, &old) != 0 &&
            FlushInstructionCache(GetCurrentProcess(), memory, 0x2000) != 0;
        expect(executable, "code pages RX, state page RW, cache flushed");
        if (executable) {
          expect(reinterpret_cast<int (*)()>(memory)() == 0,
                 "bridge resumes through a native continuation");
          const bool calls = cleanup || zero;
          expect(state->before.registers[4] % 16 == 0 &&
                     (!calls || state->callback_rsp % 16 == 8),
                 "native midpoint and callback stack alignment are exact");
          expect(state->calls == static_cast<unsigned>(calls),
                 "native non-tree path bypasses filter callback");
          expect(
              state->route == (cleanup  ? 1U
                               : !zero  ? 3U
                               : result ? 1U
                                        : 2U),
              "filter branches and cleanup resume select correct native path");
          if (calls) {
            expect(state->arguments[0] ==
                           reinterpret_cast<std::uintptr_t>(pawn.data()) &&
                       state->arguments[1] ==
                           reinterpret_cast<std::uintptr_t>(component.data()),
                   "callback receives original R13 pawn and R14 component");
            if (cleanup) {
              expect(state->arguments[2] == state->before.registers[4] + 0x50,
                     "cleanup receives selected XYZ header on original native "
                     "stack");
              expect((state->arguments[3] & 0xff) ==
                         static_cast<unsigned>(result),
                     "cleanup receives original native AL success flag");
            }
          }
          for (unsigned reg{}; reg < 16; ++reg)
            expect(
                state->after.registers[reg] ==
                    (cleanup && reg == 8 ? 0xa5a5a5a5ULL
                                         : state->before.registers[reg]),
                "all GPRs and RSP preserved except replayed native R8D load");
          for (unsigned reg{}; reg < 16; ++reg)
            expect(state->before.vectors[reg] == state->after.vectors[reg],
                   "full XMM0..15 state preserved");
          expect(state->before.flags == state->after.flags,
                 "RFLAGS preserved across hostile callback");
          for (std::uint8_t byte : state->native_stack)
            expect(byte == 0xa5,
                   "original native stack survives callback shadow writes");
          expect(pawn == original_pawn && component == original_component,
                 "pawn and component bytes remain unchanged by bridges");
        }
        expect(VirtualFree(memory, 0, MEM_RELEASE) != 0,
               "synthetic bridge allocation released");
      }
    }
  }
  std::printf("eraser bridge: %d checks, %d failures\n", checks, failures);
  return failures;
}
