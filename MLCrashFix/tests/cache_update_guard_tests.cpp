#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "CacheUpdateGuard.hpp"

static std::size_t checks{};

static void require(bool condition, const char *message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

struct FakeCache {
  alignas(8) std::array<std::byte, 0x2b0> native_fields{};
  std::atomic<bool> worker_busy{false};
  std::vector<int> additions;
  std::vector<int> removals;
  std::vector<int> live;
  std::vector<int> applied;
  std::vector<int> graph_ids;
  std::size_t busy_calls{};
  std::size_t drain_calls{};
  std::size_t count_calls{};

  void set_pending_counts(std::array<std::int32_t, 4> values) {
    const std::array<std::size_t, 4> offsets{0x230, 0x25c, 0x280, 0x2ac};
    for (std::size_t i = 0; i < offsets.size(); ++i)
      std::memcpy(native_fields.data() + offsets[i], &values[i],
                  sizeof(values[i]));
  }
};

static bool fake_busy(void *context) {
  auto &cache = *static_cast<FakeCache *>(context);
  ++cache.busy_calls;
  return cache.worker_busy.load(std::memory_order_acquire);
}

static void fake_drain(void *context) {
  auto &cache = *static_cast<FakeCache *>(context);
  ++cache.drain_calls;
  // This deliberately observable callback is a stand-in for the native drain.
  // The guard must leave its ordering, removal and ownership rules intact.
  for (int value : cache.additions) {
    cache.live.push_back(value);
    cache.applied.push_back(value);
  }
  for (int value : cache.removals) {
    std::erase(cache.live, value);
    cache.applied.push_back(-value);
  }
  cache.additions.clear();
  cache.removals.clear();
  cache.set_pending_counts({0, 0, 0, 0});
}

static int fake_graph_count(void *context) {
  auto &cache = *static_cast<FakeCache *>(context);
  ++cache.count_calls;
  return static_cast<int>(cache.graph_ids.size());
}

static void test_callback_configuration() {
  const auto before = cache_update_guard_counters();
  guarded_cache_update(nullptr);
  require(guarded_cache_graph_count(nullptr) == 0,
          "null cache has no consumable graph work");
  require(!configure_cache_update_guard_for_testing(
              {nullptr, fake_drain, fake_graph_count}) &&
              !configure_cache_update_guard_for_testing(
                  {fake_busy, nullptr, fake_graph_count}) &&
              !configure_cache_update_guard_for_testing(
                  {fake_busy, fake_drain, nullptr}) &&
              !configure_cache_update_guard_for_testing(
                  {nullptr, nullptr, nullptr}),
          "missing callbacks are rejected");
  const auto after = cache_update_guard_counters();
  require(before.deferred == after.deferred &&
              before.executed == after.executed &&
              before.graph_deferred == after.graph_deferred,
          "null cache and rejected configuration leave counters unchanged");
  FakeCache cache;
  cache.additions = {1};
  guarded_cache_update(&cache);
  require(cache.busy_calls == 1 && cache.drain_calls == 1 &&
              cache.live == std::vector<int>({1}),
          "rejected configuration preserves the previous complete callbacks");
}

static void test_graph_consumption_waits_for_pending_updates() {
  FakeCache cache;
  cache.worker_busy.store(true);
  cache.additions = {501, 502};
  cache.removals = {500};
  cache.live = {500};
  cache.graph_ids = {71, 32, 71};
  cache.set_pending_counts({3, 0, 2, 0});
  const auto pending_before = cache.native_fields;
  const auto before = cache_update_guard_counters();
  guarded_cache_update(&cache);
  require(
      guarded_cache_graph_count(&cache) == 0 && cache.count_calls == 0 &&
          cache.graph_ids == std::vector<int>({71, 32, 71}) &&
          cache.native_fields == pending_before,
      "deferred update suppresses graph consumption without discarding IDs");
  cache.worker_busy.store(false);
  require(guarded_cache_graph_count(&cache) == 0 && cache.count_calls == 0 &&
              cache.native_fields == pending_before,
          "worker finishing between checks cannot expose stale graph state");

  cache.set_pending_counts({4, 4, 8, 7});
  require(guarded_cache_graph_count(&cache) == 0 && cache.count_calls == 0,
          "pending removals alone defer graph consumption");
  cache.set_pending_counts({5, 4, 8, 8});
  require(guarded_cache_graph_count(&cache) == 0 && cache.count_calls == 0,
          "pending additions alone defer graph consumption");
  FakeCache ready;
  ready.set_pending_counts({7, 7, 3, 3});
  ready.graph_ids = {901};
  require(guarded_cache_graph_count(&ready) == 1 && ready.count_calls == 1 &&
              ready.graph_ids == std::vector<int>({901}),
          "unrelated cache with equal nonzero counters remains consumable");

  guarded_cache_update(&cache);
  require(
      guarded_cache_graph_count(&cache) == 3 && cache.count_calls == 1 &&
          cache.drain_calls == 1 && cache.additions.empty() &&
          cache.removals.empty() &&
          cache.live == std::vector<int>({501, 502}) &&
          cache.graph_ids == std::vector<int>({71, 32, 71}),
      "later native drain exposes original graph count with FIFO IDs intact");
  const auto after = cache_update_guard_counters();
  require(after.deferred == before.deferred + 1 &&
              after.executed == before.executed + 1 &&
              after.graph_deferred == before.graph_deferred + 4,
          "graph deferrals and update counters describe independent decisions");
}

static void test_deferred_queues_and_eventual_drain() {
  FakeCache cache;
  cache.worker_busy.store(true);
  cache.additions = {41, 12, 73};
  cache.removals = {8, 41};
  cache.live = {8, 9};
  const auto before = cache_update_guard_counters();

  guarded_cache_update(&cache);
  guarded_cache_update(&cache);
  require(cache.busy_calls == 2 && cache.drain_calls == 0,
          "busy cache probes once per attempt and never enters drain");
  require(cache.additions == std::vector<int>({41, 12, 73}) &&
              cache.removals == std::vector<int>({8, 41}) &&
              cache.live == std::vector<int>({8, 9}) && cache.applied.empty(),
          "deferral preserves both pending queues and live cache");
  auto counters = cache_update_guard_counters();
  require(counters.deferred == before.deferred + 2 &&
              counters.executed == before.executed,
          "only deferral counter changes while worker is busy");

  // New work joins the original queues; deferred entries retain their place.
  cache.additions.push_back(25);
  cache.removals.push_back(9);
  cache.worker_busy.store(false, std::memory_order_release);
  guarded_cache_update(&cache);
  require(cache.busy_calls == 3 && cache.drain_calls == 1,
          "first later idle attempt enters original drain exactly once");
  require(cache.applied == std::vector<int>({41, 12, 73, 25, -8, -41, -9}),
          "deferred additions and removals retain callback FIFO order");
  require(cache.additions.empty() && cache.removals.empty() &&
              cache.live == std::vector<int>({12, 73, 25}),
          "eventual drain applies every pending removal without lost work");
  counters = cache_update_guard_counters();
  require(counters.deferred == before.deferred + 2 &&
              counters.executed == before.executed + 1,
          "successful later drain increments only executed counter");

  guarded_cache_update(&cache);
  require(cache.busy_calls == 4 && cache.drain_calls == 2,
          "idle empty queues still call the original drain exactly once");
  require(cache.applied == std::vector<int>({41, 12, 73, 25, -8, -41, -9}) &&
              cache.live == std::vector<int>({12, 73, 25}),
          "subsequent empty drain does not replay deferred work");
}

static void test_independent_caches() {
  FakeCache busy;
  busy.worker_busy.store(true);
  busy.additions = {101, 102};
  busy.removals = {100};
  busy.live = {100};
  FakeCache idle;
  idle.additions = {201, 202};
  idle.removals = {200};
  idle.live = {200};
  const auto before = cache_update_guard_counters();

  guarded_cache_update(&busy);
  guarded_cache_update(&idle);
  guarded_cache_update(&busy);
  require(busy.drain_calls == 0 &&
              busy.additions == std::vector<int>({101, 102}) &&
              busy.removals == std::vector<int>({100}) &&
              busy.live == std::vector<int>({100}) && busy.applied.empty(),
          "one active worker preserves only its own cache's pending state");
  require(idle.drain_calls == 1 && idle.additions.empty() &&
              idle.removals.empty() &&
              idle.live == std::vector<int>({201, 202}) &&
              idle.applied == std::vector<int>({201, 202, -200}),
          "a busy cache cannot suppress an unrelated idle cache's drain");

  idle.worker_busy.store(true);
  idle.additions.push_back(203);
  busy.worker_busy.store(false);
  guarded_cache_update(&busy);
  guarded_cache_update(&idle);
  require(busy.drain_calls == 1 && busy.additions.empty() &&
              busy.removals.empty() &&
              busy.live == std::vector<int>({101, 102}) &&
              busy.applied == std::vector<int>({101, 102, -100}),
          "formerly busy cache drains when its own worker becomes idle");
  require(idle.drain_calls == 1 && idle.additions == std::vector<int>({203}) &&
              idle.live == std::vector<int>({201, 202}),
          "worker-state changes remain independent for each cache");
  const auto counters = cache_update_guard_counters();
  require(counters.deferred == before.deferred + 3 &&
              counters.executed == before.executed + 2,
          "aggregate counters match independently deferred and executed calls");
}

static void test_active_worker_does_not_block_update() {
  FakeCache cache;
  cache.additions = {301, 302};
  cache.removals = {300};
  cache.live = {300};
  std::atomic<bool> started{false};
  std::atomic<bool> release{false};
  std::atomic<bool> returned{false};
  std::jthread worker([&](std::stop_token stop) {
    cache.worker_busy.store(true, std::memory_order_release);
    started.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire) && !stop.stop_requested())
      std::this_thread::yield();
    cache.worker_busy.store(false, std::memory_order_release);
  });
  while (!started.load(std::memory_order_acquire))
    std::this_thread::yield();

  const auto before = cache_update_guard_counters();
  std::jthread updater([&] {
    guarded_cache_update(&cache);
    returned.store(true, std::memory_order_release);
  });
  // Give a loaded CI/Wine scheduler two seconds; no worker release is allowed
  // until the guard has returned or this diagnostic deadline has expired.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!returned.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool returned_before_release = returned.load(std::memory_order_acquire);
  const bool still_busy = cache.worker_busy.load(std::memory_order_acquire);
  release.store(true, std::memory_order_release);
  worker.join();
  updater.join();

  require(returned_before_release && still_busy,
          "guard returns while a real worker remains active without waiting");
  require(cache.busy_calls == 1 && cache.drain_calls == 0 &&
              cache.additions == std::vector<int>({301, 302}) &&
              cache.removals == std::vector<int>({300}) &&
              cache.live == std::vector<int>({300}) && cache.applied.empty(),
          "active-worker deferral leaves pending and live state untouched");
  guarded_cache_update(&cache);
  require(cache.drain_calls == 1 && cache.additions.empty() &&
              cache.removals.empty() &&
              cache.live == std::vector<int>({301, 302}),
          "released worker permits complete drain on next frame");
  const auto counters = cache_update_guard_counters();
  require(counters.deferred == before.deferred + 1 &&
              counters.executed == before.executed + 1,
          "real-worker test records one deferral followed by one execution");
}

static void test_offline_installation(const char *filename) {
  std::ifstream stream(filename, std::ios::binary | std::ios::ate);
  require(stream.good(), "open current game executable read only");
  const auto length = stream.tellg();
  require(length >= static_cast<std::streamoff>(sizeof(IMAGE_NT_HEADERS64)),
          "input is large enough to hold PE headers");
  std::vector<std::uint8_t> file(static_cast<std::size_t>(length));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(file.data()),
              static_cast<std::streamsize>(file.size()));
  require(stream.good(), "read complete executable");
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(file.data());
  require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
              static_cast<std::size_t>(dos->e_lfanew) <=
                  file.size() - sizeof(IMAGE_NT_HEADERS64),
          "valid PE header bounds");
  const auto *nt =
      reinterpret_cast<const IMAGE_NT_HEADERS64 *>(file.data() + dos->e_lfanew);
  require(nt->Signature == IMAGE_NT_SIGNATURE &&
              nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
              nt->OptionalHeader.SizeOfHeaders <= file.size() &&
              nt->OptionalHeader.SizeOfHeaders <=
                  nt->OptionalHeader.SizeOfImage,
          "valid PE mapping dimensions");
  const auto *sections = reinterpret_cast<const IMAGE_SECTION_HEADER *>(
      reinterpret_cast<const std::uint8_t *>(&nt->OptionalHeader) +
      nt->FileHeader.SizeOfOptionalHeader);
  require(reinterpret_cast<const std::uint8_t *>(sections) - file.data() +
                  nt->FileHeader.NumberOfSections *
                      sizeof(IMAGE_SECTION_HEADER) <=
              file.size(),
          "complete PE section headers");
  const auto image_size = nt->OptionalHeader.SizeOfImage;
  auto *image = static_cast<std::uint8_t *>(VirtualAlloc(
      nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  require(image != nullptr, "allocate private offline PE mapping");
  std::memcpy(image, file.data(), nt->OptionalHeader.SizeOfHeaders);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto &section = sections[i];
    require(static_cast<std::size_t>(section.PointerToRawData) +
                        section.SizeOfRawData <=
                    file.size() &&
                static_cast<std::size_t>(section.VirtualAddress) +
                        section.SizeOfRawData <=
                    image_size,
            "PE section stays inside file and mapping");
    std::memcpy(image + section.VirtualAddress,
                file.data() + section.PointerToRawData, section.SizeOfRawData);
  }

  // Installation validates this untouched mapping before the native entrypoints
  // are replaced with mocks. The executable and running game are not modified.
  constexpr std::size_t call_rva = 0x4c57f4f;
  constexpr std::size_t count_call_rva = 0x4c57f5b;
  require(call_rva + 5 < image_size, "cache-update call is within mapping");
  std::array<std::uint8_t, 5> original;
  std::memcpy(original.data(), image + call_rva, original.size());
  std::array<std::uint8_t, 5> original_count;
  std::memcpy(original_count.data(), image + count_call_rva,
              original_count.size());
  std::array<std::uint8_t, 33> original_context;
  std::memcpy(original_context.data(), image + call_rva - 12,
              original_context.size());
  require(!install_cache_update_guard(nullptr, image_size) &&
              !install_cache_update_guard(image, 0) &&
              !install_cache_update_guard(image, call_rva),
          "null and truncated mappings are rejected");
  auto *mapped_nt =
      reinterpret_cast<IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
  const auto reject_changed_identity = [&](auto &field) {
    field ^= 1;
    require(!install_cache_update_guard(image, image_size),
            "every executable identity field is enforced");
    field ^= 1;
  };
  reject_changed_identity(mapped_nt->Signature);
  reject_changed_identity(mapped_nt->FileHeader.Machine);
  reject_changed_identity(mapped_nt->FileHeader.SizeOfOptionalHeader);
  reject_changed_identity(mapped_nt->FileHeader.TimeDateStamp);
  reject_changed_identity(mapped_nt->OptionalHeader.Magic);
  reject_changed_identity(mapped_nt->OptionalHeader.SizeOfImage);
  reject_changed_identity(mapped_nt->OptionalHeader.CheckSum);
  const std::array<std::array<std::size_t, 2>, 5> fingerprints{
      {{0x4c57f43, 33},
       {0x4b72830, 181},
       {0x4b7ade0, 61},
       {0x4b70500, 7},
       {0x4b87c32, 57}}};
  for (const auto &[rva, size] : fingerprints) {
    for (std::size_t i = 0; i < size; ++i) {
      image[rva + i] ^= 1;
      require(!install_cache_update_guard(image, image_size),
              "every audited native fingerprint byte is enforced");
      image[rva + i] ^= 1;
    }
  }
  require(install_cache_update_guard(image, image_size),
          "verified current game code accepts offline installation");
  std::array<std::uint8_t, 5> patched;
  std::memcpy(patched.data(), image + call_rva, patched.size());
  std::array<std::uint8_t, 5> patched_count;
  std::memcpy(patched_count.data(), image + count_call_rva,
              patched_count.size());
  auto expected_context = original_context;
  std::memcpy(expected_context.data() + 12, patched.data(), patched.size());
  std::memcpy(expected_context.data() + 24, patched_count.data(),
              patched_count.size());
  require(patched != original && patched_count != original_count &&
              patched[0] == 0xe8 && patched_count[0] == 0xe8 &&
              std::memcmp(image + call_rva - 12, expected_context.data(),
                          expected_context.size()) == 0,
          "installation redirects only update and initial graph-count CALLs");
  require(install_cache_update_guard(image, image_size),
          "repeated installation preserves the published hook");
  require(!configure_cache_update_guard_for_testing(
              {fake_busy, fake_drain, fake_graph_count}),
          "test callbacks cannot replace a published native guard");

  // Execute the installed CALL in the private image with mock native entries.
  // Validation above saw untouched game bytes; the real native functions are
  // replaced before any execution, so no game subsystem can be entered.
  std::array<std::uint8_t, 31> saved_caller;
  std::memcpy(saved_caller.data(), image + call_rva - 9, saved_caller.size());
  const std::array<std::uint8_t, 9> prologue{0x48, 0x83, 0xec, 0x28, 0x48,
                                             0x89, 0x4c, 0x24, 0x20};
  const std::array<std::uint8_t, 7> reload_cache{0x48, 0x8b, 0x4c, 0x24,
                                                 0x20, 0x90, 0x90};
  const std::array<std::uint8_t, 5> epilogue{0x48, 0x83, 0xc4, 0x28, 0xc3};
  std::memcpy(image + call_rva - 9, prologue.data(), prologue.size());
  std::memcpy(image + call_rva + 5, reload_cache.data(), reload_cache.size());
  std::memcpy(image + count_call_rva + 5, epilogue.data(), epilogue.size());
  std::array<std::uint8_t, 12> saved_busy;
  std::array<std::uint8_t, 12> saved_drain;
  std::array<std::uint8_t, 12> saved_count;
  std::memcpy(saved_busy.data(), image + 0x4b72830, saved_busy.size());
  std::memcpy(saved_drain.data(), image + 0x4b7ade0, saved_drain.size());
  std::memcpy(saved_count.data(), image + 0x4b70500, saved_count.size());
  const auto replace_entry = [&](std::size_t rva, std::uintptr_t destination) {
    std::array<std::uint8_t, 12> thunk{0x48, 0xb8, 0, 0, 0,    0,
                                       0,    0,    0, 0, 0xff, 0xe0};
    std::memcpy(thunk.data() + 2, &destination, sizeof(destination));
    std::memcpy(image + rva, thunk.data(), thunk.size());
  };
  replace_entry(0x4b72830, reinterpret_cast<std::uintptr_t>(&fake_busy));
  replace_entry(0x4b7ade0, reinterpret_cast<std::uintptr_t>(&fake_drain));
  replace_entry(0x4b70500, reinterpret_cast<std::uintptr_t>(&fake_graph_count));
  require(FlushInstructionCache(GetCurrentProcess(), image, image_size) != 0,
          "publish private mock instructions");
  FakeCache native;
  native.worker_busy.store(true);
  native.additions = {401, 402};
  native.removals = {400};
  native.live = {400};
  native.graph_ids = {84, 43, 84};
  native.set_pending_counts({2, 0, 1, 0});
  const auto invoke = reinterpret_cast<int (*)(void *)>(image + call_rva - 9);
  const auto before = cache_update_guard_counters();
  require(invoke(&native) == 0 && native.count_calls == 0 &&
              native.busy_calls == 1 && native.drain_calls == 0 &&
              native.additions == std::vector<int>({401, 402}) &&
              native.removals == std::vector<int>({400}) &&
              native.live == std::vector<int>({400}) &&
              native.graph_ids == std::vector<int>({84, 43, 84}),
          "both installed CALLs defer updates and preserve pending graph IDs");
  native.worker_busy.store(false);
  require(
      invoke(&native) == 3 && native.count_calls == 1 &&
          native.busy_calls == 2 && native.drain_calls == 1 &&
          native.live == std::vector<int>({401, 402}) &&
          native.applied == std::vector<int>({401, 402, -400}) &&
          native.graph_ids == std::vector<int>({84, 43, 84}),
      "installed CALLs preserve cache argument, stack and native graph count");
  const auto after = cache_update_guard_counters();
  require(after.deferred == before.deferred + 1 &&
              after.executed == before.executed + 1 &&
              after.graph_deferred == before.graph_deferred + 1,
          "installed CALLs record update and dependent graph deferrals");
  std::memcpy(image + call_rva - 9, saved_caller.data(), saved_caller.size());
  std::memcpy(image + 0x4b72830, saved_busy.data(), saved_busy.size());
  std::memcpy(image + 0x4b7ade0, saved_drain.data(), saved_drain.size());
  std::memcpy(image + 0x4b70500, saved_count.data(), saved_count.size());

  DWORD old_protection{};
  require(VirtualProtect(image + call_rva, patched.size(),
                         PAGE_EXECUTE_READWRITE, &old_protection) != 0,
          "make private fixture patch writable");
  image[call_rva + 1] ^= 1;
  require(!uninstall_cache_update_guard(),
          "rollback refuses to overwrite a third-party CALL patch");
  require(image[call_rva + 1] == static_cast<std::uint8_t>(patched[1] ^ 1) &&
              std::memcmp(image + count_call_rva, patched_count.data(),
                          patched_count.size()) == 0,
          "foreign update patch and dependent graph guard remain untouched");
  std::memcpy(image + call_rva, patched.data(), patched.size());
  require(uninstall_cache_update_guard(),
          "owned CALL patch can be rolled back");
  require(std::memcmp(image + call_rva, original.data(), original.size()) ==
                  0 &&
              std::memcmp(image + count_call_rva, original_count.data(),
                          original_count.size()) == 0,
          "rollback restores both original native CALLs exactly");
  require(uninstall_cache_update_guard(), "repeated removal is harmless");
  require(!configure_cache_update_guard_for_testing(
              {fake_busy, fake_drain, fake_graph_count}),
          "removal keeps callbacks immutable for already-fetched calls");
  require(install_cache_update_guard(image, image_size),
          "same-image reinstall reuses the retained stubs");
  image[count_call_rva + 1] ^= 1;
  require(
      !uninstall_cache_update_guard() &&
          std::memcmp(image + call_rva, original.data(), original.size()) ==
              0 &&
          image[count_call_rva + 1] ==
              static_cast<std::uint8_t>(patched_count[1] ^ 1),
      "partial removal restores update CALL and preserves foreign count CALL");
  std::memcpy(image + count_call_rva, patched_count.data(),
              patched_count.size());
  require(uninstall_cache_update_guard() &&
              std::memcmp(image + count_call_rva, original_count.data(),
                          original_count.size()) == 0,
          "partial removal completes once graph CALL ownership is restored");
  require(VirtualFree(image, 0, MEM_RELEASE) != 0, "release offline mapping");
}

int main(int argc, char **argv) {
  try {
    require(argc == 1 || argc == 2,
            "usage: cache_update_guard_tests [game.exe]");
    require(configure_cache_update_guard_for_testing(
                {fake_busy, fake_drain, fake_graph_count}),
            "configure isolated callback fixture");
    test_callback_configuration();
    test_graph_consumption_waits_for_pending_updates();
    test_deferred_queues_and_eventual_drain();
    test_independent_caches();
    test_active_worker_does_not_block_update();
    if (argc == 2)
      test_offline_installation(argv[1]);
    const auto counters = cache_update_guard_counters();
    std::printf(
        "PASS: %zu checks; deferred=%llu executed=%llu graph_deferred=%llu\n",
        checks, static_cast<unsigned long long>(counters.deferred),
        static_cast<unsigned long long>(counters.executed),
        static_cast<unsigned long long>(counters.graph_deferred));
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL after %zu checks: %s\n", checks, error.what());
    return 1;
  }
}
