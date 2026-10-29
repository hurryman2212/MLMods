#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "MeleeAssignmentGuard.hpp"

// Defensive corruption limit, well above the size of a single native regiment.
static constexpr std::int32_t MAX_ASSIGNMENT_POINTS = 4096;
static constexpr std::size_t MATCH_RVA = 0x4aeea10;
static constexpr std::size_t RESERVE_INTS_RVA = 0x10df930;
static constexpr std::size_t FREE_MEMORY_RVA = 0x1129e10;
static constexpr std::size_t MATCH_CALL_RVA = 0x4d42756;
static constexpr std::array<std::uint8_t, 5> ORIGINAL_MATCH_CALL{
    0xe8, 0xb5, 0xc2, 0xda, 0xff};

struct MeleeCodeFingerprint {
  std::size_t rva;
  std::size_t size;
  std::uint64_t fnv1a;
};

// Entire audited caller, matcher, both solver branches, int-array reserve, and
// engine free bodies. FNV is a binary compatibility fingerprint, not a security
// boundary. The PE identity is independently checked before any publication.
static constexpr std::array<MeleeCodeFingerprint, 6> MELEE_FINGERPRINTS{{
    {0x4d41ec0, 0x1a58, 0x7f634849b961b9d6ULL},
    {MATCH_RVA, 0x2b9, 0xe93313b236962c7dULL},
    {0x4af06a0, 0x1020, 0xea4ebbf451ec9454ULL},
    {0x4af16c0, 0x36b, 0xb6bb70ef6940238fULL},
    {RESERVE_INTS_RVA, 0x64, 0x7b964e1c3ba79bbfULL},
    {FREE_MEMORY_RVA, 0x34, 0x7cca1b35c949dfc1ULL},
}};

static MeleeAssignmentGuardCallbacks melee_callbacks{};
static std::atomic<std::uint64_t> melee_passed_through{};
static std::atomic<std::uint64_t> melee_transposed{};
static std::atomic<std::uint64_t> melee_reassigned{};
static std::atomic<std::uint64_t> melee_rejected{};
static std::uint8_t *melee_published_image{};
static std::uint8_t *melee_published_stub{};
static std::array<std::uint8_t, 5> published_match_call{};
static bool melee_installed{};

static_assert(sizeof(MeleeAssignmentArray) == 16);
static_assert(sizeof(MeleeAssignmentPoint) == 24);

static auto melee_memory_span(const void *address, const std::size_t size,
                              const bool writable = false,
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
    const auto protection = region.Protect & 0xff;
    const bool writes = protection == PAGE_READWRITE ||
                        protection == PAGE_WRITECOPY ||
                        protection == PAGE_EXECUTE_READWRITE ||
                        protection == PAGE_EXECUTE_WRITECOPY;
    const bool reads = writes || protection == PAGE_READONLY ||
                       protection == PAGE_EXECUTE_READ;
    const bool runs = protection == PAGE_EXECUTE_READ ||
                      protection == PAGE_EXECUTE_READWRITE ||
                      protection == PAGE_EXECUTE_WRITECOPY;
    const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (!reads || (writable && !writes) || (executable && !runs) ||
        region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base ||
        base + region.RegionSize <= cursor) {
      return false;
    }
    cursor = base + region.RegionSize;
  }
  return true;
}

static auto valid_melee_array(const MeleeAssignmentArray &array,
                              const std::size_t stride,
                              const bool writable = false) -> bool {
  return array.count >= 0 && array.count <= MAX_ASSIGNMENT_POINTS &&
         array.capacity >= array.count &&
         (array.count == 0 ||
          melee_memory_span(array.data,
                            static_cast<std::size_t>(array.count) * stride,
                            writable));
}

auto configure_melee_assignment_guard_for_testing(
    const MeleeAssignmentGuardCallbacks callbacks) -> bool {
  if (melee_published_image != nullptr || callbacks.match == nullptr ||
      callbacks.reserve_ints == nullptr || callbacks.free_memory == nullptr) {
    return false;
  }
  melee_callbacks = callbacks;
  melee_passed_through.store(0, std::memory_order_relaxed);
  melee_transposed.store(0, std::memory_order_relaxed);
  melee_reassigned.store(0, std::memory_order_relaxed);
  melee_rejected.store(0, std::memory_order_relaxed);
  return true;
}

auto guarded_melee_assignment(MeleeAssignmentArray *output,
                              const MeleeAssignmentArray *rows,
                              const MeleeAssignmentArray *columns,
                              const bool bias) -> MeleeAssignmentArray * {
  if (output == rows || output == columns ||
      !melee_memory_span(output, sizeof(*output), true)) {
    melee_rejected.fetch_add(1, std::memory_order_relaxed);
    return output;
  }
  // The original function receives uninitialized result storage and constructs
  // a fresh TArray. Never read or free the previous bytes of this out
  // parameter.
  *output = {};
  if (melee_callbacks.match == nullptr ||
      melee_callbacks.reserve_ints == nullptr ||
      melee_callbacks.free_memory == nullptr ||
      !melee_memory_span(rows, sizeof(*rows)) ||
      !melee_memory_span(columns, sizeof(*columns)) ||
      !valid_melee_array(*rows, sizeof(MeleeAssignmentPoint)) ||
      !valid_melee_array(*columns, sizeof(MeleeAssignmentPoint))) {
    melee_rejected.fetch_add(1, std::memory_order_relaxed);
    return output;
  }
  // Keep native squared-distance and centroid calculations within double's
  // finite range even for corrupt coordinates that are themselves finite.
  const auto coordinate_limit =
      std::sqrt(std::numeric_limits<double>::max()) / 8;
  for (const auto *array : {rows, columns}) {
    const auto *points = static_cast<const MeleeAssignmentPoint *>(array->data);
    for (std::int32_t index = 0; index < array->count; ++index) {
      const auto &point = points[index];
      if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
          !std::isfinite(point.z) || std::abs(point.x) > coordinate_limit ||
          std::abs(point.y) > coordinate_limit ||
          std::abs(point.z) > coordinate_limit) {
        melee_rejected.fetch_add(1, std::memory_order_relaxed);
        return output;
      }
    }
  }
  if (rows->count == 0 || columns->count == 0) {
    return output;
  }
  if (rows->count <= columns->count) {
    melee_passed_through.fetch_add(1, std::memory_order_relaxed);
    return melee_callbacks.match(output, rows, columns, bias);
  }

  MeleeAssignmentArray reverse{};
  try {
    // RVA 0x10df930 reserves a TArray<int32> with the game's allocator; it
    // adjusts capacity but leaves count unchanged. Caller frees output
    // natively.
    melee_callbacks.reserve_ints(output, rows->count);
    if (output->count != 0 || output->capacity < rows->count ||
        !melee_memory_span(output->data,
                           static_cast<std::size_t>(rows->count) *
                               sizeof(std::int32_t),
                           true)) {
      melee_rejected.fetch_add(1, std::memory_order_relaxed);
      output->count = 0;
      return output;
    }
    auto *assignment = static_cast<std::int32_t *>(output->data);
    for (std::int32_t index = 0; index < rows->count; ++index) {
      assignment[index] = -1;
    }
    // More rows than columns reaches an unsafe square solver in vanilla.
    // Transposition uses its audited rectangular solver, preserving native
    // distance costs and one-to-one matching for every actual target.
    auto *result = melee_callbacks.match(&reverse, columns, rows, bias);
    bool valid = result == &reverse && reverse.count == columns->count &&
                 valid_melee_array(reverse, sizeof(std::int32_t));
    if (valid) {
      const auto *inverse = static_cast<const std::int32_t *>(reverse.data);
      for (std::int32_t target = 0; target < reverse.count; ++target) {
        const auto soldier = inverse[target];
        if (soldier < 0 || soldier >= rows->count ||
            assignment[soldier] != -1) {
          valid = false;
          break;
        }
        assignment[soldier] = target;
      }
    }
    if (reverse.data != nullptr) {
      melee_callbacks.free_memory(reverse.data);
      reverse = {};
    }
    if (!valid) {
      melee_rejected.fetch_add(1, std::memory_order_relaxed);
      return output;
    }
    const auto *source_points =
        static_cast<const MeleeAssignmentPoint *>(rows->data);
    const auto *target_points =
        static_cast<const MeleeAssignmentPoint *>(columns->data);
    std::uint64_t additional{};
    for (std::int32_t soldier = 0; soldier < rows->count; ++soldier) {
      if (assignment[soldier] >= 0) {
        continue;
      }
      auto best = std::numeric_limits<double>::infinity();
      std::int32_t closest{};
      for (std::int32_t target = 0; target < columns->count; ++target) {
        const auto dx = source_points[soldier].x - target_points[target].x;
        const auto dy = source_points[soldier].y - target_points[target].y;
        const auto distance = dx * dx + dy * dy;
        if (distance < best) {
          best = distance;
          closest = target;
        }
      }
      // Native d + 0.25*abs(centroidDistanceSquared-d) is strictly increasing
      // in d. Nearest XY is therefore exactly its minimum-cost remaining
      // target.
      assignment[soldier] = closest;
      ++additional;
    }
    output->count = rows->count;
    melee_transposed.fetch_add(1, std::memory_order_relaxed);
    melee_reassigned.fetch_add(additional, std::memory_order_relaxed);
  } catch (...) {
    if (reverse.data != nullptr) {
      melee_callbacks.free_memory(reverse.data);
    }
    output->count = 0;
    melee_rejected.fetch_add(1, std::memory_order_relaxed);
  }
  return output;
}

auto melee_assignment_guard_counters() -> MeleeAssignmentGuardCounters {
  return {melee_passed_through.load(std::memory_order_relaxed),
          melee_transposed.load(std::memory_order_relaxed),
          melee_reassigned.load(std::memory_order_relaxed),
          melee_rejected.load(std::memory_order_relaxed)};
}

static auto allocate_melee_stub(const std::uintptr_t return_address)
    -> std::uint8_t * {
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const auto granularity =
      static_cast<std::uintptr_t>(system.dwAllocationGranularity);
  const auto minimum =
      reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
  const auto maximum =
      reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
  const auto reach =
      static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max());
  auto cursor = return_address > reach ? return_address - reach : minimum;
  if (cursor < minimum) {
    cursor = minimum;
  }
  cursor = (cursor + granularity - 1) & ~(granularity - 1);
  const auto last =
      maximum - return_address < reach ? maximum : return_address + reach;
  while (cursor <= last && last - cursor >= 16) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<void *>(cursor), &region,
                     sizeof(region)) != sizeof(region)) {
      break;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base) {
      break;
    }
    const auto end = base + region.RegionSize;
    if (region.State == MEM_FREE && end > cursor && end - cursor >= 16) {
      auto *stub = static_cast<std::uint8_t *>(
          VirtualAlloc(reinterpret_cast<void *>(cursor), 16,
                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
      if (stub != nullptr) {
        return stub;
      }
      cursor += granularity;
    } else {
      if (end <= cursor || end > last) {
        break;
      }
      cursor = (end + granularity - 1) & ~(granularity - 1);
    }
  }
  return nullptr;
}

static auto replace_melee_call(std::uint8_t *image, const bool install)
    -> bool {
  if (melee_installed == install) {
    return true;
  }
  auto *site = image + MATCH_CALL_RVA;
  const auto &expected = install ? ORIGINAL_MATCH_CALL : published_match_call;
  const auto &replacement =
      install ? published_match_call : ORIGINAL_MATCH_CALL;
  if (!melee_memory_span(site, expected.size(), false, true) ||
      std::memcmp(site, expected.data(), expected.size()) != 0) {
    return false;
  }
  DWORD old_protection{};
  if (!VirtualProtect(site, expected.size(), PAGE_EXECUTE_READWRITE,
                      &old_protection)) {
    return false;
  }
  // This unaligned E8 displacement cannot use InterlockedCompareExchange.
  // Lifecycle caller guarantees no gameplay execution during publication.
  const bool unchanged =
      std::memcmp(site, expected.data(), expected.size()) == 0;
  if (unchanged) {
    std::memcpy(site, replacement.data(), replacement.size());
    melee_installed = install;
  }
  DWORD ignored{};
  if (!VirtualProtect(site, expected.size(), old_protection, &ignored)) {
    OutputDebugStringW(
        L"[MLCrashFix] Melee guard protection restore failed.\n");
  }
  if (unchanged &&
      !FlushInstructionCache(GetCurrentProcess(), site, replacement.size())) {
    OutputDebugStringW(L"[MLCrashFix] Melee guard instruction flush failed.\n");
  }
  return unchanged;
}

auto install_melee_assignment_guard(std::uint8_t *image, const std::size_t size)
    -> bool {
  if (image == nullptr || size < 0x09deb000 ||
      !melee_memory_span(image, sizeof(IMAGE_DOS_HEADER)) ||
      (melee_published_image != nullptr && melee_published_image != image)) {
    return false;
  }
  IMAGE_DOS_HEADER dos{};
  std::memcpy(&dos, image, sizeof(dos));
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
      static_cast<std::size_t>(dos.e_lfanew) >
          size - sizeof(IMAGE_NT_HEADERS64) ||
      !melee_memory_span(image + dos.e_lfanew, sizeof(IMAGE_NT_HEADERS64))) {
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
      nt.OptionalHeader.CheckSum != 0x09a1d1f2) {
    return false;
  }
  const auto &expected_call =
      melee_installed ? published_match_call : ORIGINAL_MATCH_CALL;
  if (!melee_memory_span(image + MATCH_CALL_RVA, 5, false, true) ||
      std::memcmp(image + MATCH_CALL_RVA, expected_call.data(), 5) != 0) {
    return false;
  }
  for (const auto &fingerprint : MELEE_FINGERPRINTS) {
    if (fingerprint.rva > size || fingerprint.size > size - fingerprint.rva ||
        !melee_memory_span(image + fingerprint.rva, fingerprint.size, false,
                           true)) {
      return false;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t offset = 0; offset < fingerprint.size; ++offset) {
      const auto rva = fingerprint.rva + offset;
      auto value = image[rva];
      if (melee_installed && rva >= MATCH_CALL_RVA &&
          rva < MATCH_CALL_RVA + ORIGINAL_MATCH_CALL.size()) {
        value = ORIGINAL_MATCH_CALL[rva - MATCH_CALL_RVA];
      }
      hash = (hash ^ value) * 1099511628211ULL;
    }
    if (hash != fingerprint.fnv1a) {
      return false;
    }
  }
  if (melee_installed) {
    return true;
  }
  auto *stub = melee_published_stub;
  if (stub == nullptr) {
    stub = allocate_melee_stub(
        reinterpret_cast<std::uintptr_t>(image + MATCH_CALL_RVA + 5));
    if (stub == nullptr) {
      return false;
    }
    const std::array<std::uint8_t, 6> jump{0xff, 0x25, 0, 0, 0, 0};
    const auto target =
        reinterpret_cast<std::uintptr_t>(&guarded_melee_assignment);
    std::memcpy(stub, jump.data(), jump.size());
    std::memcpy(stub + jump.size(), &target, sizeof(target));
    DWORD ignored{};
    if (!VirtualProtect(stub, 16, PAGE_EXECUTE_READ, &ignored) ||
        !FlushInstructionCache(GetCurrentProcess(), stub, 16)) {
      VirtualFree(stub, 0, MEM_RELEASE);
      return false;
    }
  }
  HMODULE pinned{};
  const auto displacement =
      reinterpret_cast<std::intptr_t>(stub) -
      reinterpret_cast<std::intptr_t>(image + MATCH_CALL_RVA + 5);
  if (displacement < std::numeric_limits<std::int32_t>::min() ||
      displacement > std::numeric_limits<std::int32_t>::max() ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&guarded_melee_assignment),
                          &pinned)) {
    if (melee_published_stub == nullptr) {
      VirtualFree(stub, 0, MEM_RELEASE);
    }
    return false;
  }
  const auto relative = static_cast<std::int32_t>(displacement);
  published_match_call = ORIGINAL_MATCH_CALL;
  std::memcpy(published_match_call.data() + 1, &relative, sizeof(relative));
  const auto previous = melee_callbacks;
  if (melee_published_image == nullptr) {
    melee_callbacks = {
        reinterpret_cast<decltype(melee_callbacks.match)>(image + MATCH_RVA),
        reinterpret_cast<decltype(melee_callbacks.reserve_ints)>(
            image + RESERVE_INTS_RVA),
        reinterpret_cast<decltype(melee_callbacks.free_memory)>(
            image + FREE_MEMORY_RVA)};
  }
  if (!replace_melee_call(image, true)) {
    if (melee_published_stub == nullptr) {
      melee_callbacks = previous;
      VirtualFree(stub, 0, MEM_RELEASE);
    }
    return false;
  }
  // Retain executable stub, callbacks, and pinned module even after removal.
  melee_published_image = image;
  melee_published_stub = stub;
  return true;
}

auto remove_melee_assignment_guard() -> bool {
  return melee_published_image == nullptr ||
         replace_melee_call(melee_published_image, false);
}
