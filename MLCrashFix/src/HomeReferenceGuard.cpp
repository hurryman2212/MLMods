#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "HomeReferenceGuard.hpp"

// Corruption limits exceed supported population and ordinary family sizes.
static constexpr std::int32_t MAX_HOME_FAMILIES = 65536;
static constexpr std::int32_t MAX_HOME_MEMBERS = 64;
static constexpr std::array<std::uint8_t, 16> HOME_ORIGINAL_BYTES{
    0x48, 0x8b, 0x07, 0x48, 0x8b, 0x88, 0x40, 0x03,
    0x00, 0x00, 0x8b, 0x81, 0xf8, 0x0c, 0x00, 0x00};
static constexpr std::array<std::uint8_t, 7> HOME_LOOP_TAIL{
    0x48, 0x83, 0xc7, 0x08, 0x48, 0x3b, 0xfe};

struct HomeCodeFingerprint {
  std::size_t rva;
  std::size_t size;
  std::uint64_t fnv1a;
};

// Entire demand functions and native SetFamilyHome establish the audited
// loop semantics and canonical family layout for this exact executable.
static constexpr std::array<HomeCodeFingerprint, 3> HOME_FINGERPRINTS{{
    {0x4bd7820, 0x1490, 0xd7ffc10c31de127bULL},
    {0x4bd8cb0, 0xea9, 0x5fdc8497c2e642bfULL},
    {0x4c255d0, 0x1ee, 0x50f8a72c0adc74abULL},
}};

struct HomePatch {
  std::size_t site;
  std::size_t skip;
  std::uint8_t *stub{};
  std::array<std::uint8_t, 16> jump{};
  bool installed{};
};

struct HomeArray {
  void *data;
  std::int32_t count;
  std::int32_t capacity;
};

struct HomeObjectHeader {
  std::uintptr_t vtable;
  std::uint32_t flags;
  std::int32_t index;
  void *object_class;
};

static std::array<HomePatch, 2> home_patches{{
    {0x4bd7ad0, 0x4bd7c25},
    {0x4bd91e0, 0x4bd9325},
}};
static std::uint8_t *home_validation_image{};
static std::size_t home_validation_size{};
static std::uint8_t *home_published_image{};
static std::atomic<std::uint64_t> home_repaired{};
static std::atomic<std::uint64_t> home_skipped{};

static_assert(sizeof(HomeArray) == 16);
static_assert(sizeof(HomeObjectHeader) == 24);

static auto home_memory_span(const void *address, const std::size_t size,
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

static auto home_read_bytes(const void *object, const std::size_t offset,
                            void *output, const std::size_t size) -> bool {
  const auto address = reinterpret_cast<std::uintptr_t>(object);
  if (address == 0 ||
      offset > std::numeric_limits<std::uintptr_t>::max() - address ||
      size > std::numeric_limits<std::uintptr_t>::max() - address - offset) {
    return false;
  }
  SIZE_T copied{};
  return ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address + offset),
                           output, size, &copied) != 0 &&
         copied == size;
}

template <typename Value>
static auto home_read(const void *object, const std::size_t offset,
                      Value &value) -> bool {
  return home_read_bytes(object, offset, &value, sizeof(value));
}

static auto home_object(const void *object) -> bool {
  HomeObjectHeader header{};
  std::uintptr_t class_vtable{};
  const auto image = reinterpret_cast<std::uintptr_t>(home_validation_image);
  // Same UObject destruction flags used by the existing banner recovery.
  return image != 0 && home_read(object, 0, header) && header.index > 0 &&
         (header.flags & 0x40018000U) == 0 && header.vtable >= image &&
         header.vtable - image < home_validation_size &&
         home_read(header.object_class, 0, class_vtable) &&
         class_vtable >= image && class_vtable - image < home_validation_size;
}

auto configure_home_reference_guard_for_testing(std::uint8_t *image,
                                                const std::size_t size)
    -> bool {
  if (home_published_image != nullptr || image == nullptr || size == 0 ||
      size > std::numeric_limits<std::uintptr_t>::max() -
                 reinterpret_cast<std::uintptr_t>(image)) {
    return false;
  }
  home_validation_image = image;
  home_validation_size = size;
  home_repaired.store(0, std::memory_order_relaxed);
  home_skipped.store(0, std::memory_order_relaxed);
  return true;
}

auto guarded_home_reference(void *unit) -> void * {
  void *home{};
  if (home_read(unit, 0x340, home) && home != nullptr) {
    return home;
  }
  do {
    void *region{};
    void *owner{};
    std::uint8_t unit_actor_flags{};
    std::uint8_t home_actor_flags{};
    std::int32_t family_id{};
    HomeArray families{};
    HomeArray members{};
    if (!home_object(unit) || !home_read(unit, 0x5d, unit_actor_flags) ||
        (unit_actor_flags & 1) != 0 || !home_read(unit, 0x328, region) ||
        !home_read(unit, 0x320, owner) || !home_read(unit, 0x348, family_id) ||
        !home_object(region) || !home_object(owner) ||
        !home_read(region, 0x4e8, families) || families.data == nullptr ||
        families.count < 1 || families.count > MAX_HOME_FAMILIES ||
        families.capacity < families.count ||
        families.capacity > MAX_HOME_FAMILIES || family_id < 0 ||
        family_id >= families.count) {
      break;
    }
    const auto family_base = reinterpret_cast<std::uintptr_t>(families.data);
    const auto family_offset = static_cast<std::size_t>(family_id) * 0x50;
    if (family_offset >
        std::numeric_limits<std::uintptr_t>::max() - family_base) {
      break;
    }
    const auto *family =
        reinterpret_cast<const void *>(family_base + family_offset);
    if (!home_read(family, 0, members) || members.data == nullptr ||
        members.count < 1 || members.count > MAX_HOME_MEMBERS ||
        members.capacity < members.count ||
        members.capacity > MAX_HOME_MEMBERS || !home_read(family, 0x20, home) ||
        !home_object(home) || !home_read(home, 0x5d, home_actor_flags) ||
        (home_actor_flags & 1) != 0) {
      break;
    }
    std::array<void *, MAX_HOME_MEMBERS> member_objects{};
    if (!home_read_bytes(members.data, 0, member_objects.data(),
                         static_cast<std::size_t>(members.count) *
                             sizeof(void *))) {
      break;
    }
    std::int32_t member_index{};
    while (member_index < members.count &&
           member_objects[member_index] != unit) {
      ++member_index;
    }
    void *home_region{};
    void *home_owner{};
    std::int32_t home_level{};
    if (member_index == members.count || !home_read(home, 0x2c8, home_region) ||
        home_region != region || !home_read(home, 0x2e0, home_owner) ||
        home_owner != owner || !home_read(home, 0xcf8, home_level)) {
      break;
    }
    // Recheck identity before the only write. GameThread owns this native
    // update; a stale family snapshot must never overwrite a newer Home.
    HomeArray current_families{};
    HomeArray current_members{};
    void *current_region{};
    void *current_owner{};
    void *current_home{};
    void *current_member{};
    std::int32_t current_id{};
    const auto unit_address = reinterpret_cast<std::uintptr_t>(unit);
    if (!home_read(unit, 0x328, current_region) || current_region != region ||
        !home_read(unit, 0x320, current_owner) || current_owner != owner ||
        !home_read(unit, 0x348, current_id) || current_id != family_id ||
        !home_read(region, 0x4e8, current_families) ||
        current_families.data != families.data ||
        current_families.count != families.count ||
        !home_read(family, 0, current_members) ||
        current_members.data != members.data ||
        current_members.count != members.count ||
        !home_read(family, 0x20, current_home) || current_home != home ||
        !home_read(members.data,
                   static_cast<std::size_t>(member_index) * sizeof(void *),
                   current_member) ||
        current_member != unit || (unit_address & 7) != 0 ||
        !home_memory_span(reinterpret_cast<void *>(unit_address + 0x340),
                          sizeof(void *), true)) {
      break;
    }
    void *previous{};
    __try {
      previous = InterlockedCompareExchangePointer(
          reinterpret_cast<void *volatile *>(unit_address + 0x340), home,
          nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      break;
    }
    if (previous == nullptr) {
      home_repaired.fetch_add(1, std::memory_order_relaxed);
      return home;
    }
    return previous;
  } while (false);
  // A truly homeless family legitimately has no home. The native loop tail
  // omits only level-2/3 equipment demand and continues the complete update.
  home_skipped.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

auto home_reference_guard_counters() -> HomeReferenceGuardCounters {
  return {home_repaired.load(std::memory_order_relaxed),
          home_skipped.load(std::memory_order_relaxed)};
}

static auto allocate_home_stub(const HomePatch &patch,
                               const std::uint8_t *image) -> std::uint8_t * {
  // Generated from analysis/20261011-crash-clRmkk/native/home-reference-stub.s.
  // Both audited sites have RSP % 16 == 0. The 0xb0-byte frame contains
  // 32-byte call shadow space, RDX/R8-R11, and XMM0-XMM5. RAX/RCX are the
  // overwritten native outputs; all other registers survive the callback.
  std::array<std::uint8_t, 225> code{
      0x48, 0x8b, 0x07, 0x48, 0x85, 0xc0, 0x74, 0x10, 0x48, 0x8b, 0x88, 0x40,
      0x03, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x0f, 0x85, 0x9f, 0x00, 0x00, 0x00,
      0x48, 0x81, 0xec, 0xb0, 0x00, 0x00, 0x00, 0x48, 0x89, 0x54, 0x24, 0x20,
      0x4c, 0x89, 0x44, 0x24, 0x28, 0x4c, 0x89, 0x4c, 0x24, 0x30, 0x4c, 0x89,
      0x54, 0x24, 0x38, 0x4c, 0x89, 0x5c, 0x24, 0x40, 0x0f, 0x11, 0x44, 0x24,
      0x50, 0x0f, 0x11, 0x4c, 0x24, 0x60, 0x0f, 0x11, 0x54, 0x24, 0x70, 0x0f,
      0x11, 0x9c, 0x24, 0x80, 0x00, 0x00, 0x00, 0x0f, 0x11, 0xa4, 0x24, 0x90,
      0x00, 0x00, 0x00, 0x0f, 0x11, 0xac, 0x24, 0xa0, 0x00, 0x00, 0x00, 0x48,
      0x89, 0xc1, 0xff, 0x15, 0x61, 0x00, 0x00, 0x00, 0x48, 0x89, 0xc1, 0x48,
      0x8b, 0x54, 0x24, 0x20, 0x4c, 0x8b, 0x44, 0x24, 0x28, 0x4c, 0x8b, 0x4c,
      0x24, 0x30, 0x4c, 0x8b, 0x54, 0x24, 0x38, 0x4c, 0x8b, 0x5c, 0x24, 0x40,
      0x0f, 0x10, 0x44, 0x24, 0x50, 0x0f, 0x10, 0x4c, 0x24, 0x60, 0x0f, 0x10,
      0x54, 0x24, 0x70, 0x0f, 0x10, 0x9c, 0x24, 0x80, 0x00, 0x00, 0x00, 0x0f,
      0x10, 0xa4, 0x24, 0x90, 0x00, 0x00, 0x00, 0x0f, 0x10, 0xac, 0x24, 0xa0,
      0x00, 0x00, 0x00, 0x48, 0x81, 0xc4, 0xb0, 0x00, 0x00, 0x00, 0x48, 0x85,
      0xc9, 0x74, 0x0c, 0x8b, 0x81, 0xf8, 0x0c, 0x00, 0x00, 0xff, 0x25, 0x0e,
      0x00, 0x00, 0x00, 0xff, 0x25, 0x10, 0x00, 0x00, 0x00, 0x11, 0x11, 0x11,
      0x11, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
      0x22, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33};
  const auto callback =
      reinterpret_cast<std::uintptr_t>(&guarded_home_reference);
  const auto resume = reinterpret_cast<std::uintptr_t>(
      image + patch.site + HOME_ORIGINAL_BYTES.size());
  const auto skip = reinterpret_cast<std::uintptr_t>(image + patch.skip);
  std::memcpy(code.data() + 0xc9, &callback, sizeof(callback));
  std::memcpy(code.data() + 0xd1, &resume, sizeof(resume));
  std::memcpy(code.data() + 0xd9, &skip, sizeof(skip));
  auto *stub = static_cast<std::uint8_t *>(VirtualAlloc(
      nullptr, code.size(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (stub == nullptr) {
    return nullptr;
  }
  std::memcpy(stub, code.data(), code.size());
  DWORD ignored{};
  if (!VirtualProtect(stub, code.size(), PAGE_EXECUTE_READ, &ignored) ||
      !FlushInstructionCache(GetCurrentProcess(), stub, code.size())) {
    VirtualFree(stub, 0, MEM_RELEASE);
    return nullptr;
  }
  return stub;
}

static auto replace_home_patch(HomePatch &patch, const bool install) -> bool {
  if (!install && !patch.installed) {
    return true;
  }
  auto *site = home_published_image + patch.site;
  const auto &expected = install ? HOME_ORIGINAL_BYTES : patch.jump;
  const auto &replacement = install ? patch.jump : HOME_ORIGINAL_BYTES;
  if (!home_memory_span(site, expected.size(), false, true) ||
      std::memcmp(site, expected.data(), expected.size()) != 0) {
    return false;
  }
  DWORD protection{};
  if (!VirtualProtect(site, expected.size(), PAGE_EXECUTE_READWRITE,
                      &protection)) {
    return false;
  }
  const bool unchanged =
      std::memcmp(site, expected.data(), expected.size()) == 0;
  if (unchanged) {
    std::memcpy(site, replacement.data(), replacement.size());
    patch.installed = install;
  }
  DWORD ignored{};
  const bool restored =
      VirtualProtect(site, expected.size(), protection, &ignored) != 0;
  const bool flushed =
      FlushInstructionCache(GetCurrentProcess(), site, expected.size()) != 0;
  return unchanged && restored && flushed;
}

auto remove_home_reference_guard() -> bool {
  if (home_published_image == nullptr) {
    return true;
  }
  bool restored = true;
  for (auto &patch : home_patches) {
    const bool result = replace_home_patch(patch, false);
    restored = result && restored;
  }
  return restored;
}

auto install_home_reference_guard(std::uint8_t *image, const std::size_t size)
    -> bool {
  if (image == nullptr || size < 0x09deb000 ||
      size > std::numeric_limits<std::uintptr_t>::max() -
                 reinterpret_cast<std::uintptr_t>(image) ||
      (home_published_image != nullptr && home_published_image != image)) {
    return false;
  }
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  if (!home_read(image, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < 0 ||
      static_cast<std::size_t>(dos.e_lfanew) > size - sizeof(nt) ||
      !home_read(image, static_cast<std::size_t>(dos.e_lfanew), nt) ||
      nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
      nt.FileHeader.TimeDateStamp != 0xb47dcd3f ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.OptionalHeader.SizeOfImage != 0x09deb000 ||
      nt.OptionalHeader.CheckSum != 0x09a1d1f2) {
    return false;
  }
  for (const auto &patch : home_patches) {
    const auto &expected = patch.installed ? patch.jump : HOME_ORIGINAL_BYTES;
    if (!home_memory_span(image + patch.site, expected.size(), false, true) ||
        std::memcmp(image + patch.site, expected.data(), expected.size()) !=
            0 ||
        !home_memory_span(image + patch.skip, HOME_LOOP_TAIL.size(), false,
                          true) ||
        std::memcmp(image + patch.skip, HOME_LOOP_TAIL.data(),
                    HOME_LOOP_TAIL.size()) != 0) {
      return false;
    }
  }
  for (const auto &fingerprint : HOME_FINGERPRINTS) {
    if (fingerprint.rva > size || fingerprint.size > size - fingerprint.rva ||
        !home_memory_span(image + fingerprint.rva, fingerprint.size, false,
                          true)) {
      return false;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t offset = 0; offset < fingerprint.size; ++offset) {
      const auto rva = fingerprint.rva + offset;
      auto value = image[rva];
      for (const auto &patch : home_patches) {
        if (patch.installed && rva >= patch.site &&
            rva - patch.site < HOME_ORIGINAL_BYTES.size()) {
          value = HOME_ORIGINAL_BYTES[rva - patch.site];
        }
      }
      hash = (hash ^ value) * 1099511628211ULL;
    }
    if (hash != fingerprint.fnv1a) {
      return false;
    }
  }
  if (home_patches[0].installed && home_patches[1].installed) {
    return true;
  }
  if (home_published_image == nullptr) {
    for (auto &patch : home_patches) {
      patch.stub = allocate_home_stub(patch, image);
      if (patch.stub == nullptr) {
        for (auto &allocated : home_patches) {
          if (allocated.stub != nullptr) {
            VirtualFree(allocated.stub, 0, MEM_RELEASE);
            allocated.stub = nullptr;
          }
        }
        return false;
      }
      patch.jump = {0xff, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x90};
      std::memcpy(patch.jump.data() + 6, &patch.stub, sizeof(patch.stub));
    }
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&guarded_home_reference),
                            &pinned)) {
      for (auto &patch : home_patches) {
        VirtualFree(patch.stub, 0, MEM_RELEASE);
        patch.stub = nullptr;
      }
      return false;
    }
    home_validation_image = image;
    home_validation_size = nt.OptionalHeader.SizeOfImage;
    // Publication is a startup-only operation. Retain all callback state,
    // executable stubs and the pinned module even after a partial rollback.
    home_published_image = image;
  }
  for (auto &patch : home_patches) {
    if (!patch.installed && !replace_home_patch(patch, true)) {
      remove_home_reference_guard();
      return false;
    }
  }
  return true;
}
