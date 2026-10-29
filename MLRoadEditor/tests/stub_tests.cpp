#include <cstdio>

#include "Patches.hpp"

static auto record_callback(void *cache, std::uint32_t index) noexcept -> bool {
  return cache != nullptr && index == 42;
}

static auto graph_work_callback(void *work) noexcept -> std::uintptr_t {
  return reinterpret_cast<std::uintptr_t>(work) ^ 0x9876543210abcdefULL;
}

static auto snap_graph_arguments(void *cache, const double *start,
                                 const double *end, const void *node_a,
                                 const void *node_b) noexcept -> bool {
  return cache == reinterpret_cast<void *>(1) &&
         start == reinterpret_cast<const double *>(2) &&
         end == reinterpret_cast<const double *>(3) &&
         node_a == reinterpret_cast<const void *>(0x1122334455667000ULL) &&
         node_b == reinterpret_cast<const void *>(0x2233445566778000ULL);
}

static auto collision_arguments(void *a, void *b, void *c, void *d,
                                std::uint8_t flag, void *pawn) noexcept
    -> bool {
  return a == reinterpret_cast<void *>(1) && b == reinterpret_cast<void *>(2) &&
         c == reinterpret_cast<void *>(3) && d == reinterpret_cast<void *>(4) &&
         flag == 7 && pawn == reinterpret_cast<void *>(0x12345678);
}

static auto endpoint_arguments(void *a, void *b, float radius, void *d,
                               void *pawn) noexcept -> bool {
  return a == reinterpret_cast<void *>(1) && b == reinterpret_cast<void *>(2) &&
         radius == 17.25f && d == reinterpret_cast<void *>(4) &&
         pawn == reinterpret_cast<void *>(0x12345678);
}

static auto segment_arguments(void *a, void *b, void *c, float width,
                              void *ignore, std::uint8_t flag,
                              void *pawn) noexcept -> bool {
  return a == reinterpret_cast<void *>(1) && b == reinterpret_cast<void *>(2) &&
         c == reinterpret_cast<void *>(3) && width == 23.5f &&
         ignore == reinterpret_cast<void *>(5) && flag == 9 &&
         pawn == reinterpret_cast<void *>(0x12345678);
}

static auto castle_road_arguments(void *road, std::uint8_t preview, void *pawn,
                                  std::uintptr_t iterator,
                                  std::uintptr_t end) noexcept -> bool {
  return road == reinterpret_cast<void *>(0x10203040) && preview == 1 &&
         pawn == reinterpret_cast<void *>(0x12345678) &&
         iterator == 0x1122334455660000ULL && end == 0x11223344556600b8ULL;
}

auto run_stub_tests() -> int {
  int failed{};
  const auto check = [&](bool success, const char *description) {
    std::printf("%s %s\n", success ? "PASS" : "FAIL", description);
    failed += !success;
  };
  auto *code = static_cast<std::uint8_t *>(VirtualAlloc(
      nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!code) {
    return 1;
  }
  std::array<std::uint8_t, 0x200> cache{};
  const std::uint32_t value = 123;
  std::memcpy(cache.data() + 0xd8, &value, sizeof(value));
  std::memcpy(cache.data() + 0x118, &value, sizeof(value));
  // Mid-function guards are reached by JMP, not CALL: preserve the native
  // stack alignment (RSP mod 16 == 0) and the caller's nonvolatile registers.
  const std::array<std::uint8_t, 16> setup{0x41, 0x57, 0x56, 0x57, 0x49, 0x89,
                                           0xcf, 0x89, 0xd6, 0x41, 0x89, 0xd0,
                                           0x41, 0x89, 0xd1, 0x90};
  const std::array<std::uint8_t, 5> leave{0x5f, 0x5e, 0x41, 0x5f, 0xc3};
  const auto run_guard = reinterpret_cast<int (*)(void *, std::uint32_t)>(code);
  for (const std::uint8_t reg : {6, 8, 9}) {
    std::memset(code, 0xcc, 0x1000);
    std::memcpy(code, setup.data(), setup.size());
    code[setup.size()] = 0xe9;
    const std::int32_t jump = 0x100 - static_cast<int>(setup.size()) - 5;
    std::memcpy(code + setup.size() + 1, &jump, sizeof(jump));
    if (reg == 9) {
      code[0x40] = 0x89;
      code[0x41] = 0xc8; // MOV EAX,ECX: replayed instruction wrote ECX.
      std::memcpy(code + 0x42, leave.data(), leave.size());
    } else {
      std::memcpy(code + 0x40, leave.data(), leave.size());
    }
    code[0x60] = 0xb8;
    const std::int32_t skipped = 456;
    std::memcpy(code + 0x61, &skipped, sizeof(skipped));
    std::memcpy(code + 0x65, leave.data(), leave.size());
    const std::array<std::uint8_t, 7> displaced{
        0x41,
        0x8b,
        static_cast<std::uint8_t>(reg == 9 ? 0x8f : 0x87),
        static_cast<std::uint8_t>(reg == 9 ? 0x18 : 0xd8),
        static_cast<std::uint8_t>(reg == 9 ? 1 : 0),
        0,
        0};
    const auto stub =
        make_record_guard(reinterpret_cast<const void *>(&record_callback), reg,
                          displaced, code + 0x40, code + 0x60);
    std::memcpy(code + 0x100, stub.data(), stub.size());
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    check(run_guard(cache.data(), 41) == 123,
          "record guard preserves original path and displaced instruction");
    check(run_guard(cache.data(), 42) == 456,
          "record guard branches only for the selected actor index");
  }

  for (const int mode : {0, 1, 2}) {
    std::memset(code, 0xcc, 0x1000);
    std::vector<std::uint8_t> caller{0x57, 0x48, 0x83, 0xec, 0x30, 0x48,
                                     0xc7, 0xc7, 0x78, 0x56, 0x34, 0x12};
    std::vector<std::uint8_t> bridge;
    if (mode == 0) {
      caller.insert(caller.end(), {0xc6, 0x44, 0x24, 0x20, 7});
      bridge = make_collision_bridge(
          reinterpret_cast<const void *>(&collision_arguments));
    } else if (mode == 1) {
      bridge = make_road_bridge(
          reinterpret_cast<const void *>(&endpoint_arguments), false);
    } else {
      caller.insert(caller.end(), {0x48, 0xc7, 0x44, 0x24, 0x20, 5, 0, 0, 0,
                                   0xc6, 0x44, 0x24, 0x28, 9});
      bridge = make_road_bridge(
          reinterpret_cast<const void *>(&segment_arguments), true);
    }
    caller.push_back(0xe8);
    const auto relative = static_cast<std::int32_t>(0x100 - caller.size() - 4);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&relative);
    caller.insert(caller.end(), bytes, bytes + sizeof(relative));
    caller.insert(caller.end(), {0x48, 0x83, 0xc4, 0x30, 0x5f, 0xc3});
    std::memcpy(code, caller.data(), caller.size());
    std::memcpy(code + 0x100, bridge.data(), bridge.size());
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    if (mode == 0) {
      const auto run =
          reinterpret_cast<bool (*)(void *, void *, void *, void *)>(code);
      check(run(reinterpret_cast<void *>(1), reinterpret_cast<void *>(2),
                reinterpret_cast<void *>(3), reinterpret_cast<void *>(4)),
            "building bridge preserves five native arguments and appends pawn");
    } else if (mode == 1) {
      const auto run =
          reinterpret_cast<bool (*)(void *, void *, float, void *)>(code);
      check(run(reinterpret_cast<void *>(1), reinterpret_cast<void *>(2),
                17.25f, reinterpret_cast<void *>(4)),
            "endpoint bridge preserves float radius in XMM2");
    } else {
      const auto run =
          reinterpret_cast<bool (*)(void *, void *, void *, float)>(code);
      check(run(reinterpret_cast<void *>(1), reinterpret_cast<void *>(2),
                reinterpret_cast<void *>(3), 23.5f),
            "segment bridge preserves XMM3, both stack arguments and pawn");
    }
  }
  std::memset(code, 0xcc, 0x1000);
  // Model the exact native CALL ABI, including unrelated high bits in RDX.
  // Preserve the source registers and transfer full 64-bit iterator/end values.
  std::vector<std::uint8_t> castle_caller{
      0x57, 0x56, 0x41, 0x56, 0x48, 0x83, 0xec, 0x20, 0x48, 0xc7, 0xc7, 0x78,
      0x56, 0x34, 0x12, 0x48, 0xbe, 0,    0,    0x66, 0x55, 0x44, 0x33, 0x22,
      0x11, 0x49, 0xbe, 0xb8, 0,    0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xe8};
  const auto castle_relative =
      static_cast<std::int32_t>(0x100 - castle_caller.size() - 4);
  const auto *castle_bytes =
      reinterpret_cast<const std::uint8_t *>(&castle_relative);
  castle_caller.insert(castle_caller.end(), castle_bytes,
                       castle_bytes + sizeof(castle_relative));
  castle_caller.insert(castle_caller.end(),
                       {0x48, 0x81, 0xff, 0x78, 0x56, 0x34, 0x12, 0x0f, 0x85});
  std::array<std::size_t, 3> castle_checks{castle_caller.size()};
  castle_caller.insert(castle_caller.end(), {0, 0, 0, 0});
  for (const bool final_pointer : {false, true}) {
    const std::uint64_t value =
        final_pointer ? 0x11223344556600b8ULL : 0x1122334455660000ULL;
    castle_caller.insert(castle_caller.end(), {0x49, 0xba});
    const auto *value_bytes = reinterpret_cast<const std::uint8_t *>(&value);
    castle_caller.insert(castle_caller.end(), value_bytes, value_bytes + 8);
    castle_caller.insert(castle_caller.end(), {static_cast<std::uint8_t>(
                                                   final_pointer ? 0x4d : 0x4c),
                                               0x39, 0xd6, 0x0f, 0x85});
    castle_checks[final_pointer ? 2 : 1] = castle_caller.size();
    castle_caller.insert(castle_caller.end(), {0, 0, 0, 0});
  }
  castle_caller.insert(castle_caller.end(),
                       {0x48, 0x83, 0xc4, 0x20, 0x41, 0x5e, 0x5e, 0x5f, 0xc3});
  const auto castle_failure = castle_caller.size();
  castle_caller.insert(castle_caller.end(), {0x0f, 0x0b});
  for (const auto offset : castle_checks) {
    const auto distance =
        static_cast<std::int32_t>(castle_failure - offset - 4);
    std::memcpy(castle_caller.data() + offset, &distance, sizeof(distance));
  }
  const auto castle_bridge = make_castle_road_bridge(
      reinterpret_cast<const void *>(&castle_road_arguments));
  std::memcpy(code, castle_caller.data(), castle_caller.size());
  std::memcpy(code + 0x100, castle_bridge.data(), castle_bridge.size());
  FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
  const auto castle_call =
      reinterpret_cast<bool (*)(void *, std::uintptr_t)>(code);
  check(
      castle_call(reinterpret_cast<void *>(0x10203040), 0xabcdef1234567801ULL),
      "castle road bridge preserves native arguments and appends RDI pawn");
  check(
      !castle_call(reinterpret_cast<void *>(0x10203040), 0xabcdef1234567800ULL),
      "castle road bridge preserves false preview argument");
  check(
      !castle_call(reinterpret_cast<void *>(0x10203048), 0xabcdef1234567801ULL),
      "castle road bridge preserves exact native road pointer");

  // Exercise all four live-register layouts. The mock callback only compares
  // the full pointer values; these synthetic addresses are never dereferenced.
  for (const auto site : {0x4b8b01f, 0x4b8b1b5, 0x4b8b598, 0x4b8b714}) {
    std::memset(code, 0xcc, 0x1000);
    std::vector<std::uint8_t> caller{0x53, 0x55, 0x57, 0x41, 0x55, 0x41,
                                     0x56, 0x41, 0x57, 0x48, 0x81, 0xec,
                                     0xa8, 0,    0,    0};
    const auto immediate = [&](std::uint64_t value) {
      const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
      caller.insert(caller.end(), bytes, bytes + sizeof(value));
    };
    constexpr std::uint64_t first = 0x1122334455667000ULL;
    constexpr std::uint64_t second = 0x2233445566778000ULL;
    const std::array<std::uint64_t, 5> registers{
        second, site == 0x4b8b714 ? second + 0x1a : first - 0x18, first - 0x18,
        second - 0x18, first + 0x1a};
    for (std::size_t index = 0; index < registers.size(); ++index) {
      // RBX, RDI, R13, R14, R15.
      const std::array<std::array<std::uint8_t, 2>, 5> moves{{{0x48, 0xbb},
                                                              {0x48, 0xbf},
                                                              {0x49, 0xbd},
                                                              {0x49, 0xbe},
                                                              {0x49, 0xbf}}};
      caller.insert(caller.end(), moves[index].begin(), moves[index].end());
      immediate(registers[index]);
    }
    // RBP-0x68 points into the mock native frame, beyond its shadow space.
    caller.insert(caller.end(), {0x48, 0x8d, 0xac, 0x24, 0xa0, 0, 0, 0, 0x48,
                                 0x89, 0x5d, 0x98, 0xe8});
    const auto relative = static_cast<std::int32_t>(0x300 - caller.size() - 4);
    const auto *relative_bytes =
        reinterpret_cast<const std::uint8_t *>(&relative);
    caller.insert(caller.end(), relative_bytes,
                  relative_bytes + sizeof(relative));
    // Preserve the callback result while checking every source register.
    caller.insert(caller.end(), {0x41, 0x88, 0xc3});
    std::vector<std::size_t> failures;
    for (std::size_t index = 0; index < registers.size(); ++index) {
      caller.insert(caller.end(), {0x49, 0xba});
      immediate(registers[index]);
      const std::array<std::array<std::uint8_t, 3>, 5> comparisons{
          {{0x4c, 0x39, 0xd3},
           {0x4c, 0x39, 0xd7},
           {0x4d, 0x39, 0xd5},
           {0x4d, 0x39, 0xd6},
           {0x4d, 0x39, 0xd7}}};
      caller.insert(caller.end(), comparisons[index].begin(),
                    comparisons[index].end());
      caller.insert(caller.end(), {0x0f, 0x85});
      failures.push_back(caller.size());
      caller.insert(caller.end(), {0, 0, 0, 0});
    }
    caller.insert(caller.end(), {0x4c, 0x8d, 0x94, 0x24, 0xa0, 0, 0, 0, 0x4c,
                                 0x39, 0xd5, 0x0f, 0x85});
    failures.push_back(caller.size());
    caller.insert(caller.end(), {0, 0, 0, 0});
    caller.insert(caller.end(),
                  {0x44, 0x88, 0xd8, 0x48, 0x81, 0xc4, 0xa8, 0,    0,    0,
                   0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x5f, 0x5d, 0x5b, 0xc3});
    const auto failure = caller.size();
    caller.insert(caller.end(), {0x0f, 0x0b});
    for (const auto offset : failures) {
      const auto distance = static_cast<std::int32_t>(failure - offset - 4);
      std::memcpy(caller.data() + offset, &distance, sizeof(distance));
    }
    const auto bridge = make_snap_graph_bridge(
        reinterpret_cast<const void *>(&snap_graph_arguments), site);
    std::memcpy(code, caller.data(), caller.size());
    std::memcpy(code + 0x300, bridge.data(), bridge.size());
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    const auto run =
        reinterpret_cast<bool (*)(void *, const double *, const double *)>(
            code);
    check(
        run(reinterpret_cast<void *>(1), reinterpret_cast<const double *>(2),
            reinterpret_cast<const double *>(3)),
        "snap graph bridge preserves source registers and full endpoint nodes");
    check(!run(nullptr, reinterpret_cast<const double *>(2),
               reinterpret_cast<const double *>(3)),
          "snap graph bridge preserves false callback result");
  }
  check(make_snap_graph_bridge(
            reinterpret_cast<const void *>(&snap_graph_arguments), 0)
            .empty(),
        "unknown snap graph callsite is rejected");

  // The worker thunk adjusts RCX before its tail jump. No extra return frame
  // may be inserted, and all 64 return bits must reach the original caller.
  std::memset(code, 0xcc, 0x1000);
  const std::array<std::uint8_t, 9> graph_thunk{0x48, 0x83, 0xc1, 0x48, 0xe9,
                                                0x37, 0,    0,    0};
  std::memcpy(code, graph_thunk.data(), graph_thunk.size());
  std::vector<std::uint8_t> graph_stub;
  append_absolute_jump(graph_stub,
                       reinterpret_cast<const void *>(&graph_work_callback));
  std::memcpy(code + 0x40, graph_stub.data(), graph_stub.size());
  FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
  const auto graph_call = reinterpret_cast<std::uintptr_t (*)(void *)>(code);
  const auto graph_expected = graph_work_callback(cache.data() + 0x48);
  check(
      graph_call(cache.data()) == graph_expected,
      "original graph thunk adjusts work pointer and preserves 64-bit return");
  PatchSet graph_patch;
  const std::array<CodePatch, 1> graph_plan{
      {{4, {0xe9, 0x37, 0, 0, 0}, graph_stub, false}}};
  check(graph_patch.install(code, graph_plan),
        "graph tail-JMP patch installs in isolated fixture");
  check(graph_call(cache.data()) == graph_expected,
        "patched graph thunk preserves adjusted work pointer and full return");
  check(graph_patch.restore() && graph_call(cache.data()) == graph_expected,
        "graph thunk restores original behavior");
  VirtualFree(code, 0, MEM_RELEASE);
  return failed;
}
