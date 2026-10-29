#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

#include "FenceLifecycle.hpp"
#ifdef _WIN32
#include "FenceLifecycleHooks.hpp"
#endif

static std::size_t checks{}, intersect_calls{}, slice_calls{}, clear_calls{},
    reference_calls{}, write_calls{};
static std::int32_t intersection_count{2}, sliced_corner_count{4};
static bool game_thread{true};
static std::uintptr_t denied_address{}, expected_owner{}, expected_component{},
    expected_grid{};
static std::array<const void *, 8> received{};
static std::array<std::uint8_t, 2> received_flags{};
static std::vector<std::uintptr_t> cleared_owners;

static void check(bool condition, const char *message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL after %zu checks: %s\n", checks, message);
    std::exit(1);
  }
}

static auto test_read(std::uintptr_t address, void *output,
                      std::size_t size) noexcept -> bool {
  if (address < 4096 || address == denied_address)
    return false;
  if (address == 0x147f52738 && size == sizeof(std::uintptr_t)) {
    const std::uintptr_t native_clear = 0x143780000;
    std::memcpy(output, &native_clear, size);
    return true;
  }
  std::memcpy(output, reinterpret_cast<const void *>(address), size);
  return true;
}

static auto test_write(std::uintptr_t address, const void *input,
                       std::size_t size) noexcept -> bool {
  if (address < 4096 || address == denied_address)
    return false;
  ++write_calls;
  std::memcpy(reinterpret_cast<void *>(address), input, size);
  return true;
}

static auto test_game_thread() noexcept -> bool { return game_thread; }

static auto test_intersect(FenceLifecycleArray *output,
                           const FenceLifecycleArray *boundary,
                           const FenceLifecycleVec3 *line,
                           const FenceLifecycleVec3 *reference,
                           std::uint8_t prune, std::uint8_t extend)
    -> FenceLifecycleArray * {
  ++intersect_calls;
  received = {output,  boundary, line,    reference,
              nullptr, nullptr,  nullptr, nullptr};
  received_flags = {prune, extend};
  output->count = intersection_count;
  return output;
}

static void test_slice(const FenceLifecycleArray *boundary,
                       const FenceLifecycleArray *corners, const void *line,
                       FenceLifecycleArray *first, FenceLifecycleArray *second,
                       FenceLifecycleArray *third,
                       FenceLifecycleArray *output_corners,
                       const void *reference) {
  ++slice_calls;
  received = {boundary, corners, line,           first,
              second,   third,   output_corners, reference};
  output_corners->count = sliced_corner_count;
}

static void test_clear(void *component) {
  check(reinterpret_cast<std::uintptr_t>(component) == expected_component,
        "native ClearInstances receives only this owner's fence component");
  ++clear_calls;
}

static void test_remove(void *grid, void *owner) {
  check(clear_calls == reference_calls + 1,
        "native fence references are removed after instance clearing");
  check(reinterpret_cast<std::uintptr_t>(grid) == expected_grid &&
            reinterpret_cast<std::uintptr_t>(owner) == expected_owner,
        "native reference removal receives exact owner and grid");
  ++reference_calls;
  cleared_owners.push_back(reinterpret_cast<std::uintptr_t>(owner));
}

struct FenceFixture {
  std::vector<std::uint8_t> owner = std::vector<std::uint8_t>(0x2000);
  std::vector<std::uint8_t> component = std::vector<std::uint8_t>(0x1000);
  std::vector<std::uint8_t> master = std::vector<std::uint8_t>(0x1800);
  std::vector<std::uint8_t> grid = std::vector<std::uint8_t>(0x1000);
  std::vector<std::uint8_t> object_class = std::vector<std::uint8_t>(0x800);
  std::array<std::array<std::uint8_t, 1024>, 3> fence_storage{};
  std::array<FenceLifecycleVec3, 5> polygon{
      {{0, 0, 0}, {500, 0, 0}, {500, 500, 0}, {0, 500, 0}, {0, 0, 0}}};
  std::array<FenceLifecycleVec3, 2> line{{{0, 250, 20}, {500, 250, 40}}};
  FenceLifecycleVec3 reference{250, 250, 30};
  std::array<FenceLifecycleArray, 5> outputs{};
  std::array<FenceLifecycleVec3, 12> output_points{};

  template <class T>
  void put(std::vector<std::uint8_t> &buffer, std::size_t offset,
           const T &value) {
    check(offset + sizeof(T) <= buffer.size(), "fixture write remains bounded");
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
  }

  template <class T>
  auto get(const std::vector<std::uint8_t> &buffer, std::size_t offset) const
      -> T {
    T value{};
    std::memcpy(&value, buffer.data() + offset, sizeof(value));
    return value;
  }

  auto address() const -> std::uintptr_t {
    return reinterpret_cast<std::uintptr_t>(owner.data());
  }

  auto boundary() -> FenceLifecycleArray * {
    return reinterpret_cast<FenceLifecycleArray *>(owner.data() + 0x660);
  }

  auto corners() -> FenceLifecycleArray * {
    return reinterpret_cast<FenceLifecycleArray *>(owner.data() + 0x6a8);
  }

  void expect_native_owner() {
    expected_owner = address();
    expected_component = reinterpret_cast<std::uintptr_t>(component.data());
    expected_grid = reinterpret_cast<std::uintptr_t>(grid.data());
  }

  FenceFixture() {
    put(object_class, 0, std::uintptr_t{0x14712a508});
    std::uint32_t index = 100;
    for (auto *object : {&owner, &component, &master, &grid}) {
      put(*object, 0, std::uintptr_t{0x147f51b90});
      put(*object, 8, std::uint32_t{8});
      put(*object, 12, index++);
      put(*object, 16, reinterpret_cast<std::uintptr_t>(object_class.data()));
    }
    put(component, 0x20, address());
    put(owner, 0x2e8, reinterpret_cast<std::uintptr_t>(master.data()));
    put(master, 0x2c0, reinterpret_cast<std::uintptr_t>(grid.data()));
    put(owner, 0xf38, reinterpret_cast<std::uintptr_t>(component.data()));
    put(owner, 0x660,
        FenceLifecycleArray{reinterpret_cast<std::uintptr_t>(polygon.data()), 5,
                            5});
    put(owner, 0x6a8,
        FenceLifecycleArray{reinterpret_cast<std::uintptr_t>(polygon.data()), 5,
                            5});
    for (std::size_t i = 0; i < 3; ++i)
      put(owner, 0xf40 + i * 16,
          FenceLifecycleArray{
              reinterpret_cast<std::uintptr_t>(fence_storage[i].data()),
              static_cast<std::int32_t>(5 - i), 16});
    for (auto &output : outputs)
      output = {reinterpret_cast<std::uintptr_t>(output_points.data()), 0, 12};
    expect_native_owner();
  }

  auto query(std::size_t output = 0) -> FenceLifecycleArray * {
    return fence_lifecycle_observe_intersection(&outputs[output], boundary(),
                                                line.data(), &reference, 1, 1);
  }

  void slice() {
    fence_lifecycle_slice(boundary(), corners(), line.data(), &outputs[1],
                          &outputs[2], &outputs[3], &outputs[4], &reference);
  }

  void finish(std::size_t output = 0) {
    fence_lifecycle_finish(owner.data(), &outputs[output]);
  }

  void assert_only_counts_changed(const std::vector<std::uint8_t> &before) {
    auto expected = before;
    for (const auto offset : {0xf48, 0xf58, 0xf68}) {
      std::int32_t zero{};
      std::memcpy(expected.data() + offset, &zero, sizeof(zero));
    }
    check(owner == expected,
          "only three fence array counts change; geometry, house, cached "
          "fitting, pointers and capacities remain identical");
  }
};

static void reset() {
  intersect_calls = slice_calls = clear_calls = reference_calls = write_calls =
      0;
  received = {};
  received_flags = {};
  cleared_owners.clear();
  intersection_count = 2;
  sliced_corner_count = 4;
  denied_address = 0;
  game_thread = true;
  FenceLifecycleApi api{};
  api.image = 0x140000000;
  api.read_memory = test_read;
  api.write_memory = test_write;
  api.on_game_thread = test_game_thread;
  api.intersect = test_intersect;
  api.slice = test_slice;
  api.clear_instances = test_clear;
  api.remove_fence_refs = test_remove;
  configure_fence_lifecycle_api(api);
  set_fence_lifecycle_enabled(true);
}

static void native_pass_through() {
  reset();
  FenceFixture fixture;
  const auto before = fixture.owner;
  const auto outputs_before = fixture.outputs;
  auto *result = fixture.query();
  check(
      result == &fixture.outputs[0] && intersect_calls == 1 &&
          fixture.outputs[0].count == 2,
      "observer returns first native query result unchanged and exactly once");
  check(received[0] == &fixture.outputs[0] &&
            received[1] == fixture.boundary() &&
            received[2] == fixture.line.data() &&
            received[3] == &fixture.reference && received_flags[0] == 1 &&
            received_flags[1] == 1,
        "native query receives all original six arguments unchanged");
  check(fixture.outputs[0].data == outputs_before[0].data &&
            fixture.outputs[0].capacity == outputs_before[0].capacity,
        "observer does not replace native allocation or capacity");
  fixture.slice();
  check(slice_calls == 1 && received[0] == fixture.boundary() &&
            received[1] == fixture.corners() &&
            received[2] == fixture.line.data() &&
            received[3] == &fixture.outputs[1] &&
            received[4] == &fixture.outputs[2] &&
            received[5] == &fixture.outputs[3] &&
            received[6] == &fixture.outputs[4] &&
            received[7] == &fixture.reference,
        "native slice receives all original eight arguments exactly once");
  fixture.finish();
  check(fixture.owner == before && clear_calls == 0 && reference_calls == 0 &&
            write_calls == 0 && fence_lifecycle_cleanup_count() == 0,
        "successful native generation preserves every owner byte and valid "
        "fence");
}

static void failed_generation_parity() {
  for (bool cached_fitting : {false, true}) {
    reset();
    FenceFixture fixture;
    if (cached_fitting) {
      fixture.put(fixture.owner, 0x938,
                  FenceLifecycleVec3{12345, -67890, 8765});
      fixture.put(fixture.owner, 0x950,
                  FenceLifecycleVec3{23456, -78901, 9876});
    } else {
      fixture.put(fixture.owner, 0xf38, std::uintptr_t{});
      for (const auto offset : {0xf48, 0xf58, 0xf68})
        fixture.put(fixture.owner, offset, std::int32_t{});
    }
    const auto before = fixture.owner;
    intersection_count = 1;
    fixture.query();
    check(fixture.owner == before && clear_calls == 0,
          "observing failed native fitting changes no object immediately");
    // Cached fitting can let the later native fence slicing succeed. Fresh
    // load has no old fence; both must end with no fence for the same failure.
    sliced_corner_count = cached_fitting ? 4 : 0;
    fixture.slice();
    fixture.finish();
    for (const auto offset : {0xf48, 0xf58, 0xf68})
      check(fixture.get<std::int32_t>(fixture.owner, offset) == 0,
            "failed fitting yields no fence in either fresh-load or stale-live "
            "state");
    check(clear_calls == (cached_fitting ? 1 : 0) &&
              reference_calls == clear_calls,
          "native cleanup runs once only when an old fence exists");
    if (cached_fitting)
      fixture.assert_only_counts_changed(before);
    else
      check(
          fixture.owner == before,
          "fresh failed load remains exactly native, without creating objects");
    const auto after = fixture.owner;
    const auto clear_before = clear_calls;
    fixture.finish();
    check(fixture.owner == after && clear_calls == clear_before,
          "finish consumes its frame record, making repeated finish a no-op");
  }
  reset();
  FenceFixture fixture;
  const auto before = fixture.owner;
  intersection_count = 1;
  fixture.query();
  fixture.finish();
  fixture.assert_only_counts_changed(before);
  check(clear_calls == 1, "failure is finalized even when the native branch "
                          "skipped fence generation");
  // Same native stack address can be reused by an unrelated branch.
  for (const auto offset : {0xf48, 0xf58, 0xf68})
    fixture.put(fixture.owner, offset, std::int32_t{2});
  const auto repopulated = fixture.owner;
  fixture.finish();
  check(clear_calls == 1 && fixture.owner == repopulated,
        "reused frame without a new fitting query cannot remove a valid fence");
}

static void native_final_slice_failure() {
  for (std::int32_t corners = 0; corners < 4; ++corners) {
    reset();
    FenceFixture fixture;
    const auto before = fixture.owner;
    fixture.query();
    sliced_corner_count = corners;
    fixture.slice();
    fixture.finish();
    check(clear_calls == 1 && reference_calls == 1 && slice_calls == 1,
          "native final split failure clears stale fence exactly once");
    fixture.assert_only_counts_changed(before);
  }
  reset();
  FenceFixture fixture;
  sliced_corner_count = 0;
  fixture.slice();
  check(clear_calls == 1, "native split failure also cleans up when no "
                          "preceding fitting query ran");
}

static void frame_isolation() {
  reset();
  FenceFixture first, second;
  intersection_count = 1;
  first.query();
  second.query();
  second.finish();
  first.expect_native_owner();
  first.finish();
  check(cleared_owners.size() == 2 && cleared_owners[0] == second.address() &&
            cleared_owners[1] == first.address(),
        "nested owner frames consume only their own failure records");

  reset();
  FenceFixture fixture;
  intersection_count = 1;
  fixture.query(0);
  fixture.query(1);
  fixture.finish(1);
  for (const auto offset : {0xf48, 0xf58, 0xf68})
    fixture.put(fixture.owner, offset, std::int32_t{2});
  fixture.finish(0);
  check(clear_calls == 2,
        "same owner with different reentrant frame cookies remains isolated");

  reset();
  FenceFixture repeated;
  intersection_count = 1;
  repeated.query();
  intersection_count = 2;
  repeated.query();
  const auto before = repeated.owner;
  repeated.finish();
  check(clear_calls == 0 && repeated.owner == before,
        "successful native query supersedes failed state for the same frame");
  intersection_count = 1;
  repeated.query();
  repeated.finish(1);
  check(clear_calls == 0,
        "wrong frame cookie cannot consume or act on another frame failure");
  repeated.finish();
  check(clear_calls == 1,
        "correct frame still finalizes after unrelated finish");
}

static void validation_failures() {
  for (int invalid = 0; invalid < 13; ++invalid) {
    reset();
    FenceFixture fixture;
    intersection_count = 1;
    fixture.query();
    switch (invalid) {
    case 0:
      fixture.put(fixture.owner, 12, std::uint32_t{500});
      break;
    case 1:
      fixture.put(fixture.owner, 8, std::uint32_t{0x40000000});
      break;
    case 2:
      fixture.put(fixture.component, 8, std::uint32_t{0x8000});
      break;
    case 3:
      fixture.put(fixture.component, 0x20,
                  reinterpret_cast<std::uintptr_t>(fixture.master.data()));
      break;
    case 4:
      denied_address =
          reinterpret_cast<std::uintptr_t>(fixture.component.data());
      break;
    case 5:
      fixture.put(fixture.owner, 0xf48, std::int32_t{-1});
      break;
    case 6:
      fixture.put(fixture.owner, 0xf4c, std::int32_t{1});
      break;
    case 7:
      fixture.put(fixture.master, 0x2c0, std::uintptr_t{});
      break;
    case 8:
      game_thread = false;
      break;
    case 9:
      set_fence_lifecycle_enabled(false);
      break;
    case 10:
      fixture.put(fixture.owner, 0, std::uintptr_t{0x147f37800});
      break;
    case 11:
      fixture.put(fixture.object_class, 0, std::uintptr_t{});
      break;
    case 12:
      denied_address =
          reinterpret_cast<std::uintptr_t>(fixture.fence_storage[2].data());
      break;
    }
    const auto before = fixture.owner;
    fixture.finish();
    check(fixture.owner == before && clear_calls == 0 && reference_calls == 0 &&
              write_calls == 0,
          "stale identity, bad UObject/component/array/context cannot mutate "
          "owner");
  }
  reset();
  FenceFixture fixture;
  set_fence_lifecycle_enabled(false);
  intersection_count = 1;
  sliced_corner_count = 0;
  const auto before = fixture.owner;
  fixture.query();
  fixture.slice();
  fixture.finish();
  check(intersect_calls == 1 && slice_calls == 1 && fixture.owner == before &&
            clear_calls == 0,
        "disabled observer and split still execute native functions once");
}

static void saturated_frames() {
  reset();
  FenceFixture fixture;
  std::array<FenceLifecycleArray, 65> nested;
  intersection_count = 1;
  for (auto &output : nested) {
    output = fixture.outputs[0];
    fence_lifecycle_observe_intersection(&output, fixture.boundary(),
                                         fixture.line.data(),
                                         &fixture.reference, 1, 1);
  }
  const auto before = fixture.owner;
  fence_lifecycle_finish(fixture.owner.data(), &nested.back());
  check(
      fixture.owner == before && clear_calls == 0,
      "overflowing failure frame skips safely without evicting another record");
  for (std::size_t i = 0; i < 64; ++i) {
    for (const auto offset : {0xf48, 0xf58, 0xf68})
      fixture.put(fixture.owner, offset, std::int32_t{2});
    fence_lifecycle_finish(fixture.owner.data(), &nested[i]);
    check(clear_calls == i + 1, "every original pending failure retains its "
                                "own frame through saturation");
  }
  for (const auto offset : {0xf48, 0xf58, 0xf68})
    fixture.put(fixture.owner, offset, std::int32_t{2});
  fence_lifecycle_observe_intersection(&nested.back(), fixture.boundary(),
                                       fixture.line.data(), &fixture.reference,
                                       1, 1);
  fence_lifecycle_finish(fixture.owner.data(), &nested.back());
  check(clear_calls == 65,
        "new failure tracking recovers normally after nested frames unwind");
}

#ifdef _WIN32
static std::array<const void *, 2> finished_arguments;
static std::size_t finish_calls{};

static void record_finish(void *owner, const void *cookie) {
  finished_arguments = {owner, cookie};
  ++finish_calls;
}

static void finish_bridge_test() {
  const auto callback = reinterpret_cast<const void *>(record_finish);
  check(fence_lifecycle_make_finish_bridge(nullptr, callback).empty() &&
            fence_lifecycle_make_finish_bridge(callback, nullptr).empty(),
        "finish bridge rejects absent callback or continuation");
  auto *memory = static_cast<std::uint8_t *>(
      VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  check(memory != nullptr, "allocate private executable bridge-test page");
  // Adapt an ordinary Windows function call into the native aligned midpoint:
  // RSI=owner, RBP=frame base. No game function or instruction body executes.
  std::vector<std::uint8_t> entry{0x55, 0x56, 0x48, 0x83, 0xec, 0x28,
                                  0x48, 0x89, 0xce, 0x48, 0x89, 0xd5};
  const auto append32 = [](std::vector<std::uint8_t> &code,
                           std::uint32_t value) {
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
  // Capture host floating-point controls, then populate every volatile GPR
  // and XMM register. The hostile callback below deliberately destroys them.
  entry.insert(entry.end(), {0x0f, 0xae, 0x9d, 0xd0, 0x04, 0, 0, 0xd9, 0xbd,
                             0xd4, 0x04, 0, 0});
  const std::array<unsigned, 7> registers{0, 1, 2, 8, 9, 10, 11};
  for (const auto reg : registers)
    immediate(entry, reg, 0xabc0000000000000ULL + reg);
  for (unsigned reg = 0; reg < 6; ++reg) {
    entry.insert(entry.end(),
                 {0xf3, 0x0f, 0x6f, static_cast<std::uint8_t>(0x85 + reg * 8)});
    append32(entry, 0x500 + reg * 16);
  }
  entry.insert(entry.end(), {0xf9, 0x9c, 0x8f, 0x85, 0xe0, 0x04, 0, 0});
  append_absolute_jump(entry, memory + 0x200);
  const auto bridge =
      fence_lifecycle_make_finish_bridge(memory + 0x700, memory + 0x400);
  check(entry.size() < 0x200 && bridge.size() < 0x200,
        "entry and finish bridge fit separate private test ranges");
  std::memcpy(memory, entry.data(), entry.size());
  std::memcpy(memory + 0x200, bridge.data(), bridge.size());
  std::vector<std::uint8_t> continuation;
  for (std::size_t i = 0; i < registers.size(); ++i) {
    const auto reg = registers[i];
    continuation.insert(continuation.end(),
                        {static_cast<std::uint8_t>(reg >= 8 ? 0x4c : 0x48),
                         0x89,
                         static_cast<std::uint8_t>(0x85 + (reg % 8) * 8)});
    append32(continuation, 0x400 + static_cast<std::uint32_t>(i) * 8);
  }
  continuation.insert(continuation.end(), {0x9c, 0x8f, 0x85, 0x40, 0x04, 0, 0});
  for (unsigned reg = 0; reg < 6; ++reg) {
    continuation.insert(
        continuation.end(),
        {0xf3, 0x0f, 0x7f, static_cast<std::uint8_t>(0x85 + reg * 8)});
    append32(continuation, 0x600 + reg * 16);
  }
  continuation.insert(
      continuation.end(),
      {0x0f, 0xae, 0x9d, 0xc0, 0x04, 0, 0, 0xd9, 0xbd, 0xc4, 0x04, 0, 0,
       // Replay result is RCX=[RBP+190], return it.
       0x48, 0x89, 0xc8, 0x48, 0x83, 0xc4, 0x28, 0x5e, 0x5d, 0xc3});
  check(continuation.size() < 0x300, "continuation fits private test range");
  std::memcpy(memory + 0x400, continuation.data(), continuation.size());
  std::vector<std::uint8_t> hostile{0x48, 0x83, 0xec, 0x28};
  immediate(hostile, 0, reinterpret_cast<std::uintptr_t>(record_finish));
  hostile.insert(hostile.end(), {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x28});
  for (unsigned reg = 0; reg < 6; ++reg)
    hostile.insert(hostile.end(), {0x66, 0x0f, 0xef,
                                   static_cast<std::uint8_t>(0xc0 + reg * 9)});
  immediate(hostile, 0, reinterpret_cast<std::uintptr_t>(memory + 0xc00));
  hostile.insert(hostile.end(), {0x0f, 0xae, 0x10, 0xd9, 0x68, 0x04});
  for (const auto reg : registers)
    immediate(hostile, reg, 0xdef0000000000000ULL + reg);
  hostile.insert(hostile.end(), {0xf8, 0xc3});
  check(hostile.size() < 0x500, "hostile callback fits private test range");
  std::memcpy(memory + 0x700, hostile.data(), hostile.size());
  const std::uint32_t clobbered_mxcsr = 0x5f80;
  const std::uint16_t clobbered_x87 = 0x077f;
  std::memcpy(memory + 0xc00, &clobbered_mxcsr, sizeof(clobbered_mxcsr));
  std::memcpy(memory + 0xc04, &clobbered_x87, sizeof(clobbered_x87));
  DWORD previous{};
  check(VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &previous) != 0 &&
            FlushInstructionCache(GetCurrentProcess(), memory, 4096) != 0,
        "private bridge page is executable and cache-flushed");
  std::array<std::uint8_t, 0x700> frame{};
  for (std::size_t i = 0; i < 6 * 16; ++i)
    frame[0x500 + i] = static_cast<std::uint8_t>(i * 7 + 3);
  const std::uintptr_t expected_return = 0x123456789abcdef0;
  std::memcpy(frame.data() + 0x180, &expected_return, sizeof(expected_return));
  std::uint64_t owner{};
  finish_calls = 0;
  const auto result = reinterpret_cast<std::uintptr_t (*)(void *, void *)>(
      memory)(&owner, frame.data());
  check(finish_calls == 1 && finished_arguments[0] == &owner &&
            finished_arguments[1] == frame.data() + 0x340,
        "finish midpoint forwards owner RSI and exact RBP+308 cookie once");
  check(result == expected_return,
        "finish midpoint replays original overwritten native RCX load");
  for (std::size_t i = 0; i < registers.size(); ++i) {
    std::uintptr_t observed{};
    std::memcpy(&observed, frame.data() + 0x400 + i * 8, sizeof(observed));
    check(observed == (registers[i] == 1
                           ? expected_return
                           : 0xabc0000000000000ULL + registers[i]),
          "finish bridge preserves all volatile GPRs except native RCX replay");
  }
  check(std::memcmp(frame.data() + 0x500, frame.data() + 0x600, 6 * 16) == 0,
        "finish bridge restores XMM0 through XMM5 after hostile callback");
  check(std::memcmp(frame.data() + 0x4d0, frame.data() + 0x4c0, 6) == 0,
        "finish bridge restores MXCSR and x87 control word");
  check(std::memcmp(frame.data() + 0x4e0, frame.data() + 0x440, 8) == 0,
        "finish bridge restores all original flags including carry");
  check(VirtualFree(memory, 0, MEM_RELEASE) != 0,
        "private executable bridge-test page released");
}

static void profile_tests(const char *path) {
  std::ifstream stream(path, std::ios::binary);
  const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(stream),
                                       {}};
  check(file.size() >= sizeof(IMAGE_DOS_HEADER),
        "game binary is opened read-only for detached hook validation");
  IMAGE_DOS_HEADER dos{};
  std::memcpy(&dos, file.data(), sizeof(dos));
  check(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= 0 &&
            static_cast<std::size_t>(dos.e_lfanew) +
                    sizeof(IMAGE_NT_HEADERS64) <=
                file.size(),
        "supported binary has bounded DOS and PE headers");
  IMAGE_NT_HEADERS64 nt{};
  std::memcpy(&nt, file.data() + dos.e_lfanew, sizeof(nt));
  check(nt.Signature == IMAGE_NT_SIGNATURE &&
            nt.OptionalHeader.SizeOfImage == 0x09dfd000 &&
            nt.OptionalHeader.SizeOfHeaders <= file.size(),
        "supported image size and PE identity validated");
  std::vector<std::uint8_t> image(nt.OptionalHeader.SizeOfImage);
  std::memcpy(image.data(), file.data(), nt.OptionalHeader.SizeOfHeaders);
  const auto sections = static_cast<std::size_t>(dos.e_lfanew) +
                        sizeof(std::uint32_t) + sizeof(IMAGE_FILE_HEADER) +
                        nt.FileHeader.SizeOfOptionalHeader;
  for (unsigned i{}; i < nt.FileHeader.NumberOfSections; ++i) {
    IMAGE_SECTION_HEADER section{};
    check(sections + (i + 1) * sizeof(section) <= file.size(),
          "section header stays in binary");
    std::memcpy(&section, file.data() + sections + i * sizeof(section),
                sizeof(section));
    check(static_cast<std::size_t>(section.PointerToRawData) +
                      section.SizeOfRawData <=
                  file.size() &&
              static_cast<std::size_t>(section.VirtualAddress) +
                      section.SizeOfRawData <=
                  image.size(),
          "section data stays inside source file and detached image");
    std::memcpy(image.data() + section.VirtualAddress,
                file.data() + section.PointerToRawData, section.SizeOfRawData);
  }
  check(fence_lifecycle_validate_image(image.data()),
        "all lifecycle fingerprints accept actual installed game binary");
  check(!fence_lifecycle_validate_image(nullptr) &&
            !fence_lifecycle_validate_image(
                reinterpret_cast<const std::uint8_t *>(1)),
        "null and unreadable image addresses fail closed");
  for (const auto &signature : fence_lifecycle_fingerprints()) {
    check(!signature.bytes.empty() && signature.bytes.size() <= 128,
          "native fingerprint length is bounded");
    for (std::size_t i = 0; i < signature.bytes.size(); ++i) {
      image[signature.rva + i] ^= 1;
      check(!fence_lifecycle_validate_image(image.data()),
            "every changed fingerprint byte rejects installation");
      image[signature.rva + i] ^= 1;
    }
  }
  for (const std::size_t offset :
       {std::size_t{0}, offsetof(IMAGE_DOS_HEADER, e_lfanew),
        static_cast<std::size_t>(dos.e_lfanew),
        static_cast<std::size_t>(dos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, FileHeader) +
            offsetof(IMAGE_FILE_HEADER, Machine),
        static_cast<std::size_t>(dos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, FileHeader) +
            offsetof(IMAGE_FILE_HEADER, TimeDateStamp),
        static_cast<std::size_t>(dos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            offsetof(IMAGE_OPTIONAL_HEADER64, Magic),
        static_cast<std::size_t>(dos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage),
        static_cast<std::size_t>(dos.e_lfanew) +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            offsetof(IMAGE_OPTIONAL_HEADER64, CheckSum)}) {
    image[offset] ^= 1;
    check(!fence_lifecycle_validate_image(image.data()),
          "changed executable identity is rejected");
    image[offset] ^= 1;
  }
  const auto original = image;
  const auto intersection = reinterpret_cast<const void *>(test_intersect);
  const auto slice = reinterpret_cast<const void *>(test_slice);
  const auto finish = reinterpret_cast<const void *>(record_finish);
  const std::array<std::pair<std::uintptr_t, std::size_t>, 3> sites{
      {{0x4d038e1, 5}, {0x4d1ecfe, 5}, {0x4d07433, 7}}};
  {
    FenceLifecycleHooks hooks;
    check(!hooks.install(image.data(), nullptr, slice, finish) &&
              !hooks.install(image.data(), intersection, nullptr, finish) &&
              !hooks.install(image.data(), intersection, slice, nullptr),
          "every absent callback prevents partial hook installation");
    check(hooks.install(image.data(), intersection, slice, finish) &&
              hooks.installed(),
          "all three lifecycle hooks install atomically");
    check(!hooks.install(image.data(), intersection, slice, finish),
          "double hook installation is rejected");
    auto expected = original;
    for (const auto &[rva, size] : sites) {
      check(image[rva] == (size == 5 ? 0xe8 : 0xe9),
            "query/split remain CALL and common tail becomes JMP");
      std::memcpy(expected.data() + rva, image.data() + rva, size);
    }
    check(image == expected,
          "only the three audited instruction ranges change in detached image");
    const auto target = [&](std::uintptr_t rva) {
      std::int32_t displacement{};
      std::memcpy(&displacement, image.data() + rva + 1, sizeof(displacement));
      return image.data() + rva + 5 + displacement;
    };
    reset();
    FenceFixture fixture;
    const auto *result = reinterpret_cast<decltype(&test_intersect)>(
        target(0x4d038e1))(&fixture.outputs[0], fixture.boundary(),
                           fixture.line.data(), &fixture.reference, 7, 9);
    check(result == &fixture.outputs[0] && intersect_calls == 1 &&
              received_flags[0] == 7 && received_flags[1] == 9 &&
              received[0] == &fixture.outputs[0] &&
              received[1] == fixture.boundary() &&
              received[2] == fixture.line.data() &&
              received[3] == &fixture.reference,
          "installed Windows thunk preserves register and fifth/sixth stack "
          "arguments");
    reinterpret_cast<decltype(&test_slice)>(target(0x4d1ecfe))(
        fixture.boundary(), fixture.corners(), fixture.line.data(),
        &fixture.outputs[1], &fixture.outputs[2], &fixture.outputs[3],
        &fixture.outputs[4], &fixture.reference);
    check(slice_calls == 1 && received[0] == fixture.boundary() &&
              received[1] == fixture.corners() &&
              received[2] == fixture.line.data() &&
              received[3] == &fixture.outputs[1] &&
              received[4] == &fixture.outputs[2] &&
              received[5] == &fixture.outputs[3] &&
              received[6] == &fixture.outputs[4] &&
              received[7] == &fixture.reference,
          "installed Windows thunk preserves all eight slice arguments exactly "
          "once");
    const auto patched_byte = image[0x4d1ecfe];
    image[0x4d1ecfe] = 0xcc;
    check(!hooks.restore() && image[0x4d1ecfe] == 0xcc && hooks.installed(),
          "uninstall never overwrites a later third-party patch");
    image[0x4d1ecfe] = patched_byte;
    check(
        hooks.restore() && !hooks.installed() && image == original,
        "owned patches and remaining stubs restore after conflict is resolved");
  }
  {
    FenceLifecycleHooks hooks;
    check(hooks.install(image.data(), intersection, slice, finish),
          "restored image supports reinstall");
  }
  check(image == original, "normal destruction restores all original bytes");
}
#endif

int main(int argc, char **argv) {
  native_pass_through();
  failed_generation_parity();
  native_final_slice_failure();
  frame_isolation();
  validation_failures();
  saturated_frames();
  configure_fence_lifecycle_api({});
  set_fence_lifecycle_enabled(false);
#ifdef _WIN32
  finish_bridge_test();
  if (argc > 1)
    profile_tests(argv[1]);
#else
  (void)argc;
  (void)argv;
#endif
  std::printf("Fence lifecycle runtime tests passed: %zu checks\n", checks);
  return 0;
}
