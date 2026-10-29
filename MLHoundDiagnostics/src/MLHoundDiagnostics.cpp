#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Diagnostics.hpp"
#include "NativePatch.hpp"
#include "UE4SSABI.hpp"

static constexpr std::int32_t maximum_string_length = 256;
static constexpr std::int32_t maximum_array_length = 4096;
static constexpr std::size_t maximum_hound_histories = 256;

struct SavedLastError {
  DWORD value = GetLastError();
  ~SavedLastError() { SetLastError(value); }
};

static auto read_memory(std::uintptr_t address, void *output,
                        std::size_t size) noexcept -> bool {
  SIZE_T copied{};
  return address != 0 &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), output,
                           size, &copied) &&
         copied == size;
}

template <typename T>
static auto read_value(std::uintptr_t address, T &output) noexcept -> bool {
  return read_memory(address, &output, sizeof(output));
}

static auto read_name(std::uintptr_t address) -> std::wstring {
  struct StringHeader {
    std::uintptr_t data;
    std::int32_t count, capacity;
  } value{};
  if (!read_value(address, value) || value.count < 1 ||
      value.count > maximum_string_length || value.capacity < value.count ||
      !value.data) {
    return L"<unknown>";
  }
  std::wstring result(static_cast<std::size_t>(value.count), L'\0');
  if (!read_memory(value.data, result.data(),
                   result.size() * sizeof(wchar_t)) ||
      result.back() != L'\0') {
    return L"<unknown>";
  }
  result.pop_back();
  // User-supplied display names must not create extra diagnostic log lines.
  std::replace_if(
      result.begin(), result.end(),
      [](wchar_t character) {
        return character < 0x20 || character == 0x7f || character == 0x2028 ||
               character == 0x2029;
      },
      L'?');
  return result.empty() ? L"<unnamed>" : result;
}

static auto log_message(std::wstring_view message) noexcept -> void {
  SavedLastError last_error;
  try {
    using Send = void (*)(std::wstring_view);
    const auto module = GetModuleHandleW(L"UE4SS.dll");
    if (!module)
      return;
    const auto send = reinterpret_cast<Send>(
        GetProcAddress(module, "?send@Output@RC@@YAXV?$basic_string_view@_WU?$"
                               "char_traits@_W@std@@@std@@@Z"));
    if (send)
      send(std::wstring(L"[MLHoundDiagnostics] ") + std::wstring(message) +
           L"\n");
  } catch (...) {
    // A diagnostic failure must never escape into a native game task.
  }
}

struct TaskState {
  std::uint8_t type = 255;
  std::int32_t resource{};
  std::uintptr_t target{};
};

static auto read_task(std::uintptr_t unit) noexcept -> TaskState {
  struct Header {
    std::uintptr_t data;
    std::int32_t count, capacity;
  } value{};
  TaskState task;
  if (!read_value(unit + 0x898, value) || value.count < 0 ||
      value.count > maximum_array_length || value.capacity < value.count)
    return task;
  if (value.count == 0) {
    task.type = 0;
    return task;
  }
  if (!value.data || !read_value(value.data, task.type) ||
      !read_value(value.data + 0x90, task.resource) ||
      !read_value(value.data + 0xa8, task.target))
    return {};
  return task;
}

struct DiagnosticRecord {
  DiagnosticEvent event{};
  std::uintptr_t unit{}, camp{}, owner{};
  std::array<std::uint32_t, 3> identity{};
  TaskState hound_task, owner_task;
  std::uint8_t current_role{}, close_to_final{}, owner_dead{};
  int owner_valid = -1;
  LogDecision decision;
  std::wstring name, hunter, region;
  std::optional<std::int64_t> meat, pelts;
};

using HistoryKey = std::array<std::uintptr_t, 4>;
static std::mutex history_mutex;
static std::map<HistoryKey, HoundHistory> histories;
static std::uintptr_t game_image{};
using OwnerValidation = bool (*)(void *, void *);
using AddTask = void (*)(void *, const void *);
using FinishTask = void (*)(void *);
static OwnerValidation native_owner_validation{};
static AddTask native_add_task{};
static FinishTask native_finish_task{};

static auto capture_record(void *unit_pointer, DiagnosticEvent event,
                           void *camp_hint = nullptr,
                           int owner_valid = -1) noexcept
    -> std::optional<DiagnosticRecord> {
  SavedLastError last_error;
  try {
    const auto unit = reinterpret_cast<std::uintptr_t>(unit_pointer);
    std::uint8_t assigned_role{};
    if (!unit || !read_value(unit + 0xab1, assigned_role) ||
        assigned_role != 14) {
      return std::nullopt;
    }
    DiagnosticRecord record;
    record.event = event;
    record.unit = unit;
    record.owner_valid = owner_valid;
    std::uintptr_t outer{};
    std::uint32_t flags{};
    if (!read_value(unit + 8, flags) || (flags & 0x40018000U) ||
        !read_value(unit + 0xc, record.identity[0]) ||
        !read_value(unit + 0x18, record.identity[1]) ||
        !read_value(unit + 0x1c, record.identity[2]) ||
        !read_value(unit + 0x20, outer))
      return std::nullopt;
    read_value(unit + 0xab0, record.current_role);
    read_value(unit + 0x338, record.camp);
    if (camp_hint)
      record.camp = reinterpret_cast<std::uintptr_t>(camp_hint);
    read_value(unit + 0xac8, record.owner);
    record.hound_task = read_task(unit);
    if (record.owner) {
      record.owner_task = read_task(record.owner);
      read_value(record.owner + 0x5a0, record.close_to_final);
      read_value(record.owner + 0x30c, record.owner_dead);
    }
    // Never hash position, stock totals or string allocation addresses.
    std::uint64_t signature = 14695981039346656037ULL;
    for (const std::uint64_t part :
         {static_cast<std::uint64_t>(record.camp),
          static_cast<std::uint64_t>(record.owner),
          static_cast<std::uint64_t>(record.owner_task.type),
          static_cast<std::uint64_t>(
              static_cast<std::uint32_t>(record.owner_task.resource)),
          static_cast<std::uint64_t>(owner_valid + 1),
          static_cast<std::uint64_t>(record.owner_dead)}) {
      signature = (signature ^ part) * 1099511628211ULL;
    }
    {
      std::lock_guard lock(history_mutex);
      const HistoryKey key{record.identity[0], record.identity[1],
                           record.identity[2], outer};
      if (!histories.contains(key) &&
          histories.size() >= maximum_hound_histories) {
        histories.clear();
      }
      record.decision =
          decide_log(histories[key], event, signature, GetTickCount64());
    }
    if (!record.decision.emit)
      return std::nullopt;
    record.name = read_name(unit + 0x2f0);
    record.hunter = record.owner ? read_name(record.owner + 0x2f0) : L"<none>";
    record.region = L"<unknown>";
    if (record.camp) {
      std::uintptr_t region{};
      if (read_value(record.camp + 0x2c8, region) && region) {
        record.region = read_name(region + 0x2b8);
      }
      struct InventoryHeader {
        std::uintptr_t data;
        std::int32_t count, capacity;
      } inventory{};
      if (read_value(record.camp + 0x448, inventory) && inventory.count >= 0 &&
          inventory.count <= maximum_array_length &&
          inventory.capacity >= inventory.count &&
          (inventory.count == 0 || inventory.data)) {
        std::int64_t meat{}, pelts{};
        bool readable = true;
        for (std::int32_t i = 0; i < inventory.count; ++i) {
          std::array<std::int32_t, 2> item{};
          if (!read_value(inventory.data + static_cast<std::uintptr_t>(i) * 24,
                          item)) {
            readable = false;
            break;
          }
          if (item[0] == 330)
            meat += item[1];
          if (item[0] == 9)
            pelts += item[1];
        }
        if (readable) {
          record.meat = meat;
          record.pelts = pelts;
        }
      }
    }
    return record;
  } catch (...) {
    return std::nullopt;
  }
}

static auto report_settings() noexcept -> void {
  SavedLastError last_error;
  try {
    std::uintptr_t type{}, defaults{};
    struct Header {
      std::uintptr_t data;
      std::int32_t count, capacity;
    } tasks{};
    std::vector<std::uint8_t> values;
    bool readable = game_image && read_value(game_image + 0x946acc0, type) &&
                    type && read_value(type + 0x110, defaults) && defaults &&
                    read_value(defaults + 0x38, tasks) && tasks.count >= 0 &&
                    tasks.count <= 32 && tasks.capacity >= tasks.count;
    if (readable && tasks.count) {
      values.resize(static_cast<std::size_t>(tasks.count));
      readable = read_memory(tasks.data, values.data(), values.size());
    }
    std::uint64_t signature = readable ? 2 : 1;
    std::wstring list;
    for (const auto task : values) {
      signature = signature * 257 + task;
      if (!list.empty())
        list += L",";
      list += std::to_wstring(task);
    }
    static std::atomic<std::uint64_t> reported{};
    if (reported.exchange(signature) == signature)
      return;
    if (!readable) {
      log_message(L"[SETTINGS] Runtime AllowedTasks not readable yet; will "
                  L"retry on later events.");
      return;
    }
    const bool allows_return =
        std::find(values.begin(), values.end(), 4) != values.end();
    log_message(L"[SETTINGS] AllowedTasks=[" + list +
                L"]; PickupGoods return stage=" +
                (allows_return ? L"allowed (0.8.111 hound fix present)."
                               : L"NOT allowed; hound return bonus may be "
                                 L"blocked by configuration."));
  } catch (...) {
  }
}

static auto emit_record(const std::optional<DiagnosticRecord> &value) noexcept
    -> void {
  if (!value)
    return;
  SavedLastError last_error;
  try {
    report_settings();
    const auto &record = *value;
    static constexpr std::array labels{L"STATE", L"FOLLOW_QUEUED",
                                       L"FOLLOW_ENDED_WITHOUT_BONUS",
                                       L"BONUS_GRANTED"};
    std::wstring message =
        L"[" + std::wstring(labels[static_cast<std::size_t>(record.event)]) +
        L"] hound=" + record.name + L" (object=" +
        std::to_wstring(record.identity[0]) + L", instance=" +
        std::to_wstring(record.identity[2]) + L"), region=" + record.region +
        L", camp=" + std::to_wstring(record.camp) + L", hunter=" +
        record.hunter + L", hound_task=" +
        std::to_wstring(record.hound_task.type) + L", hunter_task=" +
        std::to_wstring(record.owner_task.type) + L", carcass=" +
        std::to_wstring(record.owner_task.resource) +
        L", hunter_close_to_final=" + std::to_wstring(record.close_to_final);
    if (record.event == DiagnosticEvent::BonusCommitted) {
      message += L"; NATIVE BONUS COMMITTED: SmallGame +1, Pelts +1; "
                 L"post-bonus camp stock=" +
                 (record.meat ? std::to_wstring(*record.meat) : L"?") + L"/" +
                 (record.pelts ? std::to_wstring(*record.pelts) : L"?") +
                 L"; observed bonuses for this hound=" +
                 std::to_wstring(record.decision.bonuses);
    } else if (record.event == DiagnosticEvent::State) {
      message += L"; current owner valid=" +
                 std::to_wstring(record.owner_valid) +
                 L". No owner/idle can be normal; NEW owner requires same "
                 L"camp, Hunting/PlantOrTrap and <=50m.";
    } else if (record.event == DiagnosticEvent::FollowStopped) {
      message += L"; native Follow ended without entering its bonus branch; "
                 L"not by itself an error.";
    }
    if (record.decision.suppressed) {
      message += L"; identical records coalesced=" +
                 std::to_wstring(record.decision.suppressed);
    }
    log_message(message);
  } catch (...) {
  }
}

static auto owner_validation_hook(void *hound, void *camp) -> bool {
  const bool result = native_owner_validation(hound, camp);
  emit_record(
      capture_record(hound, DiagnosticEvent::State, camp, result ? 1 : 0));
  return result;
}

static auto follow_queued_hook(void *hound, const void *task) -> void {
  // Borrowed Task belongs to the native caller; do not alter or free it.
  native_add_task(hound, task);
  emit_record(capture_record(hound, DiagnosticEvent::FollowQueued));
}

static auto bonus_committed_hook(void *hound) -> void {
  // This CALL site occurs AFTER inventory and production records were updated.
  // Capture owner/task now: FinishTask clears tasks and the caller clears
  // owner.
  const auto record = capture_record(hound, DiagnosticEvent::BonusCommitted);
  native_finish_task(hound);
  emit_record(record);
}

static auto follow_stopped_hook(void *unit) -> void {
  // This site also handles other animals. Filter only diagnostics, never native
  // execution.
  const auto record = capture_record(unit, DiagnosticEvent::FollowStopped);
  native_finish_task(unit);
  emit_record(record);
}

struct HookSite {
  std::uintptr_t rva, context_rva;
  std::array<std::uint8_t, 37> context;
};
static constexpr std::array<HookSite, 4> hook_sites{
    {{0x4dabfd0, 0x4dabfc0, {0xf8, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0xa0, 0x04,
                             0x00, 0x00, 0x48, 0x8b, 0xd0, 0x49, 0x8b, 0xce,
                             0xe8, 0x5b, 0x3f, 0xfe, 0xff, 0x33, 0xf6, 0x84,
                             0xc0, 0x0f, 0x85, 0x3e, 0x01, 0x00, 0x00, 0x49,
                             0x39, 0xb6, 0xc8, 0x0a, 0x00}},
     {0x4dac283, 0x4dac273, {0xff, 0xff, 0x40, 0x88, 0xb5, 0xec, 0x00, 0x00,
                             0x00, 0x48, 0x89, 0xb5, 0xf0, 0x00, 0x00, 0x00,
                             0xe8, 0x68, 0xfc, 0x03, 0x00, 0x49, 0x8b, 0x96,
                             0xc8, 0x0a, 0x00, 0x00, 0x49, 0x8b, 0xce, 0xe8,
                             0xf9, 0xea, 0x03, 0x00, 0xb0}},
     {0x4d87341, 0x4d87331, {0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x05,
                             0xe8, 0xd2, 0x2a, 0x3a, 0xfc, 0x48, 0x8b, 0xcf,
                             0xe8, 0xea, 0x3f, 0x04, 0x00, 0x4c, 0x39, 0xbf,
                             0xc8, 0x0a, 0x00, 0x00, 0x74, 0x07, 0x4c, 0x89,
                             0xbf, 0xc8, 0x0a, 0x00, 0x00}},
     {0x4d87543, 0x4d87533, {0x8b, 0x4d, 0xa7, 0x48, 0x85, 0xc9, 0x74, 0x05,
                             0xe8, 0xd0, 0x28, 0x3a, 0xfc, 0x48, 0x8b, 0xcf,
                             0xe8, 0xe8, 0x3d, 0x04, 0x00, 0x48, 0x8b, 0xb4,
                             0x24, 0x30, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x9c,
                             0x24, 0x28, 0x01, 0x00, 0x00}}}};

static auto make_patches() -> std::vector<CodePatch> {
  const std::array callbacks{
      reinterpret_cast<const void *>(&owner_validation_hook),
      reinterpret_cast<const void *>(&follow_queued_hook),
      reinterpret_cast<const void *>(&bonus_committed_hook),
      reinterpret_cast<const void *>(&follow_stopped_hook)};
  std::vector<CodePatch> result;
  for (std::size_t index = 0; index < hook_sites.size(); ++index) {
    const auto &site = hook_sites[index];
    std::vector<std::uint8_t> expected(site.context.begin() + 16,
                                       site.context.begin() + 21);
    std::vector<std::uint8_t> stub;
    append_absolute_jump(stub, callbacks[index]);
    result.push_back({site.rva, std::move(expected), std::move(stub), true});
  }
  return result;
}

static auto validate_image(const std::uint8_t *image) -> bool {
  if (!image)
    return false;
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
      dos->e_lfanew < static_cast<LONG>(sizeof(*dos)) || dos->e_lfanew > 0x1000)
    return false;
  const auto *nt =
      reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt->FileHeader.TimeDateStamp != 0xb47dcd3f ||
      nt->OptionalHeader.SizeOfImage != 0x09deb000 ||
      nt->OptionalHeader.CheckSum != 0x09a1d1f2)
    return false;
  for (const auto &site : hook_sites) {
    if (std::memcmp(image + site.context_rva, site.context.data(),
                    site.context.size()) != 0)
      return false;
  }
  return true;
}

class MLHoundDiagnosticsMod final : public RC::CppUserModBase {
  PatchSet patches_;

public:
  MLHoundDiagnosticsMod() {
    ModName = L"MLHoundDiagnostics";
    ModVersion = L"0.1.0";
    ModDescription =
        L"Observes native hunting hound follow and committed bonus events.";
    ModAuthors = L"hurryman2212";
  }
  ~MLHoundDiagnosticsMod() override {
    if (!patches_.restore())
      log_message(L"Hook restoration failed; restart before continuing without "
                  L"this mod.");
  }
  auto on_unreal_init() -> void override {
    if (patches_.installed())
      return;
    auto *image = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
    if (!validate_image(image)) {
      log_message(L"Unsupported executable or modified hound code; no "
                  L"diagnostic hooks installed. Requires Steam 0.8.111.");
      return;
    }
    game_image = reinterpret_cast<std::uintptr_t>(image);
    native_owner_validation =
        reinterpret_cast<OwnerValidation>(game_image + 0x4d8ff30);
    native_add_task = reinterpret_cast<AddTask>(game_image + 0x4debef0);
    native_finish_task = reinterpret_cast<FinishTask>(game_image + 0x4dcb330);
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&owner_validation_hook),
                            &pinned) ||
        !patches_.install(image, make_patches())) {
      log_message(L"Diagnostic hook installation failed; restart if another "
                  L"mod prevents rollback.");
      return;
    }
    log_message(
        L"Installed 4 observational hooks for 0.8.111. Native calls/arguments "
        L"are preserved; no saves or gameplay data edited. BONUS_GRANTED means "
        L"the native +1 SmallGame/+1 Pelts branch actually completed. Paused "
        L"games need not emit events; idle is not a failure. Repeated "
        L"non-bonus events are coalesced.");
  }
};

extern "C" __declspec(dllexport) RC::CppUserModBase *start_mod() {
  return new MLHoundDiagnosticsMod();
}
extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase *mod) {
  delete mod;
}
