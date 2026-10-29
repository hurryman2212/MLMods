#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include <Windows.h>

#include "RoadRuleBridge.hpp"

struct RuleSnapshot {
  std::array<std::uint64_t, 16> registers{};
  std::array<std::array<std::uint8_t, 16>, 16> vectors{};
  std::uint64_t flags{};
  std::uint32_t mxcsr{};
  std::uint16_t control{};
};

struct RuleState {
  RuleSnapshot before{}, after{};
  std::array<std::uint8_t, 0x100> native_stack{};
  std::uintptr_t argument{}, callback_rsp{};
  std::uint32_t calls{}, route{};
  std::uint32_t clobber_mxcsr{0x5f80};
  std::uint16_t clobber_control{0x077f};
  std::array<std::uint8_t, 0x850> pawn{};
};

auto main() -> int {
  unsigned checks{}, failures{};
  const auto expect = [&](bool ok, const char *message) {
    ++checks;
    if (!ok) {
      ++failures;
      std::fprintf(stderr, "road rule bridge: %s\n", message);
    }
  };
  const auto placeholder = reinterpret_cast<void *>(1);
  expect(make_road_rule_bridge(nullptr, placeholder, placeholder).empty() &&
             make_road_rule_bridge(placeholder, nullptr, placeholder).empty() &&
             make_road_rule_bridge(placeholder, placeholder, nullptr).empty(),
         "missing callback or continuation rejected");
  for (const bool accepted : {false, true}) {
    for (const std::uint8_t castle : {0, 1, 0x80}) {
      for (const std::uint8_t compare : {0, 1, 0x80}) {
        auto *memory = static_cast<std::uint8_t *>(VirtualAlloc(
            nullptr, 0x5000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (memory == nullptr)
          return 2;
        auto *state = new (memory + 0x4000) RuleState;
        static_assert(sizeof(RuleState) < 0xf00);
        state->pawn[0x848] = castle;
        auto *patterns = memory + 0x4f00;
        for (std::size_t i{}; i < 256; ++i)
          patterns[i] = static_cast<std::uint8_t>(i * 13 + 7);
        const auto append32 = [](std::vector<std::uint8_t> &code,
                                 std::int32_t value) {
          const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
          code.insert(code.end(), bytes, bytes + sizeof(value));
        };
        const auto relative = [&](std::vector<std::uint8_t> &code,
                                  std::size_t origin, const void *target) {
          const auto difference =
              reinterpret_cast<const std::uint8_t *>(target) -
              (memory + origin + code.size() + 4);
          expect(difference >= INT32_MIN && difference <= INT32_MAX,
                 "synthetic relative address fits");
          append32(code, static_cast<std::int32_t>(difference));
        };
        const auto immediate = [](std::vector<std::uint8_t> &code, unsigned reg,
                                  std::uintptr_t value) {
          code.push_back(reg >= 8 ? 0x49 : 0x48);
          code.push_back(static_cast<std::uint8_t>(0xb8 + reg % 8));
          const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
          code.insert(code.end(), bytes, bytes + sizeof(value));
        };
        const auto vector_stack = [&](std::vector<std::uint8_t> &code,
                                      unsigned reg, bool load) {
          code.push_back(0xf3);
          if (reg >= 8)
            code.push_back(0x44);
          code.insert(code.end(),
                      {0x0f, static_cast<std::uint8_t>(load ? 0x6f : 0x7f),
                       static_cast<std::uint8_t>(0x84 + reg % 8 * 8), 0x24});
          append32(code, static_cast<std::int32_t>(0x100 + (reg - 6) * 16));
        };
        const auto snapshot = [&](std::vector<std::uint8_t> &code,
                                  std::size_t origin, RuleSnapshot *target) {
          for (unsigned reg{}; reg < 16; ++reg) {
            code.insert(code.end(),
                        {static_cast<std::uint8_t>(reg >= 8 ? 0x4c : 0x48),
                         0x89, static_cast<std::uint8_t>(0x05 + reg % 8 * 8)});
            relative(code, origin, &target->registers[reg]);
          }
          for (unsigned reg{}; reg < 16; ++reg) {
            code.push_back(0xf3);
            if (reg >= 8)
              code.push_back(0x44);
            code.insert(
                code.end(),
                {0x0f, 0x7f, static_cast<std::uint8_t>(0x05 + reg % 8 * 8)});
            relative(code, origin, &target->vectors[reg]);
          }
          code.insert(code.end(), {0x9c, 0x8f, 0x05});
          relative(code, origin, &target->flags);
          code.insert(code.end(), {0x0f, 0xae, 0x1d});
          relative(code, origin, &target->mxcsr);
          code.insert(code.end(), {0xd9, 0x3d});
          relative(code, origin, &target->control);
        };

        // Preserve the C++ caller and reproduce native midpoint alignment.
        std::vector<std::uint8_t> entry{0x53, 0x55, 0x56, 0x57, 0x41,
                                        0x54, 0x41, 0x55, 0x41, 0x56,
                                        0x41, 0x57, 0x48, 0x81, 0xec};
        append32(entry, 0x1a8);
        for (unsigned reg = 6; reg < 16; ++reg)
          vector_stack(entry, reg, false);
        for (std::int32_t offset{}; offset < 0x100; offset += 4) {
          entry.insert(entry.end(), {0xc7, 0x84, 0x24});
          append32(entry, offset);
          append32(entry, static_cast<std::int32_t>(0xa5a5a5a5U));
        }
        for (unsigned reg{}; reg < 16; ++reg) {
          if (reg == 4)
            continue;
          const auto value =
              reg == 7    ? reinterpret_cast<std::uintptr_t>(state->pawn.data())
              : reg == 12 ? 0xabc0000000000000ULL + compare
                          : 0xabc0000000000000ULL + reg;
          immediate(entry, reg, value);
        }
        for (unsigned reg{}; reg < 16; ++reg) {
          entry.push_back(0xf3);
          if (reg >= 8)
            entry.push_back(0x44);
          entry.insert(
              entry.end(),
              {0x0f, 0x6f, static_cast<std::uint8_t>(0x05 + reg % 8 * 8)});
          relative(entry, 0, patterns + reg * 16);
        }
        entry.push_back(0xf9); // Carry set before the displaced native CMP.
        snapshot(entry, 0, &state->before);
        append_absolute_jump(entry, memory + 0x1000);

        // Deliberately clobber even nonvolatile registers; the midpoint must
        // preserve every live register independently of the callback ABI.
        std::vector<std::uint8_t> callback{0x48, 0x89, 0x0d};
        relative(callback, 0x1800, &state->argument);
        callback.insert(callback.end(), {0x48, 0x89, 0x25});
        relative(callback, 0x1800, &state->callback_rsp);
        callback.insert(callback.end(), {0xff, 0x05});
        relative(callback, 0x1800, &state->calls);
        callback.insert(callback.end(), {0x0f, 0xae, 0x15});
        relative(callback, 0x1800, &state->clobber_mxcsr);
        callback.insert(callback.end(), {0xd9, 0x2d});
        relative(callback, 0x1800, &state->clobber_control);
        for (std::uint8_t offset = 8; offset < 40; offset += 4) {
          callback.insert(callback.end(), {0xc7, 0x44, 0x24, offset});
          append32(callback, static_cast<std::int32_t>(0xdeadbeefU));
        }
        for (unsigned reg{}; reg < 16; ++reg) {
          callback.push_back(0x66);
          if (reg >= 8)
            callback.push_back(0x45);
          callback.insert(
              callback.end(),
              {0x0f, 0xef, static_cast<std::uint8_t>(0xc0 + reg % 8 * 9)});
        }
        for (unsigned reg{}; reg < 16; ++reg)
          if (reg != 4)
            immediate(callback, reg, 0xdef0000000000000ULL + reg);
        callback.insert(callback.end(),
                        {0xb0, static_cast<std::uint8_t>(accepted), 0xc3});

        std::vector<std::uint8_t> fallthrough{0xc7, 0x05};
        relative(fallthrough, 0x2000, &state->route);
        append32(fallthrough, 1);
        // C7 RIP displacement is relative to the end of its immediate too.
        std::int32_t fix{};
        std::memcpy(&fix, fallthrough.data() + 2, sizeof(fix));
        fix -= 4;
        std::memcpy(fallthrough.data() + 2, &fix, sizeof(fix));
        append_absolute_jump(fallthrough, memory + 0x2400);
        auto blocked = fallthrough;
        const auto blocked_offset = static_cast<std::int32_t>(0x100);
        fix -= blocked_offset;
        std::memcpy(blocked.data() + 2, &fix, sizeof(fix));
        const std::int32_t two = 2;
        std::memcpy(blocked.data() + 6, &two, sizeof(two));

        std::vector<std::uint8_t> resume;
        snapshot(resume, 0x2400, &state->after);
        for (std::int32_t offset{}; offset < 0x100; offset += 8) {
          resume.insert(resume.end(), {0x48, 0x8b, 0x84, 0x24});
          append32(resume, offset);
          resume.insert(resume.end(), {0x48, 0x89, 0x05});
          relative(resume, 0x2400, state->native_stack.data() + offset);
        }
        for (unsigned reg = 6; reg < 16; ++reg)
          vector_stack(resume, reg, true);
        resume.insert(resume.end(), {0x48, 0x81, 0xc4});
        append32(resume, 0x1a8);
        resume.insert(resume.end(),
                      {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5f,
                       0x5e, 0x5d, 0x5b, 0x31, 0xc0, 0xc3});
        const auto bridge = make_road_rule_bridge(
            memory + 0x1800, memory + 0x2000, memory + 0x2100);
        const bool layout = entry.size() <= 0x1000 && bridge.size() <= 0x800 &&
                            callback.size() <= 0x800 &&
                            fallthrough.size() <= 0x100 &&
                            resume.size() <= 0x1c00;
        expect(layout, "synthetic code regions do not overlap");
        if (layout) {
          std::memcpy(memory, entry.data(), entry.size());
          std::memcpy(memory + 0x1000, bridge.data(), bridge.size());
          std::memcpy(memory + 0x1800, callback.data(), callback.size());
          std::memcpy(memory + 0x2000, fallthrough.data(), fallthrough.size());
          std::memcpy(memory + 0x2100, blocked.data(), blocked.size());
          std::memcpy(memory + 0x2400, resume.data(), resume.size());
          DWORD previous{};
          const bool executable =
              VirtualProtect(memory, 0x4000, PAGE_EXECUTE_READ, &previous) &&
              FlushInstructionCache(GetCurrentProcess(), memory, 0x4000);
          expect(executable,
                 "code executable and state remains separate writable memory");
          if (executable) {
            reinterpret_cast<void (*)()>(memory)();
            const bool override = accepted && compare == 0;
            const auto left = override ? std::uint8_t{0} : castle;
            const auto result = static_cast<std::uint8_t>(left - compare);
            unsigned ones{};
            for (unsigned bit{}; bit < 8; ++bit)
              ones += (result >> bit) & 1U;
            const std::uint64_t arithmetic =
                (left < compare ? 1U : 0U) | (ones % 2 == 0 ? 4U : 0U) |
                (((left ^ compare ^ result) & 0x10U) ? 0x10U : 0U) |
                (result == 0 ? 0x40U : 0U) | (result & 0x80U) |
                (((left ^ compare) & (left ^ result) & 0x80U) ? 0x800U : 0U);
            expect(state->route == (left == compare ? 1U : 2U),
                   "eligible route is ordinary; rejected and unexpected R12 "
                   "replay native branch");
            expect(state->calls == 1 &&
                       state->argument ==
                           reinterpret_cast<std::uintptr_t>(state->pawn.data()),
                   "predicate executes once with the original RDI pawn");
            expect(state->callback_rsp % 16 == 8 &&
                       state->before.registers[4] % 16 == 0,
                   "callback and midpoint stack alignment preserved");
            expect(state->before.registers == state->after.registers,
                   "all GPRs and native RSP preserved");
            expect(state->before.vectors == state->after.vectors,
                   "all sixteen XMM registers preserved");
            expect((state->after.flags & 0x8d5U) == arithmetic &&
                       (state->after.flags & ~0x8d5ULL) ==
                           (state->before.flags & ~0x8d5ULL),
                   "native CMP arithmetic flags and unrelated RFLAGS correct");
            expect(state->before.mxcsr == state->after.mxcsr &&
                       state->before.control == state->after.control,
                   "floating-point control state preserved");
            expect(state->pawn[0x848] == castle,
                   "CastleEdit field is never modified");
            for (const auto byte : state->native_stack)
              expect(byte == 0xa5,
                     "native stack untouched by predicate shadow space");
          }
        }
        expect(VirtualFree(memory, 0, MEM_RELEASE) != 0,
               "synthetic allocation released");
      }
    }
  }
  std::printf("road rule bridge: %u checks, %u failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
