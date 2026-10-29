#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "UE4SSABI.hpp"

struct RoadOwnershipPatch {
  std::uintptr_t context_rva;
  std::span<const std::uint8_t> expected;
  std::size_t branch_offset;
  std::size_t branch_length;
};

// Manor Lords 0.8.111, Steam executable. Only Region::owner comparisons are
// intercepted. The original jumps and all non-ownership checks are preserved.
// SHA256: 35cf516d8c161c8ea64e801e81a90c0e5cb69f41e5581fa50e0dc2f210d33f4b.
static constexpr std::array<std::uint8_t, 24> DELETE_CONTEXT{
    0x48, 0x85, 0xc0, 0x74, 0x18, 0x48, 0x39, 0x98, 0x50, 0x03, 0x00, 0x00,
    0x75, 0x0f, 0x48, 0x8b, 0x93, 0x18, 0x08, 0x00, 0x00, 0x48, 0x8b, 0xcb};
static constexpr std::array<std::uint8_t, 23> POINTS_CONTEXT{
    0x48, 0x85, 0xc0, 0x74, 0x12, 0x48, 0x39, 0xb8, 0x50, 0x03, 0x00, 0x00,
    0x75, 0x1c, 0x44, 0x38, 0xb8, 0xe8, 0x02, 0x00, 0x00, 0x74, 0x13};
static constexpr std::array<std::uint8_t, 24> START_CONTEXT{
    0x4d, 0x85, 0xf6, 0x74, 0x70, 0x49, 0x39, 0xbe, 0x50, 0x03, 0x00, 0x00,
    0x75, 0x67, 0x49, 0x83, 0xbe, 0x38, 0x07, 0x00, 0x00, 0x00, 0x74, 0x5d};
static constexpr std::array<std::uint8_t, 22> COST_CONTEXT{
    0x48, 0x85, 0xc0, 0x0f, 0x84, 0x3d, 0x01, 0x00, 0x00, 0x48, 0x39,
    0xb8, 0x50, 0x03, 0x00, 0x00, 0x0f, 0x85, 0x30, 0x01, 0x00, 0x00};
static constexpr std::array<RoadOwnershipPatch, 4> ROAD_PATCHES{{
    {0x4aeff73, DELETE_CONTEXT, 12, 2},
    {0x4b42132, POINTS_CONTEXT, 12, 2},
    {0x4b42222, START_CONTEXT, 12, 2},
    {0x4b425c8, COST_CONTEXT, 16, 6},
}};
static constexpr std::array<std::uint8_t, 9> KINGS_ROAD_CHECK{
    0x44, 0x38, 0xb2, 0xb2, 0x02, 0x00, 0x00, 0x75, 0x33};

static auto log_message(std::wstring_view message) -> void {
  using SendLog = void (*)(std::wstring_view);
  const auto module = GetModuleHandleW(L"UE4SS.dll");
  if (module == nullptr) {
    return;
  }
  const auto send = reinterpret_cast<SendLog>(
      GetProcAddress(module, "?send@Output@RC@@YAXV?$basic_string_view@_WU?$"
                             "char_traits@_W@std@@@std@@@Z"));
  if (send != nullptr) {
    send(std::wstring(L"[MLAllRegionRoads] ") + std::wstring(message) + L"\n");
  }
}

static auto validate_image(const std::uint8_t *image) -> bool {
  if (image == nullptr) {
    return false;
  }
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(*dos) ||
      dos->e_lfanew > 0x1000) {
    return false;
  }
  const auto *nt =
      reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt->FileHeader.TimeDateStamp != 0xb47dcd3f ||
      nt->OptionalHeader.SizeOfImage != 0x09deb000 ||
      nt->OptionalHeader.CheckSum != 0x09a1d1f2) {
    return false;
  }
  for (const auto &patch : ROAD_PATCHES) {
    if (std::memcmp(image + patch.context_rva, patch.expected.data(),
                    patch.expected.size()) != 0) {
      return false;
    }
  }
  return std::memcmp(image + 0x4aeff54, KINGS_ROAD_CHECK.data(),
                     KINGS_ROAD_CHECK.size()) == 0;
}

static auto write_code(std::uint8_t *site, std::span<const std::uint8_t> bytes)
    -> bool {
  DWORD previous{};
  if (!VirtualProtect(site, bytes.size(), PAGE_EXECUTE_READWRITE, &previous)) {
    return false;
  }
  std::memcpy(site, bytes.data(), bytes.size());
  const bool flushed =
      FlushInstructionCache(GetCurrentProcess(), site, bytes.size());
  DWORD ignored{};
  const bool restored = VirtualProtect(site, bytes.size(), previous, &ignored);
  return flushed && restored;
}

static auto make_stub(std::uint8_t *code, const RoadOwnershipPatch &patch,
                      std::uint8_t *continuation) -> bool {
  // Reproduce the original comparison. An already-owned region retains exactly
  // its original permission. Otherwise admit only ordinary roads, not fences,
  // walls, region borders or streams. Road-building previews may be Dummy.
  std::array<std::uint8_t, 58> bytes{
      0,    0,    0,    0,    0,    0, 0,    // original CMP (7)
      0x74, 0x23,                            // JE continuation
      0x50,                                  // PUSH RAX
      0x48, 0x8b, 0x87, 0x78, 0x07, 0, 0,    // RAX = pawn.dummyRoad
      0x48, 0x85, 0xc0,                      // TEST RAX,RAX
      0x74, 0x12,                            // JZ denied
      0x80, 0xb8, 0xb4, 0x02, 0,    0, 0x01, // roadType <= Dummy
      0x77, 0x09,                            // JA denied
      0x80, 0xb8, 0xb5, 0x02, 0,    0, 0,    // wallType == None
      0xeb, 0x03,                            // JMP POP RAX
      0x83, 0xc8, 0x01,                      // denied: clear ZF
      0x58,                                  // POP RAX, preserves flags
      0xff, 0x25, 0,    0,    0,    0,       // indirect JMP continuation
      0,    0,    0,    0,    0,    0, 0,    0};
  std::memcpy(bytes.data(), patch.expected.data() + patch.branch_offset - 7, 7);
  if (patch.context_rva == 0x4aeff73) {
    // Deletion uses RBX as pawn and only accepts Regular, never Dummy.
    bytes[12] = 0x83;
    bytes[13] = 0x18;
    bytes[14] = 0x08;
    bytes[28] = 0;
  }
  std::memcpy(bytes.data() + 50, &continuation, sizeof(continuation));
  return write_code(code, bytes);
}

static auto allocate_near(const std::uint8_t *site) -> std::uint8_t * {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const auto origin = reinterpret_cast<std::uintptr_t>(site) &
                      ~(std::uintptr_t(info.dwAllocationGranularity) - 1);
  // Relative JMP permits a signed 32-bit distance. Search on allocation
  // boundaries without replacing any existing mapping.
  for (std::uintptr_t distance = info.dwAllocationGranularity;
       distance < 0x70000000; distance += info.dwAllocationGranularity) {
    for (const auto address : {origin + distance, origin - distance}) {
      auto *allocation = static_cast<std::uint8_t *>(
          VirtualAlloc(reinterpret_cast<void *>(address), 0x1000,
                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
      if (allocation != nullptr) {
        return allocation;
      }
    }
  }
  return nullptr;
}

class MLAllRegionRoadsMod final : public RC::CppUserModBase {
  std::uint8_t *image_{};
  std::uint8_t *stubs_{};
  std::size_t installed_count_{};

  auto uninstall() -> void {
    bool restored = true;
    while (installed_count_ > 0) {
      const auto &patch = ROAD_PATCHES[--installed_count_];
      if (!write_code(image_ + patch.context_rva + patch.branch_offset - 7,
                      patch.expected.subspan(patch.branch_offset - 7, 7))) {
        restored = false;
        log_message(L"Could not restore a road permission branch; restart the "
                    L"game before continuing.");
      }
    }
    // If restoring a branch failed, leave its target mapped rather than create
    // a dangling executable jump. Nothing in the stub calls this DLL.
    if (restored && stubs_ != nullptr) {
      VirtualFree(stubs_, 0, MEM_RELEASE);
    }
    stubs_ = nullptr;
  }

public:
  MLAllRegionRoadsMod() {
    ModName = L"MLAllRegionRoads";
    ModVersion = L"0.1.0";
    ModDescription = L"Allows road building and removal in other regions; "
                     L"preserves original non-ownership rules.";
    ModAuthors = L"hurryman2212";
  }

  ~MLAllRegionRoadsMod() override { uninstall(); }

  auto on_unreal_init() -> void override {
    if (installed_count_ != 0) {
      return;
    }
    image_ = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
    if (!validate_image(image_)) {
      log_message(L"Unsupported executable or modified road code; no patch was "
                  L"applied.");
      return;
    }
    stubs_ = allocate_near(image_ + ROAD_PATCHES[0].context_rva);
    if (stubs_ == nullptr) {
      log_message(
          L"Could not allocate road permission guards; no patch was applied.");
      return;
    }
    for (std::size_t index = 0; index < ROAD_PATCHES.size(); ++index) {
      const auto &patch = ROAD_PATCHES[index];
      if (!make_stub(stubs_ + index * 0x100, patch,
                     image_ + patch.context_rva + patch.branch_offset)) {
        uninstall();
        log_message(
            L"Could not prepare road permission guards; no patch was applied.");
        return;
      }
    }
    DWORD previous{};
    if (!VirtualProtect(stubs_, 0x1000, PAGE_EXECUTE_READ, &previous)) {
      uninstall();
      return;
    }
    for (std::size_t index = 0; index < ROAD_PATCHES.size(); ++index) {
      const auto &patch = ROAD_PATCHES[index];
      auto *site = image_ + patch.context_rva + patch.branch_offset - 7;
      std::array<std::uint8_t, 7> jump{0xe9, 0, 0, 0, 0, 0x90, 0x90};
      const auto displacement = static_cast<std::int32_t>(
          reinterpret_cast<std::intptr_t>(stubs_ + index * 0x100) -
          reinterpret_cast<std::intptr_t>(site + 5));
      std::memcpy(jump.data() + 1, &displacement, sizeof(displacement));
      // Track before writing so even a cache/protection failure rolls back.
      ++installed_count_;
      if (!write_code(site, jump)) {
        uninstall();
        log_message(
            L"Road patch installation failed; changes were rolled back.");
        return;
      }
    }
    log_message(L"Installed four road-only ownership gates for Manor Lords "
                L"0.8.111. King's Road protection, settlement, collision, "
                L"terrain and material costs remain unchanged.");
  }
};

extern "C" __declspec(dllexport) RC::CppUserModBase *start_mod() {
  return new MLAllRegionRoadsMod();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase *mod) {
  delete mod;
}
