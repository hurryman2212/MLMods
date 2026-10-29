#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "CacheUpdateGuard.hpp"

static constexpr std::size_t CACHE_BUSY_RVA = 0x4b72830;
static constexpr std::size_t CACHE_DRAIN_RVA = 0x4b7ade0;
static constexpr std::size_t CACHE_COUNT_RVA = 0x4b70500;
static constexpr std::size_t CACHE_PREREQUISITE_RVA = 0x4b87c32;
static constexpr std::size_t CACHE_CALLER_RVA = 0x4c57f43;
static constexpr std::size_t STUB_STRIDE = 16;
static constexpr std::size_t STUB_ALLOCATION_SIZE = 2 * STUB_STRIDE;

// Native 0.8.111: all three asynchronous task slots are checked by this native
// readiness function. Its locked completion checks also supply synchronization.
static constexpr std::array<std::uint8_t, 181> CACHE_BUSY_BYTES{
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
    0x83, 0xec, 0x20, 0x48, 0x8b, 0x99, 0x20, 0x03, 0x00, 0x00, 0x33, 0xf6,
    0x48, 0x8b, 0xf9, 0x48, 0x85, 0xdb, 0x74, 0x28, 0x33, 0xc0, 0xf0, 0x0f,
    0xb1, 0x73, 0x10, 0x75, 0x68, 0x0f, 0xae, 0xf8, 0x48, 0x39, 0x73, 0x20,
    0x74, 0x16, 0x48, 0x8b, 0x4b, 0x18, 0x45, 0x33, 0xc0, 0xba, 0xff, 0xff,
    0xff, 0xff, 0x48, 0x8b, 0x01, 0xff, 0x50, 0x20, 0x48, 0x89, 0x73, 0x20,
    0x48, 0x8b, 0x9f, 0xe0, 0x03, 0x00, 0x00, 0x48, 0x85, 0xdb, 0x74, 0x28,
    0x33, 0xc0, 0xf0, 0x0f, 0xb1, 0x73, 0x10, 0x75, 0x34, 0x0f, 0xae, 0xf8,
    0x48, 0x39, 0x73, 0x20, 0x74, 0x16, 0x48, 0x8b, 0x4b, 0x18, 0x45, 0x33,
    0xc0, 0xba, 0xff, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x01, 0xff, 0x50, 0x20,
    0x48, 0x89, 0x73, 0x20, 0x48, 0x8b, 0x8f, 0xb0, 0x04, 0x00, 0x00, 0x48,
    0x85, 0xc9, 0x74, 0x1b, 0xe8, 0x53, 0x2c, 0x6b, 0xfc, 0x84, 0xc0, 0x75,
    0x12, 0xb0, 0x01, 0x48, 0x8b, 0x5c, 0x24, 0x30, 0x48, 0x8b, 0x74, 0x24,
    0x38, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3, 0x48, 0x8b, 0x5c, 0x24, 0x30,
    0x32, 0xc0, 0x48, 0x8b, 0x74, 0x24, 0x38, 0x48, 0x83, 0xc4, 0x20, 0x5f,
    0xc3};

// Includes the non-null cache check and the following queue-count branch.
static constexpr std::array<std::uint8_t, 33> CACHE_CALLER_BYTES{
    0x48, 0x8b, 0x8b, 0x38, 0x07, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74,
    0x31, 0xe8, 0x8c, 0x2e, 0xf2, 0xff, 0x48, 0x8b, 0x8b, 0x38, 0x07,
    0x00, 0x00, 0xe8, 0xa0, 0x85, 0xf1, 0xff, 0x85, 0xc0, 0x7e, 0x1c};
static constexpr std::array<std::uint8_t, 61> CACHE_DRAIN_BYTES{
    0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xd9, 0x48,
    0x81, 0xec, 0xd0, 0x00, 0x00, 0x00, 0x45, 0x33, 0xc0, 0xc7, 0x45,
    0x87, 0xff, 0xff, 0xff, 0xff, 0x4c, 0x8d, 0xa9, 0x78, 0x02, 0x00,
    0x00, 0x44, 0x89, 0x44, 0x24, 0x20, 0x45, 0x8b, 0x55, 0x28, 0x4d,
    0x8d, 0x7d, 0x10, 0x45, 0x33, 0xc9};
static constexpr std::array<std::uint8_t, 7> CACHE_COUNT_BYTES{
    0x8b, 0x81, 0x00, 0x04, 0x00, 0x00, 0xc3};
// The native cache tick only updates the graph when both prerequisite sparse
// queues have no live entries (allocated slots equal free slots).
static constexpr std::array<std::uint8_t, 57> CACHE_PREREQUISITE_BYTES{
    0x8b, 0x87, 0x30, 0x02, 0x00, 0x00, 0x48, 0x8b, 0x74, 0x24, 0x70, 0x48,
    0x8b, 0x5c, 0x24, 0x68, 0x3b, 0x87, 0x5c, 0x02, 0x00, 0x00, 0x75, 0x1b,
    0x8b, 0x87, 0x80, 0x02, 0x00, 0x00, 0x3b, 0x87, 0xac, 0x02, 0x00, 0x00,
    0x75, 0x0d, 0x48, 0x8b, 0xcf, 0x48, 0x83, 0xc4, 0x50, 0x5f, 0xe9, 0x8b,
    0xd1, 0x01, 0x00, 0x48, 0x83, 0xc4, 0x50, 0x5f, 0xc3};

struct CacheCallPatch {
  std::size_t rva;
  LONG original;
  LONG displacement{};
  bool installed{};
};

// Publish the dependent graph guard before allowing obstacle drain deferral.
static std::array<CacheCallPatch, 2> cache_call_patches{{
    {0x4c57f5b, -0xe7a60},
    {0x4c57f4f, -0xdd174},
}};

static CacheUpdateGuardCallbacks guard_callbacks{};
static std::atomic<std::uint64_t> deferred_updates{};
static std::atomic<std::uint64_t> executed_updates{};
static std::atomic<std::uint64_t> deferred_graph_updates{};
static std::uint8_t *published_image{};
static std::uint8_t *published_stub{};

auto configure_cache_update_guard_for_testing(
    const CacheUpdateGuardCallbacks callbacks) -> bool {
  if (published_image != nullptr || callbacks.busy == nullptr ||
      callbacks.drain == nullptr || callbacks.count == nullptr) {
    return false;
  }
  guard_callbacks = callbacks;
  deferred_updates.store(0, std::memory_order_relaxed);
  executed_updates.store(0, std::memory_order_relaxed);
  deferred_graph_updates.store(0, std::memory_order_relaxed);
  return true;
}

auto guarded_cache_update(void *cache) -> void {
  if (cache == nullptr || guard_callbacks.busy == nullptr ||
      guard_callbacks.drain == nullptr) {
    return;
  }
  // Cache workers are launched on the game thread. Leaving the queue untouched
  // while busy lets the native cache tick at RVA 0x4b879e0 retry it later.
  if (guard_callbacks.busy(cache)) {
    deferred_updates.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  executed_updates.fetch_add(1, std::memory_order_relaxed);
  guard_callbacks.drain(cache);
}

auto guarded_cache_graph_count(void *cache) -> int {
  if (cache == nullptr || guard_callbacks.count == nullptr) {
    return 0;
  }
  const auto *bytes = static_cast<const std::uint8_t *>(cache);
  std::int32_t queued_additions{}, free_additions{};
  std::int32_t queued_removals{}, free_removals{};
  std::memcpy(&queued_additions, bytes + 0x230, sizeof(queued_additions));
  std::memcpy(&free_additions, bytes + 0x25c, sizeof(free_additions));
  std::memcpy(&queued_removals, bytes + 0x280, sizeof(queued_removals));
  std::memcpy(&free_removals, bytes + 0x2ac, sizeof(free_removals));
  // The worker may have completed since the preceding busy check. Inspect the
  // untouched queues themselves, as the native tick does, without checking busy
  // again or consuming the independent graph queue at cache + 0x3f8.
  if (queued_additions != free_additions || queued_removals != free_removals) {
    deferred_graph_updates.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  return guard_callbacks.count(cache);
}

auto cache_update_guard_counters() -> CacheUpdateGuardCounters {
  return {deferred_updates.load(std::memory_order_relaxed),
          executed_updates.load(std::memory_order_relaxed),
          deferred_graph_updates.load(std::memory_order_relaxed)};
}

static auto readable_span(const void *address, const std::size_t size,
                          const bool executable = false) -> bool {
  auto cursor = reinterpret_cast<std::uintptr_t>(address);
  if (cursor == 0 || size == 0 ||
      size > std::numeric_limits<std::uintptr_t>::max() - cursor) {
    return false;
  }
  const auto end = cursor + size;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void *>(cursor), &region,
                     sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD) != 0) {
      return false;
    }
    const DWORD protection = region.Protect & 0xff;
    const bool readable =
        protection == PAGE_READONLY || protection == PAGE_READWRITE ||
        protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;
    const bool runs = protection == PAGE_EXECUTE_READ ||
                      protection == PAGE_EXECUTE_READWRITE ||
                      protection == PAGE_EXECUTE_WRITECOPY;
    if (!readable || (executable && !runs)) {
      return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base ||
        base + region.RegionSize <= cursor) {
      return false;
    }
    cursor = base + region.RegionSize;
  }
  return true;
}

static auto allocate_near_stub(const std::uintptr_t return_address)
    -> std::uint8_t * {
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const std::uintptr_t granularity = system.dwAllocationGranularity;
  const auto minimum =
      reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
  const auto maximum =
      reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
  // The unchanged E8 opcode uses a signed 32-bit relative displacement.
  const std::uintptr_t reach = std::numeric_limits<std::int32_t>::max();
  auto cursor = return_address > reach ? return_address - reach : minimum;
  if (cursor < minimum) {
    cursor = minimum;
  }
  cursor = (cursor + granularity - 1) & ~(granularity - 1);
  const auto last =
      maximum - return_address < reach ? maximum : return_address + reach;
  while (cursor <= last && last - cursor >= STUB_ALLOCATION_SIZE) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<void *>(cursor), &region,
                     sizeof(region)) != sizeof(region)) {
      break;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base) {
      break;
    }
    const auto region_end = base + region.RegionSize;
    if (region.State == MEM_FREE && region_end > cursor &&
        region_end - cursor >= STUB_ALLOCATION_SIZE) {
      auto *allocation = static_cast<std::uint8_t *>(
          VirtualAlloc(reinterpret_cast<void *>(cursor), STUB_ALLOCATION_SIZE,
                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
      if (allocation != nullptr) {
        return allocation;
      }
      cursor += granularity;
    } else {
      if (region_end <= cursor || region_end > last) {
        break;
      }
      cursor = (region_end + granularity - 1) & ~(granularity - 1);
    }
  }
  return nullptr;
}

static auto replace_cache_call(std::uint8_t *image, CacheCallPatch &patch,
                               const bool install) -> bool {
  if (patch.installed == install) {
    return true;
  }
  auto *site = image + patch.rva;
  if (!readable_span(site, 5, true) || site[0] != 0xe8) {
    return false;
  }
  DWORD old_protection{};
  if (!VirtualProtect(site + 1, sizeof(LONG), PAGE_EXECUTE_READWRITE,
                      &old_protection)) {
    return false;
  }
  // Keep E8 intact and never overwrite a later mod's relative target.
  const bool replaced =
      site[0] == 0xe8 && InterlockedCompareExchange(
                             reinterpret_cast<volatile LONG *>(site + 1),
                             install ? patch.displacement : patch.original,
                             install ? patch.original : patch.displacement) ==
                             (install ? patch.original : patch.displacement);
  if (replaced) {
    patch.installed = install;
  }
  DWORD ignored{};
  if (!VirtualProtect(site + 1, sizeof(LONG), old_protection, &ignored)) {
    OutputDebugStringW(
        L"[MLCrashFix] Cache guard page protection restore failed.\n");
  }
  if (replaced && !FlushInstructionCache(GetCurrentProcess(), site, 5)) {
    OutputDebugStringW(L"[MLCrashFix] Cache guard instruction flush failed.\n");
  }
  return replaced;
}

auto install_cache_update_guard(std::uint8_t *image, const std::size_t size)
    -> bool {
  // This fingerprint intentionally accepts only the audited native 0.8.111 PE.
  if (image == nullptr || size < 0x09deb000 ||
      !readable_span(image, sizeof(IMAGE_DOS_HEADER)) ||
      (published_image != nullptr && published_image != image)) {
    return false;
  }
  IMAGE_DOS_HEADER dos{};
  std::memcpy(&dos, image, sizeof(dos));
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
      static_cast<std::size_t>(dos.e_lfanew) >
          size - sizeof(IMAGE_NT_HEADERS64) ||
      !readable_span(image + dos.e_lfanew, sizeof(IMAGE_NT_HEADERS64))) {
    return false;
  }
  IMAGE_NT_HEADERS64 nt{};
  std::memcpy(&nt, image + dos.e_lfanew, sizeof(nt));
  if (nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
      nt.FileHeader.TimeDateStamp != 0xb47dcd3f ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.OptionalHeader.SizeOfImage != 0x09deb000 ||
      nt.OptionalHeader.CheckSum != 0x09a1d1f2 ||
      !readable_span(image + CACHE_BUSY_RVA, CACHE_BUSY_BYTES.size(), true) ||
      !readable_span(image + CACHE_DRAIN_RVA, CACHE_DRAIN_BYTES.size(), true) ||
      !readable_span(image + CACHE_COUNT_RVA, CACHE_COUNT_BYTES.size(), true) ||
      !readable_span(image + CACHE_PREREQUISITE_RVA,
                     CACHE_PREREQUISITE_BYTES.size(), true) ||
      !readable_span(image + CACHE_CALLER_RVA, CACHE_CALLER_BYTES.size(),
                     true) ||
      std::memcmp(image + CACHE_BUSY_RVA, CACHE_BUSY_BYTES.data(),
                  CACHE_BUSY_BYTES.size()) != 0 ||
      std::memcmp(image + CACHE_DRAIN_RVA, CACHE_DRAIN_BYTES.data(),
                  CACHE_DRAIN_BYTES.size()) != 0 ||
      std::memcmp(image + CACHE_COUNT_RVA, CACHE_COUNT_BYTES.data(),
                  CACHE_COUNT_BYTES.size()) != 0 ||
      std::memcmp(image + CACHE_PREREQUISITE_RVA,
                  CACHE_PREREQUISITE_BYTES.data(),
                  CACHE_PREREQUISITE_BYTES.size()) != 0) {
    return false;
  }
  auto expected_caller = CACHE_CALLER_BYTES;
  for (const auto &patch : cache_call_patches) {
    if (patch.installed) {
      std::memcpy(expected_caller.data() + patch.rva - CACHE_CALLER_RVA + 1,
                  &patch.displacement, sizeof(patch.displacement));
    }
    if ((reinterpret_cast<std::uintptr_t>(image + patch.rva + 1) %
         alignof(LONG)) != 0) {
      return false;
    }
  }
  if (std::memcmp(image + CACHE_CALLER_RVA, expected_caller.data(),
                  expected_caller.size()) != 0) {
    return false;
  }
  if (cache_call_patches[0].installed && cache_call_patches[1].installed) {
    return true;
  }
  auto *stub = published_stub;
  if (stub == nullptr) {
    stub = allocate_near_stub(reinterpret_cast<std::uintptr_t>(
        image + cache_call_patches[0].rva + 5));
    if (stub == nullptr) {
      return false;
    }
    // A tail jump preserves the original CALL's return address and Windows x64
    // ABI. This leaf stub needs no stack changes or unwind metadata.
    const std::array<std::uint8_t, 6> jump{0xff, 0x25, 0, 0, 0, 0};
    const std::array targets{
        reinterpret_cast<std::uintptr_t>(&guarded_cache_graph_count),
        reinterpret_cast<std::uintptr_t>(&guarded_cache_update)};
    for (std::size_t index = 0; index < targets.size(); ++index) {
      std::memcpy(stub + index * STUB_STRIDE, jump.data(), jump.size());
      std::memcpy(stub + index * STUB_STRIDE + jump.size(), &targets[index],
                  sizeof(targets[index]));
    }
    DWORD old_protection{};
    if (!VirtualProtect(stub, STUB_ALLOCATION_SIZE, PAGE_EXECUTE_READ,
                        &old_protection) ||
        !FlushInstructionCache(GetCurrentProcess(), stub,
                               STUB_ALLOCATION_SIZE)) {
      VirtualFree(stub, 0, MEM_RELEASE);
      return false;
    }
  }
  HMODULE pinned_module{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&guarded_cache_update),
                          &pinned_module)) {
    if (published_stub == nullptr) {
      VirtualFree(stub, 0, MEM_RELEASE);
    }
    return false;
  }
  for (std::size_t index = 0; index < cache_call_patches.size(); ++index) {
    auto &patch = cache_call_patches[index];
    const auto displacement =
        reinterpret_cast<std::intptr_t>(stub + index * STUB_STRIDE) -
        reinterpret_cast<std::intptr_t>(image + patch.rva + 5);
    if (displacement < std::numeric_limits<LONG>::min() ||
        displacement > std::numeric_limits<LONG>::max()) {
      if (published_stub == nullptr) {
        VirtualFree(stub, 0, MEM_RELEASE);
      }
      return false;
    }
    patch.displacement = static_cast<LONG>(displacement);
  }
  const auto previous_callbacks = guard_callbacks;
  if (published_image == nullptr) {
    guard_callbacks = {
        reinterpret_cast<bool (*)(void *)>(image + CACHE_BUSY_RVA),
        reinterpret_cast<void (*)(void *)>(image + CACHE_DRAIN_RVA),
        reinterpret_cast<int (*)(void *)>(image + CACHE_COUNT_RVA)};
  }
  if (!replace_cache_call(image, cache_call_patches[0], true)) {
    if (published_stub == nullptr) {
      guard_callbacks = previous_callbacks;
      VirtualFree(stub, 0, MEM_RELEASE);
    }
    return false;
  }
  // Even a rolled-back publication can leave an already-fetched CALL in flight.
  // Retain both stubs and the immutable callbacks after the first publication.
  published_image = image;
  published_stub = stub;
  if (!replace_cache_call(image, cache_call_patches[1], true)) {
    if (!replace_cache_call(image, cache_call_patches[0], false)) {
      OutputDebugStringW(
          L"[MLCrashFix] Partial cache guard rollback failed.\n");
    }
    return false;
  }
  return true;
}

auto uninstall_cache_update_guard() -> bool {
  if (published_image == nullptr) {
    return true;
  }
  // Restore the obstacle call first. If it cannot be removed, retain the graph
  // guard so any remaining obstacle deferral cannot expose stale graph input.
  if (!replace_cache_call(published_image, cache_call_patches[1], false)) {
    return false;
  }
  return replace_cache_call(published_image, cache_call_patches[0], false);
}
