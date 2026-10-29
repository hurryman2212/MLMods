#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include <Windows.h>

#include "TreeHooks.hpp"

struct TreeBridgeSnapshot {
  std::array<std::uint64_t, 16> registers{};
  std::array<std::array<std::uint8_t, 16>, 16> vectors{};
  std::uint64_t flags{};
};

struct TreeBridgeState {
  TreeBridgeSnapshot before{}, after{};
  std::array<std::uint8_t, 0x80> native_stack{};
  std::array<std::uintptr_t, 3> arguments{};
  std::uintptr_t callback_rsp{};
  std::uint32_t calls{};
};

auto run_tree_bridge_tests() -> int {
  int failures{}, checks{};
  const auto expect = [&](bool success, const char *message) {
    ++checks;
    if (!success) {
      ++failures;
      std::fprintf(stderr, "tree bridge: %s\n", message);
    }
  };
  expect(tree_region_bridge(false, nullptr, reinterpret_cast<const void *>(1))
             .empty(),
         "missing callback cannot create an executable bridge");
  expect(tree_region_bridge(true, reinterpret_cast<const void *>(1), nullptr)
             .empty(),
         "missing continuation cannot create an executable bridge");
  for (bool component : {false, true}) {
    for (std::uintptr_t returned :
         {std::uintptr_t{0}, std::uintptr_t{0x123456789abcdef0}}) {
      auto *memory = static_cast<std::uint8_t *>(VirtualAlloc(
          nullptr, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
      if (memory == nullptr)
        return failures + 1;
      auto *state = new (memory + 0x2000) TreeBridgeState{};
      auto *patterns = memory + 0x2400;
      for (std::size_t i{}; i < 256; ++i)
        patterns[i] = static_cast<std::uint8_t>(i * 7 + 3);
      alignas(16) std::array<std::uint8_t, 0x360> unit{};
      alignas(16) std::array<std::uint8_t, 0x80> foliage{};
      unit.fill(0x69);
      foliage.fill(0x53);
      constexpr std::uintptr_t original_region = 0x123456789abcdef0;
      std::memcpy(unit.data() + 0x328, &original_region,
                  sizeof(original_region));
      const auto original_unit = unit;
      const auto original_foliage = foliage;

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
      const auto relative = [&](std::vector<std::uint8_t> &code,
                                std::size_t origin, const void *target) {
        const auto displacement = tree_relative_displacement(
            reinterpret_cast<std::uintptr_t>(memory + origin + code.size() + 4),
            reinterpret_cast<std::uintptr_t>(target));
        expect(displacement.has_value(), "synthetic RIP displacement fits");
        append32(code, displacement.value_or(0));
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
                                TreeBridgeSnapshot *destination) {
        for (unsigned reg{}; reg < 16; ++reg) {
          code.insert(code.end(),
                      {static_cast<std::uint8_t>(reg >= 8 ? 0x4c : 0x48), 0x89,
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

      // Preserve the C++ caller, then reproduce the native RSP%16==0 midpoint.
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
        if (reg == 14)
          value = reinterpret_cast<std::uintptr_t>(unit.data());
        if (component && reg == 7)
          value = reinterpret_cast<std::uintptr_t>(foliage.data());
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
      entry.insert(entry.end(), {0x39, 0xc8});
      snapshot(entry, 0, &state->before);
      append_tree_jump(entry, memory + 0x800);

      // This hostile ABI-compliant callback overwrites the complete 32-byte
      // caller shadow space, volatile registers and XMM0..5, and arithmetic
      // flags. Nonvolatile registers are preserved by the callback contract.
      std::vector<std::uint8_t> callback;
      for (unsigned i{}; i < 3; ++i) {
        const unsigned reg = i == 0 ? 1 : i == 1 ? 2 : 8;
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
      immediate(callback, 0, returned);
      callback.push_back(0xc3);

      const auto bridge =
          tree_region_bridge(component, memory + 0xa00, memory + 0xc00);
      std::vector<std::uint8_t> resume;
      snapshot(resume, 0xc00, &state->after);
      for (std::int32_t offset{}; offset < 0x80; offset += 8) {
        resume.insert(resume.end(), {0x48, 0x8b, 0x84, 0x24});
        append32(resume, offset);
        resume.insert(resume.end(), {0x48, 0x89, 0x05});
        relative(resume, 0xc00, state->native_stack.data() + offset);
      }
      for (unsigned reg = 6; reg < 16; ++reg)
        vector_stack(resume, reg, true);
      resume.insert(resume.end(), {0x48, 0x81, 0xc4});
      append32(resume, 0x188);
      resume.insert(resume.end(),
                    {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5f, 0x5e,
                     0x5d, 0x5b, 0x31, 0xc0, 0xc3});
      const bool layout = entry.size() <= 0x800 && bridge.size() <= 0x200 &&
                          callback.size() <= 0x200 && resume.size() <= 0x800;
      expect(layout, "bridge harness code sections do not overlap");
      if (!layout) {
        VirtualFree(memory, 0, MEM_RELEASE);
        continue;
      }
      std::memcpy(memory, entry.data(), entry.size());
      std::memcpy(memory + 0x800, bridge.data(), bridge.size());
      std::memcpy(memory + 0xa00, callback.data(), callback.size());
      std::memcpy(memory + 0xc00, resume.data(), resume.size());
      DWORD old{};
      const bool executable =
          VirtualProtect(memory, 0x2000, PAGE_EXECUTE_READ, &old) != 0 &&
          FlushInstructionCache(GetCurrentProcess(), memory, 0x2000) != 0;
      expect(executable,
             "code pages RX, state page RW, instruction cache flushed");
      if (executable) {
        expect(reinterpret_cast<int (*)()>(memory)() == 0,
               "bridge resumes through native continuation");
        expect(state->before.registers[4] % 16 == 0 &&
                   state->callback_rsp % 16 == 8,
               "native midpoint and callback stack alignment are exact");
        expect(state->calls == 1 &&
                   state->arguments[0] ==
                       reinterpret_cast<std::uintptr_t>(unit.data()),
               "callback called once with original R14 unit");
        expect(state->arguments[component ? 2 : 1] == original_region,
               "callback receives original unit+0x328 region");
        if (component)
          expect(state->arguments[1] ==
                     reinterpret_cast<std::uintptr_t>(foliage.data()),
                 "second bridge receives original RDI foliage component");
        const unsigned output = component ? 3U : 7U;
        for (unsigned reg{}; reg < 16; ++reg)
          expect(state->after.registers[reg] ==
                     (reg == output ? returned : state->before.registers[reg]),
                 "only requested output GPR changes; all others and RSP "
                 "preserved");
        for (unsigned reg{}; reg < 16; ++reg)
          expect(state->before.vectors[reg] == state->after.vectors[reg],
                 "full XMM0..15 state preserved");
        expect(state->before.flags == state->after.flags,
               "RFLAGS preserved across hostile callback");
        for (std::uint8_t byte : state->native_stack)
          expect(byte == 0xa5,
                 "native stack bytes survive callback shadow writes");
        expect(unit == original_unit && foliage == original_foliage,
               "unit Region and every original object byte remain unchanged");
      }
      expect(VirtualFree(memory, 0, MEM_RELEASE) != 0,
             "synthetic bridge allocation released");
    }
  }
  std::printf("tree bridge: %d checks, %d failures\n", checks, failures);
  return failures;
}
