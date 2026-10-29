#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "CityHooks.hpp"

namespace {
struct BridgeSnapshot {
  std::array<std::uint64_t, 16> registers{};
  std::array<std::array<std::uint8_t, 16>, 16> vectors{};
  std::uint64_t flags{};
};

struct BridgeState {
  BridgeSnapshot before{};
  BridgeSnapshot after{};
  std::array<std::uint8_t, 0x80> native_stack{};
  void *callback_building{};
  void *callback_flag{};
};

struct SuppliesBridgeState {
  void *game{};
  int type{};
  const CityVector *position{};
  const CityVector *rotation{};
  void *building{};
  void *result{};
  int calls{};
};

SuppliesBridgeState supplies_bridge_state{};

auto supplies_bridge_callback(void *game, int type, const CityVector *position,
                              const CityVector *rotation, void *building)
    -> void * {
  supplies_bridge_state.game = game;
  supplies_bridge_state.type = type;
  supplies_bridge_state.position = position;
  supplies_bridge_state.rotation = rotation;
  supplies_bridge_state.building = building;
  ++supplies_bridge_state.calls;
  return supplies_bridge_state.result;
}
} // namespace

auto run_bridge_tests() -> int {
  int failures{};
  int checks{};
  const auto check = [&](bool success, const char *message) {
    ++checks;
    if (!success) {
      ++failures;
      std::fprintf(stderr, "bridge: %s\n", message);
    }
  };
  auto *memory = static_cast<std::uint8_t *>(
      VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (memory == nullptr)
    return 1;
  auto *state = new (memory + 0x1000) BridgeState{};
  auto *patterns = memory + 0x1400;
  for (std::size_t i{}; i < 16 * 16; ++i)
    patterns[i] = static_cast<std::uint8_t>(i * 7 + 3);
  alignas(16) std::array<std::uint8_t, 0x300> building{};
  constexpr std::uintptr_t master = 0x123456789abcdef0;
  std::memcpy(building.data() + 0x2e8, &master, sizeof(master));

  const auto append32 = [](std::vector<std::uint8_t> &code,
                           std::int32_t value) {
    const auto *raw = reinterpret_cast<const std::uint8_t *>(&value);
    code.insert(code.end(), raw, raw + sizeof(value));
  };
  const auto immediate = [](std::vector<std::uint8_t> &code, unsigned reg,
                            std::uintptr_t value) {
    code.push_back(reg >= 8 ? 0x49 : 0x48);
    code.push_back(static_cast<std::uint8_t>(0xb8 + reg % 8));
    const auto *raw = reinterpret_cast<const std::uint8_t *>(&value);
    code.insert(code.end(), raw, raw + sizeof(value));
  };
  const auto relative = [&](std::vector<std::uint8_t> &code, std::size_t origin,
                            const void *destination) {
    const auto delta = city_relative_displacement(
        reinterpret_cast<std::uintptr_t>(memory + origin + code.size() + 4),
        reinterpret_cast<std::uintptr_t>(destination));
    check(delta.has_value(), "synthetic RIP-relative operand fits");
    append32(code, delta.value_or(0));
  };
  const auto vector_stack = [&](std::vector<std::uint8_t> &code, unsigned reg,
                                bool load) {
    code.push_back(0xf3);
    if (reg >= 8)
      code.push_back(0x44);
    code.insert(code.end(),
                {0x0f, static_cast<std::uint8_t>(load ? 0x6f : 0x7f),
                 static_cast<std::uint8_t>(0x84 + (reg % 8) * 8), 0x24});
    append32(code, static_cast<std::int32_t>(0x80 + (reg - 6) * 16));
  };
  const auto snapshot = [&](std::vector<std::uint8_t> &code, std::size_t origin,
                            BridgeSnapshot *destination) {
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
    code.insert(code.end(), {0x9c, 0x8f, 0x05}); // PUSHFQ; POP [RIP+disp32].
    relative(code, origin, &destination->flags);
  };

  // Preserve our C++ caller's nonvolatile state, then recreate the native
  // midpoint: RSP aligned0, R12=Manor and a live byte at originalRSP+0x61.
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
    if (reg != 4)
      immediate(entry, reg,
                reg == 12 ? reinterpret_cast<std::uintptr_t>(building.data())
                          : 0xabc0000000000000ULL + reg);
  }
  for (unsigned reg{}; reg < 16; ++reg) {
    entry.push_back(0xf3);
    if (reg >= 8)
      entry.push_back(0x44);
    entry.insert(entry.end(),
                 {0x0f, 0x6f, static_cast<std::uint8_t>(0x05 + (reg % 8) * 8)});
    relative(entry, 0, patterns + reg * 16);
  }
  entry.insert(entry.end(),
               {0x39, 0xc8}); // CMP EAX,ECX: callback changes flags.
  snapshot(entry, 0, &state->before);
  append_city_jump(entry, memory + 0x800);

  // A deliberately hostile but ABI-compliant leaf callback: changes the one
  // requested byte, records both arguments, then clobbers all volatile GPRs
  // and XMM0..5. Nonvolatile XMM6..15 are preserved by the callback contract.
  std::vector<std::uint8_t> callback{0x48, 0x89, 0x0d};
  relative(callback, 0xa00, &state->callback_building);
  callback.insert(callback.end(), {0x48, 0x89, 0x15});
  relative(callback, 0xa00, &state->callback_flag);
  callback.insert(callback.end(), {0xc6, 0x02, 0x01, 0x31, 0xc0});
  for (unsigned reg{}; reg < 6; ++reg)
    callback.insert(
        callback.end(),
        {0x66, 0x0f, 0xef, static_cast<std::uint8_t>(0xc0 + reg * 9)});
  for (const unsigned reg : {0U, 1U, 2U, 8U, 9U, 10U, 11U})
    immediate(callback, reg, 0xdef0000000000000ULL + reg);
  callback.push_back(0xc3);
  const auto manor = city_manor_bridge(memory + 0xa00, memory + 0xc00);

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
  resume.insert(resume.end(), {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c,
                               0x5f, 0x5e, 0x5d, 0x5b, 0x31, 0xc0, 0xc3});

  const auto supplies = city_supplies_bridge(
      reinterpret_cast<const void *>(&supplies_bridge_callback));
  // Preserve the calling C++ function's RDI; the native callsite carries the
  // currently demolished building in RDI while RCX/RDX/R8/R9 are the 4
  // arguments.
  std::vector<std::uint8_t> supplies_entry{0x57, 0x48, 0x83, 0xec, 0x20};
  immediate(supplies_entry, 7,
            reinterpret_cast<std::uintptr_t>(building.data()));
  immediate(supplies_entry, 0,
            reinterpret_cast<std::uintptr_t>(memory + 0xb00));
  supplies_entry.insert(supplies_entry.end(),
                        {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3});
  const bool layout = entry.size() < 0x800 && manor.size() < 0x200 &&
                      callback.size() < 0x100 && supplies.size() < 0x100 &&
                      resume.size() < 0x280 && supplies_entry.size() < 0x180;
  check(layout, "synthetic code regions do not overlap");
  if (!layout) {
    VirtualFree(memory, 0, MEM_RELEASE);
    return failures;
  }
  std::memcpy(memory, entry.data(), entry.size());
  std::memcpy(memory + 0x800, manor.data(), manor.size());
  std::memcpy(memory + 0xa00, callback.data(), callback.size());
  std::memcpy(memory + 0xb00, supplies.data(), supplies.size());
  std::memcpy(memory + 0xc00, resume.data(), resume.size());
  std::memcpy(memory + 0xe80, supplies_entry.data(), supplies_entry.size());
  DWORD old{};
  const bool executable =
      VirtualProtect(memory, 0x1000, PAGE_EXECUTE_READ, &old) != 0 &&
      FlushInstructionCache(GetCurrentProcess(), memory, 0x1000) != 0;
  check(executable, "bridge code page RX and flushed");
  if (!executable) {
    VirtualFree(memory, 0, MEM_RELEASE);
    return failures;
  }
  check(reinterpret_cast<int (*)()>(memory)() == 0,
        "Manor bridge resumes and returns");
  check(state->before.registers[4] % 16 == 0,
        "native midpoint stack alignment recreated");
  check(state->callback_building == building.data() &&
            reinterpret_cast<std::uintptr_t>(state->callback_flag) ==
                state->before.registers[4] + 0x61,
        "callback receives exact R12 and original RSP+0x61");
  check(state->after.registers[0] == master,
        "displaced load sets RAX to building+0x2e8");
  for (unsigned reg = 1; reg < 16; ++reg)
    check(state->before.registers[reg] == state->after.registers[reg],
          "all other volatile/nonvolatile GPRs and RSP preserved");
  for (unsigned reg{}; reg < 16; ++reg)
    check(state->before.vectors[reg] == state->after.vectors[reg],
          "full XMM0..15 register state preserved");
  check(state->before.flags == state->after.flags,
        "all saved RFLAGS preserved");
  for (unsigned offset{}; offset < state->native_stack.size(); ++offset)
    check(
        state->native_stack[offset] == (offset == 0x61 ? 1 : 0xa5),
        "only native instaBuild byte changed, all adjacent stack bytes intact");

  CityVector position{1, 2, 3}, rotation{4, 5, 6};
  void *game = state;
  const auto run_supplies =
      reinterpret_cast<void *(*)(void *, int, const CityVector *,
                                 const CityVector *)>(memory + 0xe80);
  supplies_bridge_state = {};
  supplies_bridge_state.result = building.data() + 0x20;
  check(run_supplies(game, 88, &position, &rotation) ==
            supplies_bridge_state.result,
        "supplies bridge preserves callback pointer result");
  check(supplies_bridge_state.calls == 1 &&
            supplies_bridge_state.game == game &&
            supplies_bridge_state.type == 88 &&
            supplies_bridge_state.position == &position &&
            supplies_bridge_state.rotation == &rotation &&
            supplies_bridge_state.building == building.data(),
        "supplies bridge forwards four original arguments and RDI as fifth");
  supplies_bridge_state.result = nullptr;
  check(run_supplies(game, -7, nullptr, &rotation) == nullptr &&
            supplies_bridge_state.type == -7 &&
            supplies_bridge_state.position == nullptr &&
            supplies_bridge_state.calls == 2,
        "null result and unusual original arguments forwarded unchanged");
  check(VirtualFree(memory, 0, MEM_RELEASE) != 0,
        "synthetic bridge allocation released");
  std::printf("bridge: %d checks, %d failures\n", checks, failures);
  return failures;
}
