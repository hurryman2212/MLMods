#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include <Windows.h>

#include "ManorLoadPatch.hpp"

struct ManorLoadBridgeSnapshot {
  std::array<std::uint64_t, 16> registers{};
  std::array<std::array<std::uint8_t, 16>, 16> vectors{};
  std::uint64_t flags{};
  std::uint32_t mxcsr{};
  std::uint16_t x87_control{};
};

struct ManorLoadBridgeState {
  ManorLoadBridgeSnapshot before{}, after{};
  std::array<std::uint8_t, 0x80> native_stack{};
  std::array<std::uintptr_t, 3> arguments{};
  std::uintptr_t callback_rsp{};
  std::uint32_t calls{}, route{};
  std::uint32_t clobber_mxcsr{0x5f80};
  std::uint16_t clobber_x87{0x077f};
  std::array<std::uint64_t, 2> plan_header{};
  std::uintptr_t native_end{}, iterator_end{}, iterator_current{};
};

auto run_manor_load_bridge_tests() -> int {
  int checks{}, failures{};
  const auto expect = [&](bool success, const char *message) {
    ++checks;
    if (!success) {
      ++failures;
      std::fprintf(stderr, "manor load bridge: %s\n", message);
    }
  };
  const auto *placeholder = reinterpret_cast<const void *>(1);
  expect(manor_load_bridge(nullptr, placeholder, placeholder).empty(),
         "missing callback rejected");
  expect(manor_load_bridge(placeholder, nullptr, placeholder).empty(),
         "missing direct continuation rejected");
  expect(manor_load_bridge(placeholder, placeholder, nullptr).empty(),
         "missing assembly continuation rejected");
  expect(manor_load_begin_bridge(nullptr, placeholder).empty() &&
             manor_load_begin_bridge(placeholder, nullptr).empty() &&
             manor_load_end_bridge(nullptr, placeholder).empty() &&
             manor_load_end_bridge(placeholder, nullptr).empty(),
         "begin/end reject incomplete callback contracts");

  const std::array<std::pair<std::uint64_t, std::uint64_t>, 6> comparisons{
      {{0, 0},
       {1, 0},
       {0, 1},
       {0x8000000000000000ULL, UINT64_MAX},
       {0x7fffffffffffffffULL, UINT64_MAX},
       {0xfedcba98, 0xfedcba98}}};
  for (const unsigned kind : {0U, 1U, 2U}) {
    for (const auto &[left, right] : comparisons) {
      for (const bool eligible : {false, true}) {
        auto *memory = static_cast<std::uint8_t *>(VirtualAlloc(
            nullptr, 0x5000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (memory == nullptr)
          return failures + 1;
        auto *state = new (memory + 0x4000) ManorLoadBridgeState{};
        auto *patterns = memory + 0x4800;
        for (std::size_t i{}; i < 256; ++i)
          patterns[i] = static_cast<std::uint8_t>(i * 7 + 3);
        alignas(16) std::array<std::uint8_t, 0xb0> record{};
        alignas(16) std::array<std::uint8_t, 0x500> manor{};
        alignas(16) std::array<std::uint8_t, 0x400> definition{};
        record.fill(0x37);
        manor.fill(0x69);
        definition.fill(0x53);
        std::memcpy(definition.data() + 0x180, &left, sizeof(left));
        const auto original_data =
            reinterpret_cast<std::uintptr_t>(record.data());
        const std::uint64_t original_counts = 0x0000000300000003ULL;
        std::memcpy(definition.data() + 0x398, &original_data,
                    sizeof(original_data));
        std::memcpy(definition.data() + 0x3a0, &original_counts,
                    sizeof(original_counts));
        const std::uint32_t planned_count = left == 0 ? 0 : 2;
        state->plan_header = {
            planned_count == 0 ? 0
                               : reinterpret_cast<std::uintptr_t>(manor.data()),
            (static_cast<std::uint64_t>(planned_count) << 32) | planned_count};
        const auto original_record = record;
        const auto original_manor = manor;
        const auto original_definition = definition;

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
                                  ManorLoadBridgeSnapshot *destination) {
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
          code.insert(code.end(), {0x0f, 0xae, 0x1d});
          relative(code, origin, &destination->mxcsr);
          code.insert(code.end(), {0xd9, 0x3d});
          relative(code, origin, &destination->x87_control);
        };

        // Preserve the C++ caller, then reproduce native midpoint RSP%16==0.
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
            value = reinterpret_cast<std::uintptr_t>(definition.data());
          if (reg == 3)
            value = reinterpret_cast<std::uintptr_t>(definition.data() + 0x180);
          if (kind == 1 && reg == 5)
            value = reinterpret_cast<std::uintptr_t>(&state->native_end) + 0x80;
          if (reg == 13)
            value = reinterpret_cast<std::uintptr_t>(record.data());
          if (reg == 14)
            value = kind == 1
                        ? reinterpret_cast<std::uintptr_t>(definition.data())
                        : right;
          if (reg == 7)
            value = reinterpret_cast<std::uintptr_t>(manor.data());
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
        entry.insert(entry.end(), {0x4c, 0x39, 0x33});
        snapshot(entry, 0, &state->before);
        // Incoming carry differs from the original comparison in some cases;
        // the bridge must reproduce CMP rather than preserve stale entry flags.
        if (kind == 0)
          entry.push_back(0xf9);
        jump(entry, memory + 0x1000);

        // Deliberately clobber all fifteen GPRs, all sixteen XMM registers,
        // floating-point control, flags, and the entire Windows shadow space.
        std::vector<std::uint8_t> callback;
        for (unsigned i{}; i < 3; ++i) {
          const unsigned reg = i == 0 ? 1 : i == 1 ? 2 : 8;
          callback.insert(callback.end(),
                          {static_cast<std::uint8_t>(reg >= 8 ? 0x4c : 0x48),
                           0x89,
                           static_cast<std::uint8_t>(0x05 + (reg % 8) * 8)});
          relative(callback, 0x1800, &state->arguments[i]);
        }
        callback.insert(callback.end(), {0x48, 0x89, 0x25});
        relative(callback, 0x1800, &state->callback_rsp);
        callback.insert(callback.end(), {0xff, 0x05});
        relative(callback, 0x1800, &state->calls);
        for (std::uint8_t offset = 8; offset <= 36; offset += 4) {
          callback.insert(callback.end(), {0xc7, 0x44, 0x24, offset});
          append32(callback, static_cast<std::int32_t>(0xdeadbeefU));
        }
        callback.insert(callback.end(), {0x0f, 0xae, 0x15});
        relative(callback, 0x1800, &state->clobber_mxcsr);
        callback.insert(callback.end(), {0xd9, 0x2d});
        relative(callback, 0x1800, &state->clobber_x87);
        for (unsigned reg{}; reg < 16; ++reg) {
          callback.push_back(0x66);
          if (reg >= 8)
            callback.push_back(0x45);
          callback.insert(
              callback.end(),
              {0x0f, 0xef, static_cast<std::uint8_t>(0xc0 + (reg % 8) * 9)});
        }
        for (unsigned reg{}; reg < 16; ++reg)
          if (reg != 4)
            immediate(callback, reg, 0xdef0000000000000ULL + reg);
        immediate(callback, 0,
                  kind == 1 ? (eligible ? reinterpret_cast<std::uintptr_t>(
                                              state->plan_header.data())
                                        : 0)
                            : 0xdef0000000000000ULL +
                                  static_cast<unsigned>(eligible));
        callback.push_back(0xc3);

        const auto bridge =
            kind == 1
                ? manor_load_begin_bridge(memory + 0x1800, memory + 0x2000)
            : kind == 2
                ? manor_load_end_bridge(memory + 0x1800, memory + 0x2000)
                : manor_load_bridge(memory + 0x1800, memory + 0x2000,
                                    memory + 0x2040);
        std::array<std::vector<std::uint8_t>, 2> routes;
        for (std::size_t i{}; i < routes.size(); ++i) {
          auto &route = routes[i];
          route.insert(route.end(), {0xc7, 0x05});
          const auto displacement =
              reinterpret_cast<std::uint8_t *>(&state->route) -
              (memory + 0x2000 + i * 0x40 + 10);
          append32(route, static_cast<std::int32_t>(displacement));
          append32(route, static_cast<std::int32_t>(i + 1));
          if (kind == 1 && i == 0) {
            snapshot(route, 0x2000, &state->after);
            // Execute the native cached-end calculation and empty-plan branch.
            route.insert(route.end(),
                         {0x4c, 0x69, 0xe0, 0xb0, 0,    0,    0,    0x4d,
                          0x03, 0xe5, 0x4c, 0x89, 0x65, 0x80, 0x4d, 0x3b,
                          0xec, 0x74, 0x07, 0x4d, 0x8d, 0xbd, 0xa8, 0,
                          0,    0,    0x4c, 0x89, 0x25});
            relative(route, 0x2000, &state->iterator_end);
            route.insert(route.end(), {0x4c, 0x89, 0x3d});
            relative(route, 0x2000, &state->iterator_current);
          }
          jump(route, memory + 0x2400);
        }
        std::vector<std::uint8_t> resume;
        if (kind != 1)
          snapshot(resume, 0x2400, &state->after);
        for (std::int32_t offset{}; offset < 0x80; offset += 8) {
          resume.insert(resume.end(), {0x48, 0x8b, 0x84, 0x24});
          append32(resume, offset);
          resume.insert(resume.end(), {0x48, 0x89, 0x05});
          relative(resume, 0x2400, state->native_stack.data() + offset);
        }
        for (unsigned reg = 6; reg < 16; ++reg)
          vector_stack(resume, reg, true);
        resume.insert(resume.end(), {0x48, 0x81, 0xc4});
        append32(resume, 0x188);
        resume.insert(resume.end(),
                      {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5f,
                       0x5e, 0x5d, 0x5b, 0x31, 0xc0, 0xc3});
        const bool layout = entry.size() <= 0x1000 && bridge.size() <= 0x800 &&
                            callback.size() <= 0x800 &&
                            resume.size() <= 0x1c00 &&
                            (kind != 1 || routes[0].size() < 0x400);
        expect(layout, "synthetic code sections do not overlap");
        if (!layout) {
          VirtualFree(memory, 0, MEM_RELEASE);
          continue;
        }
        std::memcpy(memory, entry.data(), entry.size());
        std::memcpy(memory + 0x1000, bridge.data(), bridge.size());
        std::memcpy(memory + 0x1800, callback.data(), callback.size());
        for (std::size_t i{}; i < (kind == 1 ? 1 : routes.size()); ++i)
          std::memcpy(memory + 0x2000 + i * 0x40, routes[i].data(),
                      routes[i].size());
        std::memcpy(memory + 0x2400, resume.data(), resume.size());
        DWORD previous{};
        const bool executable =
            VirtualProtect(memory, 0x4000, PAGE_EXECUTE_READ, &previous) != 0 &&
            FlushInstructionCache(GetCurrentProcess(), memory, 0x4000) != 0;
        expect(executable,
               "code is RX, state is RW, instruction cache is flushed");
        if (executable) {
          expect(reinterpret_cast<int (*)()>(memory)() == 0,
                 "bridge reaches native continuation");
          expect(state->before.registers[4] % 16 == 0 &&
                     state->callback_rsp % 16 == 8,
                 "native midpoint and Windows callback alignment are correct");
          expect(state->calls == 1, "exactly one callback executes");
          expect(state->route ==
                     (kind != 0 || eligible || left == right ? 1U : 2U),
                 "eligible forces direct; ineligible preserves native CMP/Jcc");
          expect(
              kind != 0 ||
                  (state->arguments[0] ==
                       reinterpret_cast<std::uintptr_t>(record.data()) &&
                   state->arguments[1] ==
                       reinterpret_cast<std::uintptr_t>(manor.data()) &&
                   state->arguments[2] ==
                       reinterpret_cast<std::uintptr_t>(definition.data())),
              "callback receives R13 record, RDI manor, full RAX definition");
          expect(kind != 1 ||
                     (state->arguments[0] == state->before.registers[6] &&
                      state->arguments[1] == state->before.registers[7] &&
                      state->arguments[2] == state->before.registers[14]),
                 "begin receives original master, manor and saved-building "
                 "object");
          expect(kind != 2 || state->arguments[0] == state->before.registers[7],
                 "end receives original manor");
          for (unsigned reg{}; reg < 16; ++reg) {
            auto expected = state->before.registers[reg];
            if (kind == 1 && reg == 0)
              expected = eligible ? planned_count : 3;
            if (kind == 1 && reg == 13)
              expected = eligible ? state->plan_header[0] : original_data;
            if (kind == 2 && reg == 2)
              expected = state->before.registers[5] + 0x248;
            expect(expected == state->after.registers[reg],
                   "GPRs preserved except intentional native replay/iteration "
                   "values");
            expect(state->before.vectors[reg] == state->after.vectors[reg],
                   "every XMM register is preserved");
          }
          expect(state->before.flags == state->after.flags,
                 "complete flags equal original CMP result on both routes");
          expect(state->before.mxcsr == state->after.mxcsr &&
                     state->before.x87_control == state->after.x87_control,
                 "floating-point control survives hostile callback");
          if (kind == 1) {
            const auto start = eligible ? state->plan_header[0] : original_data;
            const auto count = eligible ? planned_count : 3;
            const auto end = start + static_cast<std::uintptr_t>(count) * 0xb0;
            expect(
                state->native_end == end && state->iterator_end == end,
                "native R12 and cached [RBP-0x80] both use replacement count");
            expect(
                state->iterator_current ==
                    (count == 0 ? state->before.registers[15] : start + 0xa8),
                "native R15 initialization and empty-plan branch use "
                "replacement range");
          }
          for (const auto byte : state->native_stack)
            expect(byte == 0xa5, "original native stack is untouched");
          expect(record == original_record && manor == original_manor &&
                     definition == original_definition,
                 "record, manor and definition remain unchanged");
        }
        expect(VirtualFree(memory, 0, MEM_RELEASE) != 0,
               "synthetic code allocation released");
      }
    }
  }
  std::printf("manor load bridge: %d checks, %d failures\n", checks, failures);
  return failures;
}
