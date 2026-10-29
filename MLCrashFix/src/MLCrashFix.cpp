#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "CacheUpdateGuard.hpp"
#include "HomeReferenceGuard.hpp"
#include "MeleeAssignmentGuard.hpp"
#include "UE4SSABI.hpp"

namespace {
constexpr ULONGLONG MELEE_REPORT_INTERVAL_MS = 5000;
constexpr ULONGLONG HOME_REPORT_INTERVAL_MS = 5000;

using SendLogFunction = void (*)(std::wstring_view);
using GetObjectNameFunction = void *(*)(const void *, void *);
using GetNativeObjectFunction = void *(*)(void *);
using GetNativeClassFunction = void *(*)();
using SpawnDecalsFunction = void (*)(void *);
using GetTargetPositionFunction = double *(*)(void *, double *);
using SetWorldLocationFunction = void (*)(void *, const double *, bool, void *,
                                          std::uint8_t);

constexpr std::string_view SEND_LOG_EXPORT =
    "?send@Output@RC@@YAXV?$basic_string_view@_WU?$char_traits@_W@std@@@std@@@"
    "Z";
constexpr std::string_view GET_OBJECT_NAME_EXPORT =
    "?GetName@UObject@Unreal@RC@@QEBA?AV?$basic_string@_WU?$char_traits@_W@std"
    "@@V?$allocator@_W@2@@std@@XZ";
constexpr DWORD EXPECTED_TIMESTAMP = 0x93E60B7F;
constexpr DWORD EXPECTED_IMAGE_SIZE = 0x09DE5000;
constexpr DWORD EXPECTED_CHECKSUM = 0x09A195C1;
constexpr DWORD BANNER_EXPECTED_TIMESTAMP = 0xB47DCD3F;
constexpr DWORD BANNER_EXPECTED_IMAGE_SIZE = 0x09DEB000;
constexpr DWORD BANNER_EXPECTED_CHECKSUM = 0x09A1D1F2;
constexpr std::uintptr_t BANNER_FAULT_RVA = 0x4b217e0;
constexpr std::uintptr_t BANNER_CALL_SEQUENCE_RVA = 0x4dc64bb;
constexpr std::uintptr_t BANNER_CALLER_RVA = 0x4dc64ce;
constexpr std::uintptr_t BANNER_RESUME_RVA = 0x4dc6503;
constexpr std::size_t PATCH_SIZE = 16;
constexpr std::size_t STUB_ALLOCATION_SIZE = 0x2000;
constexpr std::size_t COUNTER_OFFSET = 0x1000;
constexpr std::uintptr_t MARKETPLACE_FAULT_RVA = 0x4dbe83a;
constexpr std::uintptr_t MARKETPLACE_ABORT_RVA = 0x4dbef48;
constexpr std::uintptr_t POSITION_COMPONENT_FAULT_RVA = 0x4b17610;
constexpr std::uintptr_t POSITION_COMPONENT_SQUAD_FAULT_RVA = 0x4d5bb16;
constexpr std::uintptr_t POSITION_TRANSFORM_FAULT_RVA = 0x3792853;
constexpr std::uintptr_t POSITION_TRANSFORM_ARRAY_SEQUENCE_RVA = 0x4b1769e;
constexpr std::uintptr_t POSITION_TRANSFORM_ARRAY_RETURN_RVA = 0x4b176bb;
constexpr std::uintptr_t POSITION_TRANSFORM_FORMATION_SEQUENCE_RVA = 0x4ae5d72;
constexpr std::uintptr_t POSITION_TRANSFORM_FORMATION_RETURN_RVA = 0x4ae5d7e;
constexpr std::uintptr_t REFERENCE_TARGET_FAULT_RVA = 0x4c189f3;
constexpr std::uintptr_t REFERENCE_TARGET_RETURN_RVA = 0x4c18e48;
constexpr std::uintptr_t GET_WORLD_RVA = 0x34b3ae0;
constexpr std::uintptr_t GET_GAME_INSTANCE_RVA = 0x38b17e0;
constexpr std::uintptr_t GAME_INSTANCE_CLASS_RVA = 0x4a6e7e0;
constexpr std::uintptr_t SPAWN_DECALS_RVA = 0x4de7c90;
constexpr std::uintptr_t GET_TARGET_POSITION_RVA = 0x4dcad60;
constexpr std::uintptr_t SET_WORLD_LOCATION_RVA = 0x3792810;

constexpr std::array<std::uint8_t, 7> BANNER_FAULT_BYTES{0x8B, 0x81, 0xE8, 0x12,
                                                         0x00, 0x00, 0xC3};
// Covers the owner load, leaf getter call and complete heraldry assignment.
// The following native branch target resumes the rest of banner creation.
constexpr std::array<std::uint8_t, 72> BANNER_CALL_SEQUENCE_BYTES{
    0x48, 0x8B, 0x8E, 0x20, 0x03, 0x00, 0x00, 0x48, 0x8B, 0xBE, 0x00, 0x0D,
    0x00, 0x00, 0xE8, 0x12, 0xB3, 0xD5, 0xFF, 0x8B, 0xD0, 0x48, 0x8B, 0xCB,
    0xE8, 0x98, 0x7D, 0xE2, 0xFF, 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48,
    0x8D, 0x15, 0xAB, 0x8E, 0x1B, 0x03, 0x48, 0x8D, 0x8C, 0x24, 0xB0, 0x00,
    0x00, 0x00, 0x48, 0x8B, 0xD8, 0xE8, 0x9B, 0xC7, 0x48, 0xFC, 0x4C, 0x8B,
    0xC3, 0x48, 0x8B, 0xCF, 0x48, 0x8B, 0x10, 0xE8, 0x5D, 0x9E, 0xD0, 0xFE};
constexpr std::array<std::uint8_t, 25> BANNER_RESUME_BYTES{
    0x48, 0x8B, 0x86, 0x18, 0x03, 0x00, 0x00, 0x8B, 0x96,
    0xB8, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x88, 0xD0, 0x02,
    0x00, 0x00, 0xE8, 0x04, 0x02, 0xF8, 0xFF};

constexpr std::array<std::uint8_t, PATCH_SIZE> ORIGINAL_BYTES{
    0x48, 0x8B, 0x07, 0x48, 0x8B, 0x88, 0x40, 0x03,
    0x00, 0x00, 0x8B, 0x81, 0xF8, 0x0C, 0x00, 0x00};
constexpr std::array<std::uint8_t, 7> LOOP_TAIL_BYTES{0x48, 0x83, 0xC7, 0x08,
                                                      0x48, 0x3B, 0xFE};
constexpr std::array<std::uint8_t, 7> MARKETPLACE_FAULT_BYTES{
    0x49, 0x39, 0x87, 0x30, 0x10, 0x00, 0x00};
constexpr std::array<std::uint8_t, 10> MARKETPLACE_ABORT_BYTES{
    0x32, 0xC0, 0x48, 0x8B, 0x9C, 0x24, 0xE0, 0x01, 0x00, 0x00};
constexpr std::array<std::uint8_t, 7> POSITION_COMPONENT_FAULT_BYTES{
    0x0F, 0x10, 0xB1, 0xF0, 0x01, 0x00, 0x00};
// Includes the cached-component load and R15's reuse as the second array's
// count. The unit remains at *RDI when the following position read faults.
constexpr std::array<std::uint8_t, 26> POSITION_COMPONENT_SQUAD_SEQUENCE_BYTES{
    0x49, 0x8B, 0x87, 0x28, 0x04, 0x00, 0x00, 0x4C, 0x63,
    0x7D, 0xB8, 0x0F, 0x10, 0xB0, 0xF0, 0x01, 0x00, 0x00,
    0xF2, 0x0F, 0x10, 0xB8, 0x00, 0x02, 0x00, 0x00};
constexpr std::array<std::uint8_t, 7> POSITION_TRANSFORM_FAULT_BYTES{
    0x48, 0x8B, 0x89, 0xB0, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 29> POSITION_TRANSFORM_ARRAY_SEQUENCE_BYTES{
    0x49, 0x8B, 0x45, 0x00, 0x0F, 0x29, 0x45, 0x50, 0xF2, 0x0F,
    0x11, 0x4D, 0x60, 0x4A, 0x8B, 0x0C, 0x38, 0x48, 0x8B, 0x89,
    0x28, 0x04, 0x00, 0x00, 0xE8, 0x55, 0xB1, 0xC7, 0xFE};
constexpr std::array<std::uint8_t, 12>
    POSITION_TRANSFORM_FORMATION_SEQUENCE_BYTES{
        0x49, 0x8B, 0x8F, 0x28, 0x04, 0x00, 0x00, 0xE8, 0x92, 0xCA, 0xCA, 0xFE};
constexpr std::array<std::uint8_t, 7> REFERENCE_TARGET_FAULT_BYTES{
    0x48, 0x8B, 0x99, 0xE0, 0x01, 0x00, 0x00};
constexpr std::array<std::uint8_t, 15> REFERENCE_TARGET_RETURN_BYTES{
    0x48, 0x8B, 0x9C, 0x24, 0xD0, 0x00, 0x00, 0x00,
    0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 9> GET_WORLD_BYTES{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x41, 0x08};
constexpr std::array<std::uint8_t, 13> GET_GAME_INSTANCE_BYTES{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xD1,
    0x41, 0xB8, 0x01, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 14> GAME_INSTANCE_CLASS_BYTES{
    0x4C, 0x8B, 0xDC, 0x48, 0x83, 0xEC, 0x78,
    0x48, 0x8B, 0x05, 0x42, 0x1A, 0x9F, 0x04};
constexpr std::array<std::uint8_t, 18> SPAWN_DECALS_BYTES{
    0x48, 0x8B, 0xC4, 0x53, 0x48, 0x81, 0xEC, 0xA0, 0x00,
    0x00, 0x00, 0x80, 0xB9, 0x40, 0x04, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 16> GET_TARGET_POSITION_BYTES{
    0x48, 0x63, 0x81, 0xE0, 0x07, 0x00, 0x00, 0x85,
    0xC0, 0x78, 0x18, 0x3B, 0x81, 0xD8, 0x07, 0x00};
// Fingerprint the entire prologue through the fault, including every stack
// adjustment: its caller return address is at fault-time RSP + 0x1B8.
constexpr std::array<std::uint8_t, 67> SET_WORLD_LOCATION_BYTES{
    0x4C, 0x8B, 0xDC, 0x53, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x98, 0x01,
    0x00, 0x00, 0x41, 0x0F, 0x29, 0x73, 0xC8, 0x41, 0x0F, 0x29, 0x7B, 0xB8,
    0x45, 0x0F, 0x29, 0x43, 0xA8, 0x45, 0x0F, 0x29, 0x4B, 0x98, 0x45, 0x0F,
    0x29, 0xB3, 0x48, 0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0x05, 0xBF, 0xF1, 0x99,
    0x05, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x84, 0x24, 0xF0, 0x00, 0x00, 0x00,
    0x44, 0x0F, 0x10, 0x32, 0x48, 0x8B, 0xD9};

struct PatchSpec {
  std::uintptr_t site_rva;
  std::uintptr_t skip_rva;
};

constexpr std::array<PatchSpec, 2> PATCH_SPECS{{
    {0x04BD29D0, 0x04BD2B25},
    {0x04BD40E0, 0x04BD4225},
}};

struct Capture {
  volatile LONG64 total_count{};
  volatile LONG64 recovered_count{};
  volatile LONG64 unrecoverable_count{};
  volatile LONG64 object{};
  volatile LONG64 slot{};
  volatile LONG64 end{};
  volatile LONG64 recovered_home{};
};

struct MarketplaceCapture {
  volatile LONG64 total_count{};
  volatile LONG64 unit{};
  volatile LONG64 region{};
  volatile LONG64 candidate{};
};

struct PositionComponentCapture {
  volatile LONG64 total_count{};
  volatile LONG64 location_read_count{};
  volatile LONG64 squad_read_count{};
  volatile LONG64 transform_call_count{};
  volatile LONG64 formation_transform_count{};
  volatile LONG64 object{};
  volatile LONG64 root_component{};
};

struct ReferenceTargetCapture {
  volatile LONG64 total_count{};
  volatile LONG64 related_building{};
};

struct BannerCapture {
  volatile LONG64 total_count{};
  volatile LONG64 unit{};
};

struct InstalledPatch {
  std::uint8_t *site{};
  void *allocation{};
  Capture *capture{};
  void *reported_object{};
};

struct RawWideString {
  std::array<std::byte, 16> storage{};
  std::uint64_t size{};
  std::uint64_t capacity{};
};

static_assert(sizeof(RawWideString) == 32);

SendLogFunction send_log{};
GetObjectNameFunction get_object_name{};
GetNativeObjectFunction native_get_world{};
GetNativeObjectFunction native_get_game_instance{};
GetNativeClassFunction native_game_instance_class{};
SpawnDecalsFunction native_spawn_decals{};
GetTargetPositionFunction native_get_target_position{};
SetWorldLocationFunction native_set_world_location{};
thread_local bool rebuilding_decals{};
std::uint8_t *image_begin{};
std::uint8_t *image_end{};
MarketplaceCapture marketplace_capture{};
PositionComponentCapture position_component_capture{};
ReferenceTargetCapture reference_target_capture{};
std::uint8_t *banner_image_begin{};
BannerCapture banner_capture{};

auto log_message(const std::wstring_view message) -> void {
  std::wstring line{L"[MLCrashFix] "};
  line.append(message);
  line.push_back(L'\n');
  if (send_log != nullptr) {
    send_log(line);
  } else {
    OutputDebugStringW(line.c_str());
  }
}

auto resolve_ue4ss_api() -> void {
  const HMODULE ue4ss = GetModuleHandleW(L"UE4SS.dll");
  if (ue4ss == nullptr) {
    return;
  }
  const std::string export_name{SEND_LOG_EXPORT};
  send_log = reinterpret_cast<SendLogFunction>(
      GetProcAddress(ue4ss, export_name.c_str()));
  const std::string object_name_export{GET_OBJECT_NAME_EXPORT};
  get_object_name = reinterpret_cast<GetObjectNameFunction>(
      GetProcAddress(ue4ss, object_name_export.c_str()));
}

auto is_readable(const void *address, const std::size_t size) -> bool {
  if (address == nullptr || size == 0) {
    return false;
  }
  MEMORY_BASIC_INFORMATION information{};
  if (VirtualQuery(address, &information, sizeof(information)) == 0 ||
      information.State != MEM_COMMIT ||
      (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return false;
  }
  const auto *begin = static_cast<const std::uint8_t *>(address);
  const auto *region_begin =
      static_cast<const std::uint8_t *>(information.BaseAddress);
  return begin >= region_begin &&
         size <= static_cast<std::size_t>(region_begin +
                                          information.RegionSize - begin);
}

auto is_writable(const void *address, const std::size_t size) -> bool {
  if (address == nullptr || size == 0) {
    return false;
  }
  MEMORY_BASIC_INFORMATION information{};
  if (VirtualQuery(address, &information, sizeof(information)) == 0 ||
      information.State != MEM_COMMIT ||
      (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
      (information.Protect &
       (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
        PAGE_EXECUTE_WRITECOPY)) == 0) {
    return false;
  }
  const auto *begin = static_cast<const std::uint8_t *>(address);
  const auto *region_begin =
      static_cast<const std::uint8_t *>(information.BaseAddress);
  return begin >= region_begin &&
         size <= static_cast<std::size_t>(region_begin +
                                          information.RegionSize - begin);
}

auto read_pointer(const void *address) -> void * {
  void *value{};
  if (is_readable(address, sizeof(value))) {
    std::memcpy(&value, address, sizeof(value));
  }
  return value;
}

auto repair_position_component(void *object) -> void * {
  if (rebuilding_decals || object == nullptr || !is_readable(object, 0x7E8)) {
    return nullptr;
  }
  struct RepairScope {
    RepairScope() { rebuilding_decals = true; }
    ~RepairScope() { rebuilding_decals = false; }
  } repair_scope{};
  auto *bytes = static_cast<std::uint8_t *>(object);
  void *root_component = read_pointer(bytes + 0x1B8);
  void *existing = read_pointer(bytes + 0x428);
  if (existing != nullptr) {
    return existing != root_component && is_readable(existing, 0x208) &&
                   read_pointer(static_cast<std::uint8_t *>(existing) + 0x20) ==
                       object
               ? existing
               : nullptr;
  }
  // +0x420/+0x428 are owned squad decals. Death/cleanup destroys this pair;
  // substituting Actor::RootComponent here would let that cleanup destroy the
  // actor's root. Rebuild only a completely absent pair using native creation.
  if (native_get_world == nullptr || native_get_game_instance == nullptr ||
      native_game_instance_class == nullptr || native_spawn_decals == nullptr ||
      native_get_target_position == nullptr ||
      native_set_world_location == nullptr || bytes[0x30C] != 0 ||
      bytes[0x440] != 0 || read_pointer(bytes + 0x420) != nullptr ||
      !is_writable(bytes, 0x660) ||
      (root_component != nullptr && !is_readable(root_component, 0x208)) ||
      !is_readable(read_pointer(bytes + 0x318), 0x2E0)) {
    return nullptr;
  }

  // getTargetPos reads Path[PathPoint].Pos or finalLocation.Pos. Validate its
  // backing storage before asking the game, and preserve the movement target
  // across spawnDecals (which initializes decals at the current position).
  std::int32_t path_count{};
  std::int32_t path_point{};
  std::memcpy(&path_count, bytes + 0x7D8, sizeof(path_count));
  std::memcpy(&path_point, bytes + 0x7E0, sizeof(path_point));
  if (path_count < 0) {
    return nullptr;
  }
  if (path_point >= 0 && path_point < path_count) {
    auto *path = static_cast<std::uint8_t *>(read_pointer(bytes + 0x7D0));
    if (path == nullptr ||
        !is_readable(path + static_cast<std::size_t>(path_point) * 0x88 + 8,
                     3 * sizeof(double))) {
      return nullptr;
    }
  }
  std::array<double, 3> target{};
  native_get_target_position(object, target.data());
  for (const double coordinate : target) {
    if (!std::isfinite(coordinate)) {
      return nullptr;
    }
  }

  void *world = native_get_world(object);
  if (world == nullptr) {
    return nullptr;
  }
  void *game_instance = native_get_game_instance(world);
  if (!is_readable(game_instance, 0x1F0) ||
      !is_readable(
          read_pointer(static_cast<std::uint8_t *>(game_instance) + 0x1E8),
          0x28)) {
    return nullptr;
  }
  auto *expected_class =
      static_cast<std::uint8_t *>(native_game_instance_class());
  auto *actual_class = static_cast<std::uint8_t *>(
      read_pointer(static_cast<std::uint8_t *>(game_instance) + 0x10));
  if (!is_readable(expected_class, 0x3C) || !is_readable(actual_class, 0x3C)) {
    return nullptr;
  }
  std::int32_t expected_index{};
  std::int32_t actual_index{};
  std::memcpy(&expected_index, expected_class + 0x38, sizeof(expected_index));
  std::memcpy(&actual_index, actual_class + 0x38, sizeof(actual_index));
  auto *class_chain =
      static_cast<std::uint8_t *>(read_pointer(actual_class + 0x30));
  if (expected_index < 0 || actual_index < expected_index ||
      class_chain == nullptr ||
      read_pointer(class_chain + static_cast<std::size_t>(expected_index) *
                                     sizeof(void *)) != expected_class + 0x30) {
    return nullptr;
  }

  native_spawn_decals(object);
  void *origin_decal = read_pointer(bytes + 0x420);
  void *target_decal = read_pointer(bytes + 0x428);
  if (bytes[0x440] == 0 || origin_decal == target_decal ||
      origin_decal == root_component || target_decal == root_component ||
      !is_readable(origin_decal, 0x208) || !is_readable(target_decal, 0x208) ||
      read_pointer(static_cast<std::uint8_t *>(origin_decal) + 0x20) !=
          object ||
      read_pointer(static_cast<std::uint8_t *>(target_decal) + 0x20) !=
          object) {
    return nullptr;
  }
  native_set_world_location(target_decal, target.data(), false, nullptr, 0);
  return target_decal;
}

auto is_in_game_image(const void *address) -> bool {
  const auto *value = static_cast<const std::uint8_t *>(address);
  return value >= image_begin && value < image_end;
}

auto read_object_name(const void *object) -> std::optional<std::wstring> {
  if (get_object_name == nullptr || !is_readable(object, 0x28)) {
    return std::nullopt;
  }

  const auto *bytes = static_cast<const std::uint8_t *>(object);
  void *vtable{};
  void *object_class{};
  std::int32_t internal_index{};
  std::memcpy(&vtable, bytes, sizeof(vtable));
  std::memcpy(&internal_index, bytes + 0xC, sizeof(internal_index));
  std::memcpy(&object_class, bytes + 0x10, sizeof(object_class));
  if (!is_in_game_image(vtable) || internal_index <= 0 ||
      internal_index > 10'000'000 || !is_readable(object_class, 0x28)) {
    return std::nullopt;
  }

  void *class_vtable{};
  std::memcpy(&class_vtable, object_class, sizeof(class_vtable));
  if (!is_in_game_image(class_vtable)) {
    return std::nullopt;
  }

  RawWideString raw_name{};
  get_object_name(object, &raw_name);
  if (raw_name.size == 0 || raw_name.size > 256 ||
      raw_name.capacity < raw_name.size) {
    return std::nullopt;
  }

  const wchar_t *name{};
  if (raw_name.capacity <= 7) {
    name = reinterpret_cast<const wchar_t *>(raw_name.storage.data());
  } else {
    std::memcpy(&name, raw_name.storage.data(), sizeof(name));
  }
  if (!is_readable(name, (raw_name.size + 1) * sizeof(wchar_t))) {
    return std::nullopt;
  }

  // GetName allocates with UE4SS's CRT. This diagnostic intentionally retains
  // at most one short name per distinct invalid object instead of freeing that
  // allocation through this mod's private static CRT.
  return std::wstring{name, static_cast<std::size_t>(raw_name.size)};
}

auto has_expected_image_fingerprint(const std::uint8_t *image) -> bool {
  if (image == nullptr) {
    return false;
  }
  const auto *dos_header = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
  if (dos_header->e_magic != IMAGE_DOS_SIGNATURE || dos_header->e_lfanew <= 0) {
    return false;
  }
  const auto *nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
      image + dos_header->e_lfanew);
  return nt_headers->Signature == IMAGE_NT_SIGNATURE &&
         nt_headers->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
         nt_headers->FileHeader.TimeDateStamp == EXPECTED_TIMESTAMP &&
         nt_headers->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
         nt_headers->OptionalHeader.SizeOfImage == EXPECTED_IMAGE_SIZE &&
         nt_headers->OptionalHeader.CheckSum == EXPECTED_CHECKSUM;
}

auto has_bytes(const void *address,
               const std::span<const std::uint8_t> expected) -> bool {
  return std::memcmp(address, expected.data(), expected.size()) == 0;
}

auto has_banner_image_fingerprint(const std::uint8_t *image) -> bool {
  if (!is_readable(image, sizeof(IMAGE_DOS_HEADER))) {
    return false;
  }
  const auto *dos_header = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
  if (dos_header->e_magic != IMAGE_DOS_SIGNATURE ||
      dos_header->e_lfanew < static_cast<LONG>(sizeof(*dos_header)) ||
      dos_header->e_lfanew > 0x1000 ||
      !is_readable(image + dos_header->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) {
    return false;
  }
  const auto *nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
      image + dos_header->e_lfanew);
  return nt_headers->Signature == IMAGE_NT_SIGNATURE &&
         nt_headers->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
         nt_headers->FileHeader.TimeDateStamp == BANNER_EXPECTED_TIMESTAMP &&
         nt_headers->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
         nt_headers->OptionalHeader.SizeOfImage == BANNER_EXPECTED_IMAGE_SIZE &&
         nt_headers->OptionalHeader.CheckSum == BANNER_EXPECTED_CHECKSUM;
}

auto has_banner_code(const std::uint8_t *image) -> bool {
  return image != nullptr &&
         is_readable(image + BANNER_FAULT_RVA, BANNER_FAULT_BYTES.size()) &&
         is_readable(image + BANNER_CALL_SEQUENCE_RVA,
                     BANNER_CALL_SEQUENCE_BYTES.size()) &&
         is_readable(image + BANNER_RESUME_RVA, BANNER_RESUME_BYTES.size()) &&
         has_bytes(image + BANNER_FAULT_RVA, BANNER_FAULT_BYTES) &&
         has_bytes(image + BANNER_CALL_SEQUENCE_RVA,
                   BANNER_CALL_SEQUENCE_BYTES) &&
         has_bytes(image + BANNER_RESUME_RVA, BANNER_RESUME_BYTES);
}

auto has_expected_loop(const std::uint8_t *site, const std::uint8_t *skip)
    -> bool {
  if (!has_bytes(skip, LOOP_TAIL_BYTES)) {
    return false;
  }
  if (skip[7] != 0x0F || skip[8] != 0x85) {
    return false;
  }
  std::int32_t relative_target{};
  std::memcpy(&relative_target, skip + 9, sizeof(relative_target));
  return skip + 13 + relative_target == site;
}

auto write_executable_memory(void *destination,
                             const std::span<const std::uint8_t> bytes)
    -> bool {
  DWORD previous_protection{};
  if (VirtualProtect(destination, bytes.size(), PAGE_EXECUTE_READWRITE,
                     &previous_protection) == 0) {
    return false;
  }
  std::memcpy(destination, bytes.data(), bytes.size());
  FlushInstructionCache(GetCurrentProcess(), destination, bytes.size());
  DWORD ignored_protection{};
  return VirtualProtect(destination, bytes.size(), previous_protection,
                        &ignored_protection) != 0;
}

auto make_stub(std::uint8_t *allocation, const std::uint8_t *resume,
               const std::uint8_t *skip) -> Capture * {
  // When SMUnit::Home is null, recover it from the authoritative
  // Region.workerFamilies[workerFamilyID].familyHome value. The guards also
  // require that the unit is still a member of that family and that the home
  // belongs to the same region and owner. If any invariant fails, retain the
  // crash-prevention fallback and skip only this array element.
  std::array<std::uint8_t, 339> stub{
      0x48, 0x8B, 0x07, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x01, 0x01, 0x00, 0x00,
      0x48, 0x8B, 0x88, 0x40, 0x03, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x0F, 0x85,
      0xD0, 0x00, 0x00, 0x00, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0xF0,
      0x48, 0xFF, 0x05, 0xD5, 0x0F, 0x00, 0x00, 0x48, 0x89, 0x05, 0xE6, 0x0F,
      0x00, 0x00, 0x48, 0x89, 0x3D, 0xE7, 0x0F, 0x00, 0x00, 0x48, 0x89, 0x35,
      0xE8, 0x0F, 0x00, 0x00, 0x48, 0xC7, 0x05, 0xE5, 0x0F, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x8B, 0x90, 0x48, 0x03, 0x00, 0x00, 0x85, 0xD2, 0x0F,
      0x88, 0x9F, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0x80, 0x28, 0x03, 0x00, 0x00,
      0x4D, 0x85, 0xC0, 0x0F, 0x84, 0x8F, 0x00, 0x00, 0x00, 0x41, 0x3B, 0x90,
      0xF0, 0x04, 0x00, 0x00, 0x0F, 0x83, 0x82, 0x00, 0x00, 0x00, 0x4D, 0x8B,
      0x88, 0xE8, 0x04, 0x00, 0x00, 0x4D, 0x85, 0xC9, 0x74, 0x76, 0x48, 0x6B,
      0xD2, 0x50, 0x49, 0x01, 0xD1, 0x4D, 0x8B, 0x11, 0x41, 0x8B, 0x51, 0x08,
      0x4D, 0x85, 0xD2, 0x74, 0x63, 0x85, 0xD2, 0x7E, 0x5F, 0x49, 0x39, 0x02,
      0x74, 0x0A, 0x49, 0x83, 0xC2, 0x08, 0xFF, 0xCA, 0x75, 0xF3, 0xEB, 0x50,
      0x49, 0x8B, 0x49, 0x20, 0x48, 0x85, 0xC9, 0x74, 0x47, 0x4C, 0x39, 0x81,
      0xC8, 0x02, 0x00, 0x00, 0x75, 0x3E, 0x48, 0x8B, 0x90, 0x20, 0x03, 0x00,
      0x00, 0x48, 0x85, 0xD2, 0x74, 0x32, 0x48, 0x39, 0x91, 0xE0, 0x02, 0x00,
      0x00, 0x75, 0x29, 0x48, 0x89, 0x88, 0x40, 0x03, 0x00, 0x00, 0x48, 0x89,
      0x0D, 0x53, 0x0F, 0x00, 0x00, 0xF0, 0x48, 0xFF, 0x05, 0x23, 0x0F, 0x00,
      0x00, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x8B, 0x81, 0xF8, 0x0C,
      0x00, 0x00, 0xFF, 0x25, 0x4B, 0x00, 0x00, 0x00, 0xF0, 0x48, 0xFF, 0x05,
      0x10, 0x0F, 0x00, 0x00, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0xFF,
      0x25, 0x3E, 0x00, 0x00, 0x00, 0xF0, 0x48, 0xFF, 0x05, 0xEB, 0x0E, 0x00,
      0x00, 0xF0, 0x48, 0xFF, 0x05, 0xF3, 0x0E, 0x00, 0x00, 0x48, 0x89, 0x05,
      0xF4, 0x0E, 0x00, 0x00, 0x48, 0x89, 0x3D, 0xF5, 0x0E, 0x00, 0x00, 0x48,
      0x89, 0x35, 0xF6, 0x0E, 0x00, 0x00, 0x48, 0xC7, 0x05, 0xF3, 0x0E, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x25, 0x08, 0x00, 0x00, 0x00, 0x11,
      0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x22,
      0x22, 0x22, 0x22};
  constexpr std::size_t RESUME_ADDRESS_OFFSET = 0x143;
  constexpr std::size_t SKIP_ADDRESS_OFFSET = 0x14B;
  std::memcpy(stub.data() + RESUME_ADDRESS_OFFSET, &resume, sizeof(resume));
  std::memcpy(stub.data() + SKIP_ADDRESS_OFFSET, &skip, sizeof(skip));
  std::memcpy(allocation, stub.data(), stub.size());

  auto *capture = reinterpret_cast<Capture *>(allocation + COUNTER_OFFSET);
  *capture = {};

  DWORD previous_protection{};
  if (VirtualProtect(allocation, 0x1000, PAGE_EXECUTE_READ,
                     &previous_protection) == 0) {
    return nullptr;
  }
  FlushInstructionCache(GetCurrentProcess(), allocation, stub.size());
  return capture;
}

auto make_jump(const void *stub) -> std::array<std::uint8_t, PATCH_SIZE> {
  std::array<std::uint8_t, PATCH_SIZE> jump{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
                                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                            0x00, 0x00, 0x90, 0x90};
  std::memcpy(jump.data() + 6, &stub, sizeof(stub));
  return jump;
}

LONG CALLBACK handle_ai_state_exception(EXCEPTION_POINTERS *exception) {
  if (exception == nullptr || exception->ExceptionRecord == nullptr ||
      exception->ContextRecord == nullptr || image_begin == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const EXCEPTION_RECORD &record = *exception->ExceptionRecord;
  CONTEXT &context = *exception->ContextRecord;
  if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
      record.NumberParameters < 2 || record.ExceptionInformation[0] != 0) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  if (record.ExceptionAddress == image_begin + MARKETPLACE_FAULT_RVA &&
      record.ExceptionInformation[1] == 0x1030 && context.R15 == 0) {
    InterlockedExchange64(&marketplace_capture.unit,
                          static_cast<LONG64>(context.Rdi));
    InterlockedExchange64(&marketplace_capture.region,
                          static_cast<LONG64>(context.R8));
    InterlockedExchange64(&marketplace_capture.candidate,
                          static_cast<LONG64>(context.R14));
    InterlockedIncrement64(&marketplace_capture.total_count);
    context.Rip =
        reinterpret_cast<DWORD64>(image_begin + MARKETPLACE_ABORT_RVA);
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  if (record.ExceptionAddress == image_begin + POSITION_COMPONENT_FAULT_RVA &&
      record.ExceptionInformation[1] == 0x1F0 && context.Rcx == 0 &&
      context.Rsi != 0) {
    void *object = read_pointer(reinterpret_cast<void *>(context.Rsi));
    void *component = repair_position_component(object);
    if (component == nullptr) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    InterlockedExchange64(&position_component_capture.object,
                          reinterpret_cast<LONG64>(object));
    InterlockedExchange64(&position_component_capture.root_component,
                          reinterpret_cast<LONG64>(component));
    InterlockedIncrement64(&position_component_capture.location_read_count);
    InterlockedIncrement64(&position_component_capture.total_count);
    context.Rcx = reinterpret_cast<DWORD64>(component);
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  if (record.ExceptionAddress == image_begin + POSITION_TRANSFORM_FAULT_RVA &&
      context.Rip == reinterpret_cast<DWORD64>(record.ExceptionAddress) &&
      record.ExceptionInformation[1] == 0xB0 && context.Rcx == 0 &&
      context.Rbx == 0) {
    if (!is_readable(reinterpret_cast<void *>(context.Rsp), 0x1C0)) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    void *caller = read_pointer(reinterpret_cast<void *>(context.Rsp + 0x1B8));
    void *object{};
    const bool formation_call =
        caller == image_begin + POSITION_TRANSFORM_FORMATION_RETURN_RVA;
    if (formation_call) {
      // Slot assignment keeps the unit itself in R15; R13 is a scalar index.
      // Never interpret that scalar as the array pointer used by the other
      // caller, even though both faults occur inside SetWorldLocation.
      object = reinterpret_cast<void *>(context.R15);
    } else if (caller == image_begin + POSITION_TRANSFORM_ARRAY_RETURN_RVA &&
               context.R15 % sizeof(void *) == 0 &&
               is_readable(reinterpret_cast<void *>(context.R13), 16)) {
      auto *array = reinterpret_cast<std::uint8_t *>(context.R13);
      std::int32_t count{};
      std::memcpy(&count, array + 8, sizeof(count));
      auto *objects = static_cast<std::uint8_t *>(read_pointer(array));
      if (count > 0 &&
          context.R15 / sizeof(void *) < static_cast<std::uint64_t>(count) &&
          objects != nullptr) {
        object = read_pointer(objects + context.R15);
      }
    } else {
      // This engine setter has many callers with unrelated register layouts.
      return EXCEPTION_CONTINUE_SEARCH;
    }
    void *component = repair_position_component(object);
    if (component == nullptr) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    InterlockedExchange64(&position_component_capture.object,
                          reinterpret_cast<LONG64>(object));
    InterlockedExchange64(&position_component_capture.root_component,
                          reinterpret_cast<LONG64>(component));
    InterlockedIncrement64(&position_component_capture.transform_call_count);
    if (formation_call) {
      InterlockedIncrement64(
          &position_component_capture.formation_transform_count);
    }
    InterlockedIncrement64(&position_component_capture.total_count);
    // Retry the setter with its original RDX, XMM14, stack and destination.
    // This completes the new formation order after recreating the decals;
    // skipping the setter would strand the unit at its previous target.
    context.Rbx = reinterpret_cast<DWORD64>(component);
    context.Rcx = reinterpret_cast<DWORD64>(component);
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  if (record.ExceptionAddress ==
          image_begin + POSITION_COMPONENT_SQUAD_FAULT_RVA &&
      context.Rip == reinterpret_cast<DWORD64>(record.ExceptionAddress) &&
      record.ExceptionInformation[1] == 0x1F0 && context.Rax == 0 &&
      context.Rdi != 0 && context.Rdi % sizeof(void *) == 0 &&
      context.R12 > context.Rdi &&
      (context.R12 - context.Rdi) % sizeof(void *) == 0) {
    void *object = read_pointer(reinterpret_cast<void *>(context.Rdi));
    void *component = repair_position_component(object);
    if (component == nullptr) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    InterlockedExchange64(&position_component_capture.object,
                          reinterpret_cast<LONG64>(object));
    InterlockedExchange64(&position_component_capture.root_component,
                          reinterpret_cast<LONG64>(component));
    InterlockedIncrement64(&position_component_capture.squad_read_count);
    InterlockedIncrement64(&position_component_capture.total_count);

    // Squad position matching has already appended this unit's realPos vector
    // to the first array. Retry both component-position loads so its second
    // array and subsequent per-member assignment retain the same indexing.
    context.Rax = reinterpret_cast<DWORD64>(component);
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  if (record.ExceptionAddress == image_begin + REFERENCE_TARGET_FAULT_RVA &&
      record.ExceptionInformation[1] == 0x1E0 && context.Rcx == 0 &&
      context.R12 == 0) {
    InterlockedExchange64(&reference_target_capture.related_building,
                          static_cast<LONG64>(context.R15));
    InterlockedIncrement64(&reference_target_capture.total_count);
    context.Rip =
        reinterpret_cast<DWORD64>(image_begin + REFERENCE_TARGET_RETURN_RVA);
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK handle_banner_exception(EXCEPTION_POINTERS *exception) {
  if (exception == nullptr || exception->ExceptionRecord == nullptr ||
      exception->ContextRecord == nullptr || banner_image_begin == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const EXCEPTION_RECORD &record = *exception->ExceptionRecord;
  CONTEXT &context = *exception->ContextRecord;
  const auto image = reinterpret_cast<std::uintptr_t>(banner_image_begin);
  if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
      (record.ExceptionFlags & EXCEPTION_NONCONTINUABLE) != 0 ||
      record.NumberParameters < 2 || record.ExceptionInformation[0] != 0 ||
      record.ExceptionInformation[1] != 0x12E8 ||
      record.ExceptionAddress != banner_image_begin + BANNER_FAULT_RVA ||
      context.Rip != image + BANNER_FAULT_RVA || context.Rcx != 0 ||
      context.Rsp % 16 != 8 || context.Rsi == 0 ||
      context.Rsi % alignof(void *) != 0) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  SIZE_T copied{};
  std::uintptr_t caller{};
  if (!ReadProcessMemory(GetCurrentProcess(),
                         reinterpret_cast<const void *>(context.Rsp), &caller,
                         sizeof(caller), &copied) ||
      copied != sizeof(caller) || caller != image + BANNER_CALLER_RVA) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  // Only the banner-bearing SMUnit in this exact native caller is eligible.
  // Saved dead state is restored later during loading, so do not inspect it.
  std::array<std::uint8_t, 0x948> unit{};
  if (!ReadProcessMemory(GetCurrentProcess(),
                         reinterpret_cast<const void *>(context.Rsi),
                         unit.data(), unit.size(), &copied) ||
      copied != unit.size()) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  std::uintptr_t vtable{};
  std::uintptr_t object_class{};
  std::uintptr_t owner{};
  std::uint32_t flags{};
  std::int32_t index{};
  std::int32_t equipment{};
  std::memcpy(&vtable, unit.data(), sizeof(vtable));
  std::memcpy(&flags, unit.data() + 8, sizeof(flags));
  std::memcpy(&index, unit.data() + 0xC, sizeof(index));
  std::memcpy(&object_class, unit.data() + 0x10, sizeof(object_class));
  std::memcpy(&owner, unit.data() + 0x320, sizeof(owner));
  std::memcpy(&equipment, unit.data() + 0x944, sizeof(equipment));
  if (vtable < image || vtable >= image + BANNER_EXPECTED_IMAGE_SIZE ||
      (flags & 0x40018000U) != 0 || index <= 0 || owner != 0 ||
      equipment != 157 || object_class == 0) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  std::uintptr_t class_vtable{};
  if (!ReadProcessMemory(GetCurrentProcess(),
                         reinterpret_cast<const void *>(object_class),
                         &class_vtable, sizeof(class_vtable), &copied) ||
      copied != sizeof(class_vtable) || class_vtable < image ||
      class_vtable >= image + BANNER_EXPECTED_IMAGE_SIZE) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  InterlockedExchange64(&banner_capture.unit, static_cast<LONG64>(context.Rsi));
  InterlockedIncrement64(&banner_capture.total_count);
  // The getter is a stackless leaf. Pop only its return address and rejoin
  // the native path after armsTexture assignment. Do not fabricate an owner,
  // abort the equipment/load operation, or alter any unit or save data.
  context.Rsp += sizeof(std::uintptr_t);
  context.Rip = image + BANNER_RESUME_RVA;
  return EXCEPTION_CONTINUE_EXECUTION;
}

class MLCrashFixMod final : public RC::CppUserModBase {
public:
  MLCrashFixMod() {
    ModName = L"MLCrashFix";
    ModVersion = L"0.1.0";
    ModDescription =
        L"Version-scoped AI recovery for 0.8.110 and home, banner, "
        L"cache-update and elevated-combat recovery for 0.8.111.";
    ModAuthors = L"hurryman2212";
  }

  ~MLCrashFixMod() override { uninstall(); }

  auto on_unreal_init() -> void override {
    resolve_ue4ss_api();
    install();
  }

  auto on_update() -> void override {
    if (home_guard_installed_) {
      const auto counts = home_reference_guard_counters();
      const auto now = GetTickCount64();
      if ((counts.repaired != home_guard_repaired_reported_ ||
           counts.skipped != home_guard_skipped_reported_) &&
          (home_guard_last_report_ == 0 ||
           now - home_guard_last_report_ >= HOME_REPORT_INTERVAL_MS)) {
        home_guard_repaired_reported_ = counts.repaired;
        home_guard_skipped_reported_ = counts.skipped;
        home_guard_last_report_ = now;
        log_message(
            L"Safe AI equipment-demand updates: missing unit homes recovered=" +
            std::to_wstring(counts.repaired) +
            L", units without a validated home-level contribution=" +
            std::to_wstring(counts.skipped) +
            L". Existing family homes and homeless state are retained; "
            L"remaining region updates and AI processing continue.");
      }
    }
    if (melee_guard_installed_) {
      const auto counts = melee_assignment_guard_counters();
      if (counts.transposed != melee_guard_reported_ ||
          counts.rejected != melee_guard_rejected_reported_) {
        const auto now = GetTickCount64();
        if (melee_guard_last_report_ == 0 ||
            now - melee_guard_last_report_ >= MELEE_REPORT_INTERVAL_MS) {
          melee_guard_reported_ = counts.transposed;
          melee_guard_rejected_reported_ = counts.rejected;
          melee_guard_last_report_ = now;
          log_message(L"Safe elevated-combat assignments: rectangular groups=" +
                      std::to_wstring(counts.transposed) +
                      L", additional fighters assigned valid nearest targets=" +
                      std::to_wstring(counts.reassigned) +
                      L", rejected invalid data=" +
                      std::to_wstring(counts.rejected) +
                      L". Native movement and combat continue after target "
                      L"assignment.");
        }
      }
    }
    if (cache_guard_installed_) {
      const auto counts = cache_update_guard_counters();
      if (counts.deferred != cache_guard_reported_ ||
          counts.graph_deferred != cache_graph_reported_) {
        cache_guard_reported_ = counts.deferred;
        cache_graph_reported_ = counts.graph_deferred;
        log_message(
            L"Safe load-time cache deferrals: active worker=" +
            std::to_wstring(counts.deferred) +
            L", graph awaiting obstacle updates=" +
            std::to_wstring(counts.graph_deferred) +
            L". Both queues are retained for the ordinary native cache tick; "
            L"no obstacle or graph result was discarded.");
      }
    }
    if (banner_image_begin != nullptr && exception_handler_ != nullptr) {
      const LONG64 total =
          InterlockedCompareExchange64(&banner_capture.total_count, 0, 0);
      if (total != 0 && total != banner_reported_count_) {
        banner_reported_count_ = total;
        const auto unit = static_cast<std::uintptr_t>(
            InterlockedCompareExchange64(&banner_capture.unit, 0, 0));
        std::wostringstream message{};
        message << L"Skipped ownerless banner heraldry; native equipment and "
                   L"loading continue (total "
                << total << L", unit=0x" << std::hex << unit
                << L"). No owner or save data was changed.";
        log_message(message.str());
      }
      return;
    }
    for (std::size_t index{}; index < installed_count_; ++index) {
      InstalledPatch &patch = patches_[index];
      const LONG64 total_count =
          InterlockedCompareExchange64(&patch.capture->total_count, 0, 0);
      void *object = reinterpret_cast<void *>(
          InterlockedCompareExchange64(&patch.capture->object, 0, 0));
      if (total_count == 0 || object == patch.reported_object) {
        continue;
      }
      patch.reported_object = object;
      const LONG64 recovered_count =
          InterlockedCompareExchange64(&patch.capture->recovered_count, 0, 0);
      const LONG64 unrecoverable_count = InterlockedCompareExchange64(
          &patch.capture->unrecoverable_count, 0, 0);
      void *slot = reinterpret_cast<void *>(
          InterlockedCompareExchange64(&patch.capture->slot, 0, 0));
      void *end = reinterpret_cast<void *>(
          InterlockedCompareExchange64(&patch.capture->end, 0, 0));
      void *recovered_home = reinterpret_cast<void *>(
          InterlockedCompareExchange64(&patch.capture->recovered_home, 0, 0));

      std::wostringstream message{};
      if (recovered_home != nullptr) {
        message << L"Restored SMUnit::Home from its Region family at guard ";
      } else {
        message << L"Prevented null dereference but could not validate a Home "
                   L"recovery at guard ";
      }
      message << index + 1 << L" (total " << total_count << L", recovered "
              << recovered_count << L", unrecoverable " << unrecoverable_count
              << L", object=0x" << std::hex
              << reinterpret_cast<std::uintptr_t>(object) << L", slot=0x"
              << reinterpret_cast<std::uintptr_t>(slot) << L", end=0x"
              << reinterpret_cast<std::uintptr_t>(end);
      if (const auto name = read_object_name(object); name.has_value()) {
        message << L", UObject=" << *name;
      }
      if (recovered_home != nullptr) {
        message << L", Home=0x"
                << reinterpret_cast<std::uintptr_t>(recovered_home);
        if (const auto name = read_object_name(recovered_home);
            name.has_value()) {
          message << L", HomeUObject=" << *name;
        }
      }
      message << L").";
      log_message(message.str());
    }

    if (exception_handler_ == nullptr) {
      return;
    }
    const LONG64 marketplace_total =
        InterlockedCompareExchange64(&marketplace_capture.total_count, 0, 0);
    if (marketplace_total != 0 &&
        marketplace_total != marketplace_reported_count_) {
      marketplace_reported_count_ = marketplace_total;

      void *unit = reinterpret_cast<void *>(
          InterlockedCompareExchange64(&marketplace_capture.unit, 0, 0));
      void *region = reinterpret_cast<void *>(
          InterlockedCompareExchange64(&marketplace_capture.region, 0, 0));
      void *candidate = reinterpret_cast<void *>(
          InterlockedCompareExchange64(&marketplace_capture.candidate, 0, 0));
      void *individual_workplace{};
      void *home{};
      std::int32_t family_id{-1};
      if (is_readable(unit, 0x350)) {
        const auto *bytes = static_cast<const std::uint8_t *>(unit);
        std::memcpy(&individual_workplace, bytes + 0x338,
                    sizeof(individual_workplace));
        std::memcpy(&home, bytes + 0x340, sizeof(home));
        std::memcpy(&family_id, bytes + 0x348, sizeof(family_id));
      }

      std::wostringstream message{};
      message << L"Cancelled a marketplace task with no resolvable assigned "
                 L"workplace before partial task creation (total "
              << marketplace_total << L", unit=0x" << std::hex
              << reinterpret_cast<std::uintptr_t>(unit) << L", region=0x"
              << reinterpret_cast<std::uintptr_t>(region)
              << L", individualWorkplace=0x"
              << reinterpret_cast<std::uintptr_t>(individual_workplace)
              << L", home=0x" << reinterpret_cast<std::uintptr_t>(home)
              << L", candidate=0x"
              << reinterpret_cast<std::uintptr_t>(candidate) << std::dec
              << L", familyID=" << family_id;
      if (const auto name = read_object_name(unit); name.has_value()) {
        message << L", UObject=" << *name;
      }
      if (const auto name = read_object_name(region); name.has_value()) {
        message << L", RegionUObject=" << *name;
      }
      if (const auto name = read_object_name(home); name.has_value()) {
        message << L", HomeUObject=" << *name;
      }
      message << L").";
      log_message(message.str());
    }

    const LONG64 position_total = InterlockedCompareExchange64(
        &position_component_capture.total_count, 0, 0);
    if (position_total != 0 && position_total != position_reported_count_) {
      position_reported_count_ = position_total;
      const LONG64 location_read_count = InterlockedCompareExchange64(
          &position_component_capture.location_read_count, 0, 0);
      const LONG64 squad_read_count = InterlockedCompareExchange64(
          &position_component_capture.squad_read_count, 0, 0);
      const LONG64 transform_call_count = InterlockedCompareExchange64(
          &position_component_capture.transform_call_count, 0, 0);
      const LONG64 formation_transform_count = InterlockedCompareExchange64(
          &position_component_capture.formation_transform_count, 0, 0);
      void *object = reinterpret_cast<void *>(InterlockedCompareExchange64(
          &position_component_capture.object, 0, 0));
      void *root_component =
          reinterpret_cast<void *>(InterlockedCompareExchange64(
              &position_component_capture.root_component, 0, 0));

      std::wostringstream message{};
      message << L"Recreated missing squad decals with native spawnDecals and "
                 L"restored the unit's movement target (total "
              << position_total << L", object=0x" << std::hex
              << reinterpret_cast<std::uintptr_t>(object) << L", targetDecal=0x"
              << reinterpret_cast<std::uintptr_t>(root_component) << std::dec
              << L", locationReads=" << location_read_count << L", squadReads="
              << squad_read_count << L", transformCalls="
              << transform_call_count << L", formationTransforms="
              << formation_transform_count;
      if (const auto name = read_object_name(object); name.has_value()) {
        message << L", UObject=" << *name;
      }
      if (const auto name = read_object_name(root_component);
          name.has_value()) {
        message << L", TargetDecalUObject=" << *name;
      }
      message << L").";
      log_message(message.str());
    }

    const LONG64 reference_total = InterlockedCompareExchange64(
        &reference_target_capture.total_count, 0, 0);
    if (reference_total != 0 && reference_total != reference_reported_count_) {
      reference_reported_count_ = reference_total;
      void *related_building =
          reinterpret_cast<void *>(InterlockedCompareExchange64(
              &reference_target_capture.related_building, 0, 0));

      std::wostringstream message{};
      message << L"Skipped a reference-map update whose owning container "
                 L"lookup returned null before mutation (total "
              << reference_total << L", relatedBuilding=0x" << std::hex
              << reinterpret_cast<std::uintptr_t>(related_building);
      if (const auto name = read_object_name(related_building);
          name.has_value()) {
        message << L", BuildingUObject=" << *name;
      }
      message << L").";
      log_message(message.str());
    }
  }

private:
  std::array<InstalledPatch, PATCH_SPECS.size()> patches_{};
  std::size_t installed_count_{};
  void *exception_handler_{};
  LONG64 marketplace_reported_count_{};
  LONG64 position_reported_count_{};
  LONG64 reference_reported_count_{};
  LONG64 banner_reported_count_{};
  bool cache_guard_installed_{};
  std::uint64_t cache_guard_reported_{};
  std::uint64_t cache_graph_reported_{};
  bool melee_guard_installed_{};
  bool home_guard_installed_{};
  std::uint64_t home_guard_repaired_reported_{};
  std::uint64_t home_guard_skipped_reported_{};
  ULONGLONG home_guard_last_report_{};
  std::uint64_t melee_guard_reported_{};
  std::uint64_t melee_guard_rejected_reported_{};
  ULONGLONG melee_guard_last_report_{};

  auto install() -> void {
    if (installed_count_ != 0 || exception_handler_ != nullptr ||
        cache_guard_installed_ || melee_guard_installed_ ||
        home_guard_installed_) {
      return;
    }

    auto *image = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
    if (has_banner_image_fingerprint(image)) {
      home_guard_installed_ =
          install_home_reference_guard(image, BANNER_EXPECTED_IMAGE_SIZE);
      if (home_guard_installed_) {
        log_message(
            L"Installed 0.8.111 AI home-reference guards in both equipment "
            L"demand loops. Recover missing unit Home only from its validated "
            L"family home; otherwise omit only that unit's home-level armor "
            L"contribution. Native AI updates continue without cancelling "
            L"tasks or inventing homes.");
      } else {
        log_message(L"0.8.111 home-reference guard validation/installation "
                    L"failed; this null-home crash is not protected. Other "
                    L"independently validated guards remain available.");
      }
      melee_guard_installed_ =
          install_melee_assignment_guard(image, BANNER_EXPECTED_IMAGE_SIZE);
      if (melee_guard_installed_) {
        log_message(
            L"Installed 0.8.111 elevated-combat rectangular-assignment guard: "
            L"preserves native matching for valid shapes and assigns surplus "
            L"fighters valid targets before native movement/combat "
            L"processing.");
      } else {
        log_message(
            L"0.8.111 elevated-combat guard validation/installation failed; "
            L"the new rectangular-assignment crash is not protected. Existing "
            L"banner and collision-cache protections are independent.");
      }
      cache_guard_installed_ =
          install_cache_update_guard(image, BANNER_EXPECTED_IMAGE_SIZE);
      if (cache_guard_installed_) {
        log_message(
            L"Installed 0.8.111 load-time collision-cache synchronization "
            L"guard: native readiness checks cover pathfinding and plot-snap "
            L"workers. Downstream graph work waits for obstacle queues to "
            L"drain. Busy updates are deferred, never dropped.");
      } else {
        log_message(L"0.8.111 collision-cache guard validation/installation "
                    L"failed; both protections could not be enabled. Fully "
                    L"restart before retrying this mod.");
      }
      if (!has_banner_code(image)) {
        log_message(
            L"0.8.111 banner code validation failed; no banner handler or "
            L"legacy patch was applied.");
        return;
      }
      banner_capture = {};
      banner_reported_count_ = 0;
      banner_image_begin = image;
      exception_handler_ =
          AddVectoredExceptionHandler(1, handle_banner_exception);
      if (exception_handler_ == nullptr) {
        banner_image_begin = nullptr;
        log_message(L"Could not install the 0.8.111 banner exception guard; "
                    L"the separate collision-cache guard is unaffected.");
        return;
      }
      log_message(L"Installed the exact-caller ownerless banner heraldry "
                  L"guard for Manor Lords 0.8.111. Native equipment and save "
                  L"loading continue; 0.8.110 legacy guards are not applied.");
      return;
    }
    if (!has_expected_image_fingerprint(image)) {
      log_message(L"Unsupported executable; no patch was applied.");
      return;
    }
    image_begin = image;
    image_end = image + EXPECTED_IMAGE_SIZE;

    for (const PatchSpec &spec : PATCH_SPECS) {
      std::uint8_t *site = image + spec.site_rva;
      std::uint8_t *skip = image + spec.skip_rva;
      if (!has_bytes(site, ORIGINAL_BYTES) || !has_expected_loop(site, skip)) {
        log_message(L"Code validation failed; no patch was applied.");
        uninstall();
        return;
      }

      auto *allocation = static_cast<std::uint8_t *>(
          VirtualAlloc(nullptr, STUB_ALLOCATION_SIZE, MEM_COMMIT | MEM_RESERVE,
                       PAGE_READWRITE));
      if (allocation == nullptr) {
        log_message(L"Could not allocate a guard; no patch was applied.");
        uninstall();
        return;
      }

      Capture *capture = make_stub(allocation, site + PATCH_SIZE, skip);
      if (capture == nullptr) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        log_message(L"Could not protect a guard; no patch was applied.");
        uninstall();
        return;
      }

      const auto jump = make_jump(allocation);
      if (!write_executable_memory(site, jump)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        log_message(L"Could not install a guard; changes were rolled back.");
        uninstall();
        return;
      }
      patches_[installed_count_++] = {
          .site = site,
          .allocation = allocation,
          .capture = capture,
      };
    }

    std::uint8_t *marketplace_fault = image + MARKETPLACE_FAULT_RVA;
    std::uint8_t *marketplace_abort = image + MARKETPLACE_ABORT_RVA;
    std::uint8_t *position_component_fault =
        image + POSITION_COMPONENT_FAULT_RVA;
    std::uint8_t *position_component_squad_sequence =
        image + POSITION_COMPONENT_SQUAD_FAULT_RVA - 11;
    std::uint8_t *position_transform_fault =
        image + POSITION_TRANSFORM_FAULT_RVA;
    std::uint8_t *reference_target_fault = image + REFERENCE_TARGET_FAULT_RVA;
    std::uint8_t *reference_target_return = image + REFERENCE_TARGET_RETURN_RVA;
    if (!has_bytes(marketplace_fault, MARKETPLACE_FAULT_BYTES) ||
        !has_bytes(marketplace_abort, MARKETPLACE_ABORT_BYTES) ||
        !has_bytes(position_component_fault, POSITION_COMPONENT_FAULT_BYTES) ||
        !has_bytes(position_component_squad_sequence,
                   POSITION_COMPONENT_SQUAD_SEQUENCE_BYTES) ||
        !has_bytes(position_transform_fault, POSITION_TRANSFORM_FAULT_BYTES) ||
        !has_bytes(image + POSITION_TRANSFORM_ARRAY_SEQUENCE_RVA,
                   POSITION_TRANSFORM_ARRAY_SEQUENCE_BYTES) ||
        !has_bytes(image + POSITION_TRANSFORM_FORMATION_SEQUENCE_RVA,
                   POSITION_TRANSFORM_FORMATION_SEQUENCE_BYTES) ||
        !has_bytes(reference_target_fault, REFERENCE_TARGET_FAULT_BYTES) ||
        !has_bytes(reference_target_return, REFERENCE_TARGET_RETURN_BYTES) ||
        !has_bytes(image + GET_WORLD_RVA, GET_WORLD_BYTES) ||
        !has_bytes(image + GET_GAME_INSTANCE_RVA, GET_GAME_INSTANCE_BYTES) ||
        !has_bytes(image + GAME_INSTANCE_CLASS_RVA,
                   GAME_INSTANCE_CLASS_BYTES) ||
        !has_bytes(image + SPAWN_DECALS_RVA, SPAWN_DECALS_BYTES) ||
        !has_bytes(image + GET_TARGET_POSITION_RVA,
                   GET_TARGET_POSITION_BYTES) ||
        !has_bytes(image + SET_WORLD_LOCATION_RVA, SET_WORLD_LOCATION_BYTES)) {
      log_message(
          L"AI exception code validation failed; no patch was applied.");
      uninstall();
      return;
    }
    native_get_world =
        reinterpret_cast<GetNativeObjectFunction>(image + GET_WORLD_RVA);
    native_get_game_instance = reinterpret_cast<GetNativeObjectFunction>(
        image + GET_GAME_INSTANCE_RVA);
    native_game_instance_class = reinterpret_cast<GetNativeClassFunction>(
        image + GAME_INSTANCE_CLASS_RVA);
    native_spawn_decals =
        reinterpret_cast<SpawnDecalsFunction>(image + SPAWN_DECALS_RVA);
    native_get_target_position = reinterpret_cast<GetTargetPositionFunction>(
        image + GET_TARGET_POSITION_RVA);
    native_set_world_location = reinterpret_cast<SetWorldLocationFunction>(
        image + SET_WORLD_LOCATION_RVA);
    marketplace_capture = {};
    position_component_capture = {};
    reference_target_capture = {};
    marketplace_reported_count_ = 0;
    position_reported_count_ = 0;
    reference_reported_count_ = 0;
    exception_handler_ =
        AddVectoredExceptionHandler(1, handle_ai_state_exception);
    if (exception_handler_ == nullptr) {
      log_message(L"Could not install the AI state exception guard; "
                  L"changes were rolled back.");
      uninstall();
      return;
    }

    log_message(L"Installed two SMUnit::Home recovery guards and one "
                L"AI state exception handler with five guarded faults and "
                L"caller-aware formation recovery for "
                L"Manor Lords 0.8.110.");
  }

  auto uninstall() -> void {
    if (home_guard_installed_) {
      if (!remove_home_reference_guard()) {
        log_message(L"Home-reference hook changed externally; retained its "
                    L"pinned callbacks. Fully restart the game.");
      }
      home_guard_installed_ = false;
    }
    if (melee_guard_installed_) {
      if (!remove_melee_assignment_guard()) {
        log_message(L"Elevated-combat hook changed externally; retained its "
                    L"pinned guard. Fully restart before changing mods.");
      }
      melee_guard_installed_ = false;
    }
    if (cache_guard_installed_) {
      if (!uninstall_cache_update_guard()) {
        log_message(L"Collision-cache hook changed externally; retained its "
                    L"pinned guard. Fully restart before changing mods.");
      }
      cache_guard_installed_ = false;
    }
    if (exception_handler_ != nullptr) {
      RemoveVectoredExceptionHandler(exception_handler_);
      exception_handler_ = nullptr;
    }
    banner_image_begin = nullptr;
    banner_capture = {};
    banner_reported_count_ = 0;
    native_get_world = nullptr;
    native_get_game_instance = nullptr;
    native_game_instance_class = nullptr;
    native_spawn_decals = nullptr;
    native_get_target_position = nullptr;
    native_set_world_location = nullptr;
    while (installed_count_ != 0) {
      InstalledPatch &patch = patches_[--installed_count_];
      write_executable_memory(patch.site, ORIGINAL_BYTES);
      VirtualFree(patch.allocation, 0, MEM_RELEASE);
      patch = {};
    }
  }
};
} // namespace

#define ML_CRASH_FIX_API __declspec(dllexport)

extern "C" {
ML_CRASH_FIX_API RC::CppUserModBase *start_mod() { return new MLCrashFixMod(); }

ML_CRASH_FIX_API void uninstall_mod(RC::CppUserModBase *mod) { delete mod; }
}
