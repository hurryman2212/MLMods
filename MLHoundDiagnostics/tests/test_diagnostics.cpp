#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <intrin.h>

#include "Diagnostics.hpp"
#include "NativePatch.hpp"

static std::uintptr_t captured_unit{};
static const void *captured_task{};
static std::uint64_t native_calls{};
static bool stack_aligned{};

__declspec(noinline) static void queued_target(std::uintptr_t unit,
                                               const void *task) {
  captured_unit = unit;
  captured_task = task;
  ++native_calls;
  stack_aligned =
      reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress()) % 16 == 8;
}

__declspec(noinline) static void finish_target(std::uintptr_t unit) {
  captured_unit = unit;
  ++native_calls;
}

__declspec(noinline) static bool validation_target(std::uintptr_t unit,
                                                   const void *camp) {
  captured_unit = unit;
  captured_task = camp;
  ++native_calls;
  return unit == reinterpret_cast<std::uintptr_t>(camp);
}

int main() {
  int checks{};
  const auto check = [&checks](bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
    ++checks;
  };
  try {
    HoundHistory history;
    auto result = decide_log(history, DiagnosticEvent::State, 10, 0);
    check(result.emit && result.suppressed == 0 && result.bonuses == 0,
          "first state at time zero");
    result = decide_log(history, DiagnosticEvent::State, 10, 1);
    check(!result.emit && result.suppressed == 1,
          "identical state is coalesced");
    result = decide_log(history, DiagnosticEvent::State, 10, 59999);
    check(!result.emit && result.suppressed == 2,
          "state does not emit before the heartbeat boundary");
    result = decide_log(history, DiagnosticEvent::State, 10, 60000);
    check(result.emit && result.suppressed == 2,
          "heartbeat reports the suppressed count");
    result = decide_log(history, DiagnosticEvent::State, 10, 60001);
    check(!result.emit && result.suppressed == 1,
          "heartbeat clears prior suppression count");
    result = decide_log(history, DiagnosticEvent::State, 11, 60002);
    check(result.emit && result.suppressed == 1,
          "changed state emits immediately, preserving suppression summary");
    result = decide_log(history, DiagnosticEvent::FollowQueued, 20, 60002);
    check(result.emit, "follow queue is independent of the status timer");
    result = decide_log(history, DiagnosticEvent::FollowStopped, 20, 60002);
    check(result.emit, "follow stop is not hidden by an immediate start");
    result = decide_log(history, DiagnosticEvent::FollowQueued, 20, 60003);
    check(!result.emit && result.suppressed == 1,
          "identical follow queue events coalesce");
    result = decide_log(history, DiagnosticEvent::FollowQueued, 20, 65001);
    check(!result.emit && result.suppressed == 2,
          "follow queue timer boundary excludes 4999 ms");
    result = decide_log(history, DiagnosticEvent::FollowQueued, 20, 65002);
    check(result.emit && result.suppressed == 2,
          "follow queue timer includes 5000 ms");
    result = decide_log(history, DiagnosticEvent::FollowQueued, 21, 65003);
    check(result.emit, "a different owner or task signature emits immediately");
    result = decide_log(history, DiagnosticEvent::FollowStopped, 20, 60003);
    check(!result.emit && result.suppressed == 1,
          "stop history has its own suppression count");
    result = decide_log(history, DiagnosticEvent::FollowStopped, 21, 60004);
    check(result.emit && result.suppressed == 1,
          "changed stop observations are not rate limited");

    for (std::uint64_t number = 1; number <= 100; ++number) {
      result = decide_log(history, DiagnosticEvent::BonusCommitted, 99, 65003);
      check(result.emit && result.suppressed == 0 && result.bonuses == number,
            "every actual native reward is recorded, even with identical data");
    }
    result = decide_log(history, DiagnosticEvent::State, 11, 65004);
    check(!result.emit && result.bonuses == 100,
          "nonreward observations do not change the reward count");
    HoundHistory another_hound;
    result = decide_log(another_hound, DiagnosticEvent::State, 11, 65004);
    check(result.emit && result.bonuses == 0,
          "hound histories do not leak across individuals");
    history = {};
    result = decide_log(history, DiagnosticEvent::State, 11, 65004);
    check(result.emit && result.bonuses == 0,
          "new world or changed object serial resets observational history");
    result = decide_log(history, DiagnosticEvent::State, 11, 1);
    check(result.emit && result.suppressed == 0,
          "a backwards diagnostic clock does not lock out future events");
    for (int iteration = 0; iteration < 100; ++iteration) {
      result = decide_log(history, DiagnosticEvent::State, 11, 1);
      check(!result.emit && result.bonuses == 0,
            "paused repeated observations produce no bonus or warning event");
    }
    result = decide_log(history, DiagnosticEvent::State, 11, 60001);
    check(result.emit && result.suppressed == 100 && result.bonuses == 0,
          "a neutral heartbeat after repeated identical state is not a reward");

    auto *page = static_cast<std::uint8_t *>(VirtualAlloc(
        nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    check(page != nullptr, "allocate isolated executable test page");
    std::vector<std::uint8_t> queued_stub;
    append_absolute_jump(queued_stub,
                         reinterpret_cast<const void *>(&queued_target));
    check(queued_stub.size() == 14 && queued_stub[0] == 0xff &&
              queued_stub[1] == 0x25,
          "tail stub is a RIP-relative indirect jump without register moves");
    std::memcpy(page, queued_stub.data(), queued_stub.size());
    std::vector<std::uint8_t> finish_stub;
    append_absolute_jump(finish_stub,
                         reinterpret_cast<const void *>(&finish_target));
    std::memcpy(page + 0x40, finish_stub.data(), finish_stub.size());
    std::vector<std::uint8_t> validation_stub;
    append_absolute_jump(validation_stub,
                         reinterpret_cast<const void *>(&validation_target));
    std::memcpy(page + 0x80, validation_stub.data(), validation_stub.size());
    DWORD previous{};
    check(VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &previous) != 0 &&
              FlushInstructionCache(GetCurrentProcess(), page, 0x1000) != 0,
          "make isolated stubs executable");
    const std::array<std::uint8_t, 0xd8> task{41};
    const auto task_before = task;
    using Queue = void (*)(std::uintptr_t, const void *);
    native_calls = 0;
    reinterpret_cast<Queue>(page)(0x12345678, task.data());
    check(native_calls == 1 && captured_unit == 0x12345678 &&
              captured_task == task.data() && task == task_before,
          "queued-task tail stub preserves both arguments and borrowed data");
    check(stack_aligned, "tail stub preserves Windows x64 stack alignment");
    using Finish = void (*)(std::uintptr_t);
    reinterpret_cast<Finish>(page + 0x40)(0x76543210);
    check(native_calls == 2 && captured_unit == 0x76543210,
          "finish-task tail stub calls its target exactly once");
    using Validate = bool (*)(std::uintptr_t, const void *);
    check(reinterpret_cast<Validate>(page + 0x80)(
              0x1234, reinterpret_cast<void *>(0x1234)),
          "validation tail stub preserves a true return");
    check(!reinterpret_cast<Validate>(page + 0x80)(
              0x1234, reinterpret_cast<void *>(0x5678)),
          "validation tail stub preserves a false return");
    check(native_calls == 4 && captured_unit == 0x1234 &&
              captured_task == reinterpret_cast<void *>(0x5678),
          "both validation calls pass their original arguments exactly once");
    check(VirtualFree(page, 0, MEM_RELEASE) != 0,
          "release isolated executable test page");
    std::printf("Diagnostics checks passed: %d\n", checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Diagnostics check failed: %s\n", error.what());
    return 1;
  }
}
