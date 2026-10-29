#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <Windows.h>

#include "CityHooks.hpp"
#include "CityRuntime.hpp"
#include "ExtensionCancel.hpp"
#include "Retinue.hpp"
#include "UE4SSABI.hpp"
#include "Upgrade.hpp"
#include "Windmill.hpp"

static auto city_log(std::wstring_view message) -> void {
  using Send = void (*)(std::wstring_view);
  const auto module = GetModuleHandleW(L"UE4SS.dll");
  const auto send = module
                        ? reinterpret_cast<Send>(GetProcAddress(
                              module, "?send@Output@RC@@YAXV?$basic_string_"
                                      "view@_WU?$char_traits@_W@std@@@std@@@Z"))
                        : nullptr;
  if (send)
    send(std::wstring(L"[MLCityEditor] ") + std::wstring(message) + L"\n");
}

using CityLua = RC::LuaMadeSimple::Lua;
using CityLuaCallback = int (*)(const CityLua &);
using CityLuaRegister = void (*)(const CityLua *, const std::string &,
                                 const CityLuaCallback &);
using CityLuaSetBool = void (*)(const CityLua *, bool);
using CityLuaGetInteger = std::int64_t (*)(const CityLua *, std::int32_t);
using CityIsGameThread = bool (*)();
static CityLuaSetBool city_lua_set_bool{};
static CityLuaGetInteger city_lua_get_integer{};
static CityIsGameThread city_is_game_thread{};

static auto city_lua_can_cancel_artisan(const CityLua &lua) -> int {
  if (city_lua_set_bool == nullptr)
    return 0;
  bool allowed{};
  try {
    if (city_lua_get_integer && city_is_game_thread && city_is_game_thread()) {
      const auto address = city_lua_get_integer(&lua, 1);
      allowed = address > 0 &&
                city_can_cancel_artisan(reinterpret_cast<void *>(address));
    }
  } catch (...) {
    // A malformed UI query cannot grant permission or cross the Lua ABI.
  }
  city_lua_set_bool(&lua, allowed);
  return 1;
}

static auto city_register_cancel_query(CityLua &lua) -> void {
  const auto module = GetModuleHandleW(L"UE4SS.dll");
  if (module == nullptr)
    return;
  const auto register_function =
      reinterpret_cast<CityLuaRegister>(GetProcAddress(
          module, "?register_function@Lua@LuaMadeSimple@RC@@QEBAXAEBV?"
                  "$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@"
                  "std@@AEBQ6AHAEBV123@@Z@Z"));
  city_lua_set_bool = reinterpret_cast<CityLuaSetBool>(
      GetProcAddress(module, "?set_bool@Lua@LuaMadeSimple@RC@@QEBAX_N@Z"));
  city_lua_get_integer = reinterpret_cast<CityLuaGetInteger>(
      GetProcAddress(module, "?get_integer@Lua@LuaMadeSimple@RC@@QEBA_JH@Z"));
  city_is_game_thread = reinterpret_cast<CityIsGameThread>(
      GetProcAddress(module, "?IsInGameThreadRaw@Unreal@RC@@YA_NXZ"));
  if (!register_function || !city_lua_set_bool || !city_lua_get_integer ||
      !city_is_game_thread) {
    city_log(L"Artisan cancellation UI bridge is unavailable; the companion "
             L"script will not expose the cancellation control.");
    return;
  }
  const CityLuaCallback callback = &city_lua_can_cancel_artisan;
  register_function(&lua, "MLCityEditorCanCancelArtisan", callback);
}

class MLCityEditorMod final : public RC::CppUserModBase {
  CityPatchSet patches_;
  std::uint64_t reported_failures_{};
  std::uint64_t reported_retinue_repairs_{};
  std::uint64_t reported_windmill_repairs_{};
  std::uint64_t reported_extension_cancellations_declined_{};
  bool reported_retinue_conflict_{};

public:
  MLCityEditorMod() {
    ModName = L"MLCityEditor";
    ModVersion = L"0.1.0";
    ModDescription = L"Temporary player-only free instant construction and "
                     L"clean demolition.";
    ModAuthors = L"hurryman2212";
  }
  ~MLCityEditorMod() override {
    if (patches_.restore())
      city_clear_callbacks();
    else
      city_log(L"Could not restore a modified hook; pinned callbacks remain "
               L"available. Fully restart the game.");
  }
  auto on_lua_start(RC::StringViewType name, CityLua &lua, CityLua &main,
                    CityLua &async, CityLua *hook) -> void override {
    if (name != L"MLCityEditor")
      return;
    std::vector<CityLua *> states{&lua, &main, &async};
    if (hook)
      states.push_back(hook);
    std::vector<CityLua *> registered;
    for (auto *state : states) {
      if (std::find(registered.begin(), registered.end(), state) ==
          registered.end()) {
        city_register_cancel_query(*state);
        registered.push_back(state);
      }
    }
  }
  auto on_lua_start(RC::StringViewType name, CityLua &lua, CityLua &main,
                    CityLua &async, std::vector<CityLua *> &hooks)
      -> void override {
    on_lua_start(name, lua, main, async, static_cast<CityLua *>(nullptr));
    if (name == L"MLCityEditor")
      for (auto *hook : hooks)
        if (hook && hook != &lua && hook != &main && hook != &async)
          city_register_cancel_query(*hook);
  }
  auto on_unreal_init() -> void override {
    if (patches_.installed())
      return;
    auto *image = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
    try {
      if (!city_validate_image(image, 0x09dfd000)) {
        city_log(
            L"Unsupported executable or code mismatch; no changes applied.");
        return;
      }
      HMODULE retained{};
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&city_validate_image),
                              &retained)) {
        city_log(L"Could not retain callbacks safely; no changes applied.");
        return;
      }
      city_image = reinterpret_cast<std::uintptr_t>(image);
      const auto patches = city_make_patches();
      if (!patches_.install(image, 0x09dfd000, patches,
                            &city_prepare_callbacks)) {
        if (patches_.restore())
          city_clear_callbacks();
        city_log(L"Hook installation failed; requested rollback. Fully restart "
                 L"before retrying.");
        return;
      }
      city_log(
          L"Enabled for 0.8.117: player-only free immediate "
          L"construction/upgrades/reconstruction and demolition without "
          L"Supplies. "
          L"Explicit player Rebuild requests receive no material costs and "
          L"finish through native completion after replacement assemblies "
          L"load. "
          L"Rebuilding clears a stale Rubble function before native type "
          L"initialization restores normal building work and inventory "
          L"controls. "
          L"House extensions wait for all asynchronous assembly work, "
          L"including level-4 artisan main-house replacement, before "
          L"completion. "
          L"Completed player windmills with a hidden shaft and stopped "
          L"timer are activated with the native windmill completion function; "
          L"normal windmills and unfinished buildings are unchanged. "
          L"Artisan extension cancellation uses native extension cleanup "
          L"and native family workplace release, preserving residents and "
          L"their homes. Active recipe priorities are removed through native "
          L"recipe controls; refueling history and stockpile settings reset "
          L"without removing goods, while the current building's native fuel "
          L"requirement and regional fuel registration are retained. "
          L"Native UI cancellation controls are supplied by "
          L"this mod's companion script. "
          L"New apple, pear and quince orchards finish fully planted and "
          L"mature after variety selection, without adding fruit or changing "
          L"the harvest season. Existing orchards are not mass-edited. "
          L"Player building upgrade prerequisites are bypassed; placement "
          L"rules, AI and animal purchases remain native. Completed player "
          L"Manors without a retinue are repaired with the native initial "
          L"squad/5-soldier queue, without refilling existing squads. "
          L"Remove this mod and fully restart to restore normal "
          L"rules. Edited buildings remain ordinary saved buildings.");
    } catch (...) {
      if (patches_.restore())
        city_clear_callbacks();
      city_log(L"Initialization failed; requested rollback. Fully restart "
               L"before retrying.");
    }
  }
  auto on_update() -> void override {
    const auto cancellation_declines = extension_cancel_declined_count();
    if (cancellation_declines != reported_extension_cancellations_declined_) {
      reported_extension_cancellations_declined_ = cancellation_declines;
      city_log(L"An artisan extension cancellation could not pass the "
               L"ownership, family or assembly safety checks. Inspect the "
               L"building before saving; do not repeat cancellation while "
               L"construction or assembly is pending.");
    }
    const auto windmill_repairs = city_windmill_repair_count();
    if (windmill_repairs != reported_windmill_repairs_) {
      reported_windmill_repairs_ = windmill_repairs;
      city_log(L"Restored completed windmill blades and their native rotation "
               L"timer. Staffing and wind-efficiency rules remain native.");
    }
    if (!reported_retinue_conflict_ && city_retinue_conflict_count() != 0) {
      reported_retinue_conflict_ = true;
      city_log(L"A retinue already exists for a region whose retinue link is "
               L"missing. No duplicate was created; that region needs a "
               L"separate ownership/link inspection.");
    }
    const auto repairs = city_retinue_repair_count();
    if (repairs != reported_retinue_repairs_) {
      reported_retinue_repairs_ = repairs;
      city_log(
          L"Restored a missing Manor retinue and queued the native first "
          L"five soldiers. Existing retinues were not replaced or refilled.");
    }
    const auto failures = upgrade_failed_count();
    if (failures != reported_failures_) {
      reported_failures_ = failures;
      city_log(L"An upgrade could not be safely finalized; its native "
               L"construction state was retained. Check the building before "
               L"saving and report this message.");
    }
  }
};

extern "C" {
__declspec(dllexport) bool
MLCityEditorCanRebuildHouse(void *building) noexcept {
  return city_can_rebuild_house(building);
}
__declspec(dllexport) RC::CppUserModBase *start_mod() {
  return new MLCityEditorMod();
}
__declspec(dllexport) void uninstall_mod(RC::CppUserModBase *mod) {
  delete mod;
}
}
