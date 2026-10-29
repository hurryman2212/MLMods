#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#include <Windows.h>

#include "FenceLifecycle.hpp"
#include "FenceLifecycleHooks.hpp"
#include "HousePlacement.hpp"
#include "ManorLoadNative.hpp"
#include "ManorLoadRuntime.hpp"
#include "MarketSelection.hpp"
#include "PlacementHooks.hpp"
#include "PlacementRuntime.hpp"
#include "UE4SSABI.hpp"

static std::atomic<bool> enabled{};
static std::atomic<std::uint64_t> passes{}, buildings{}, hidden{}, shown{},
    skipped{}, collisions{}, retired_helpers{};
static std::atomic<std::uint64_t> restored_manors{}, planned_parts{},
    road_parts{};
static void (*native_assembly_tail)(void *){};
static void (*native_hover)(void *, void *){};
static std::atomic<std::uint64_t> market_redirects{};
static thread_local bool reconciling{};
static bool (*native_is_game_thread)(){};

static auto fence_is_game_thread() noexcept -> bool {
  return native_is_game_thread != nullptr && native_is_game_thread();
}

static auto fence_write(std::uintptr_t address, const void *value,
                        std::size_t size) noexcept -> bool {
  SIZE_T written{};
  return address != 0 && value != nullptr && size != 0 &&
         size <= std::numeric_limits<std::uintptr_t>::max() - address &&
         WriteProcessMemory(GetCurrentProcess(),
                            reinterpret_cast<void *>(address), value, size,
                            &written) != 0 &&
         written == size;
}

static void clear_fence_instances(void *component) {
  // The lifecycle preflight verifies component identity, ownership and this
  // exact native vtable entry before reaching the callback.
  const auto *vtable = *reinterpret_cast<std::uintptr_t **>(component);
  reinterpret_cast<void (*)(void *)>(vtable[0xba8 / sizeof(void *)])(component);
}

static auto can_rebuild_house(void *owner) noexcept -> bool {
  using Query = bool (*)(void *) noexcept;
  static Query query{};
  if (query == nullptr) {
    try {
      HMODULE self{}, city{};
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&can_rebuild_house),
                              &self))
        return false;
      std::wstring filename(32768, L'\0'); // Windows extended path limit.
      const auto length = GetModuleFileNameW(
          self, filename.data(), static_cast<DWORD>(filename.size()));
      if (length == 0 || length >= filename.size())
        return false;
      filename.resize(length);
      const auto path = std::filesystem::path(filename)
                            .parent_path()
                            .parent_path()
                            .parent_path() /
                        L"MLCityEditor" / L"dlls" / L"main.dll";
      // Never load another mod. If the installed CityEditor has no reliable
      // assembly barrier, preserve the original regeneration call instead.
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, path.c_str(),
                              &city))
        return false;
      query = reinterpret_cast<Query>(
          GetProcAddress(city, "MLCityEditorCanRebuildHouse"));
    } catch (...) {
      return false;
    }
  }
  return query != nullptr && query(owner);
}

static bool placement_read(std::uintptr_t address, void *output,
                           std::size_t size) noexcept {
  SIZE_T copied{};
  return address && output && size &&
         size <= std::numeric_limits<std::uintptr_t>::max() - address &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), output,
                           size, &copied) &&
         copied == size;
}

static void log_placement(std::wstring_view message) {
  using Send = void (*)(std::wstring_view);
  const auto ue4ss = GetModuleHandleW(L"UE4SS.dll");
  const auto send = ue4ss
                        ? reinterpret_cast<Send>(GetProcAddress(
                              ue4ss, "?send@Output@RC@@YAXV?$basic_string_view@"
                                     "_WU?$char_traits@_W@std@@@std@@@Z"))
                        : nullptr;
  if (send)
    send(std::wstring(L"[MLPlacementFix] ") + std::wstring(message) + L"\n");
}

static void record_result(const PlacementStats &stats) noexcept {
  buildings.fetch_add(stats.buildings, std::memory_order_relaxed);
  hidden.fetch_add(stats.hidden_priority + stats.hidden_marked,
                   std::memory_order_relaxed);
  shown.fetch_add(stats.shown_priority + stats.shown_marked +
                      stats.shown_general,
                  std::memory_order_relaxed);
  collisions.fetch_add(stats.collisions_restored, std::memory_order_relaxed);
  retired_helpers.fetch_add(stats.helpers_retired, std::memory_order_relaxed);
  skipped.fetch_add(stats.skipped, std::memory_order_relaxed);
  passes.fetch_add(1, std::memory_order_release);
}

static void reconcile_owner(void *owner) noexcept {
  if (!enabled.load(std::memory_order_acquire) || reconciling)
    return;
  reconciling = true;
  record_result(placement_reconcile_building(owner));
  reconciling = false;
}

static void reconcile_loaded_world(void *master) noexcept {
  if (!enabled.load(std::memory_order_acquire) || reconciling)
    return;
  reconciling = true;
  record_result(placement_reconcile_world(master));
  reconciling = false;
}

static void assembly_completed(void *owner) {
  // This native tail updates building-local socket/point data. Never suppress
  // it, including after this pinned mod is disabled during process shutdown.
  native_assembly_tail(owner);
  reconcile_owner(owner);
}

static void select_market_stall(void *pawn, void *accepted) {
  auto *target = accepted;
  if (enabled.load(std::memory_order_acquire)) {
    target = market_selection_target(pawn, accepted);
    if (target != accepted)
      market_redirects.fetch_add(1, std::memory_order_relaxed);
  }
  // Keep the original hover/highlight update exactly once, including shutdown.
  // The subsequent native click handler still enforces ownership and UI rules.
  native_hover(pawn, target);
}

static bool select_manor_restore(void *record, void *manor,
                                 void *definition) noexcept {
  const bool selected =
      (enabled.load(std::memory_order_acquire) || manor_load_plan_active()) &&
      manor_load_restore_saved(record, manor, definition);
  if (selected)
    road_parts.fetch_add(1, std::memory_order_relaxed);
  return selected;
}

static const ManorLoadArray *begin_manor_restore(void *master, void *manor,
                                                 void *saved) noexcept {
  if (!enabled.load(std::memory_order_acquire) && !manor_load_plan_active())
    return nullptr;
  const auto *plan = manor_load_begin(master, manor, saved);
  if (plan) {
    restored_manors.fetch_add(1, std::memory_order_relaxed);
    planned_parts.fetch_add(static_cast<std::uint64_t>(plan->count),
                            std::memory_order_relaxed);
  }
  return plan;
}

class MLPlacementFixMod final : public RC::CppUserModBase {
  PlacementHooks hooks_;
  ManorLoadHook manor_hooks_;
  FenceLifecycleHooks fence_hooks_;
  std::uint64_t reported_passes_{};
  std::uint64_t reported_manors_{}, reported_roads_{};
  std::uint64_t reported_house_repairs_{}, reported_house_skips_{};
  std::uint64_t reported_market_redirects_{}, last_market_report_ms_{};
  std::uint64_t reported_fence_cleanups_{};

public:
  MLPlacementFixMod() {
    ModName = L"MLPlacementFix";
    ModVersion = L"0.1.0";
    ModAuthors = L"hurryman2212";
    ModDescription =
        L"Native Manor load placement and consistent auxiliary-object priority "
        L"after loading and construction.";
  }

  ~MLPlacementFixMod() override {
    set_fence_lifecycle_enabled(false);
    set_house_placement_enabled(false);
    enabled.store(false, std::memory_order_release);
    hooks_.retain_until_exit();
    manor_hooks_.retain_until_exit();
    fence_hooks_.retain_until_exit();
  }

  auto on_unreal_init() -> void override {
    if (hooks_.installed() || manor_hooks_.installed() ||
        fence_hooks_.installed())
      return;
    auto *image = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
    if (!placement_validate_image(image) || !manor_load_validate_image(image)) {
      log_placement(L"Unsupported executable or conflicting hook. Steam "
                    L"0.8.117 required; no changes applied.");
      return;
    }
    placement_image = reinterpret_cast<std::uintptr_t>(image);
    manor_load_image = placement_image;
    configure_manor_load_api({placement_read});
    configure_market_selection(
        placement_image,
        {placement_read,
         reinterpret_cast<decltype(MarketSelectionApi::contains)>(image +
                                                                  0x4b4d1f0),
         reinterpret_cast<decltype(MarketSelectionApi::cell_index)>(image +
                                                                    0x4c3e610),
         reinterpret_cast<decltype(MarketSelectionApi::max_workers)>(
             image + 0x4d40700)});
    native_hover =
        reinterpret_cast<void (*)(void *, void *)>(image + 0x4b6ef70);
    configure_placement_api(
        {placement_read,
         reinterpret_cast<decltype(PlacementApi::set_visibility)>(image +
                                                                  0x37a9c60),
         reinterpret_cast<decltype(PlacementApi::set_module_collision)>(
             image + 0x4e1c5b0),
         reinterpret_cast<decltype(PlacementApi::retire_helper)>(image +
                                                                 0x4c11a20)});
    configure_house_placement_api(
        {.read_memory = placement_read,
         .rebuild = reinterpret_cast<decltype(HousePlacementApi::rebuild)>(
             image + 0x4cffbb0),
         .choose = reinterpret_cast<decltype(HousePlacementApi::choose)>(
             image + 0x4d17af0),
         .image = placement_image,
         .can_rebuild_house = can_rebuild_house,
         .destroy_actor =
             reinterpret_cast<decltype(HousePlacementApi::destroy_actor)>(
                 image + 0x4df7390),
         .remove_actor =
             reinterpret_cast<decltype(HousePlacementApi::remove_actor)>(
                 image + 0x4b1ef90),
         .is_field = reinterpret_cast<decltype(HousePlacementApi::is_field)>(
             image + 0x4dba960),
         .cleanup_module =
             reinterpret_cast<decltype(HousePlacementApi::cleanup_module)>(
                 image + 0x4dade90)});
    native_assembly_tail =
        reinterpret_cast<void (*)(void *)>(image + 0x4d4e4b0);
    const auto ue4ss = GetModuleHandleW(L"UE4SS.dll");
    native_is_game_thread =
        ue4ss ? reinterpret_cast<bool (*)()>(GetProcAddress(
                    ue4ss, "?IsInGameThreadRaw@Unreal@RC@@YA_NXZ"))
              : nullptr;
    configure_fence_lifecycle_api(
        {.image = placement_image,
         .read_memory = placement_read,
         .write_memory = fence_write,
         .on_game_thread = fence_is_game_thread,
         .intersect = reinterpret_cast<decltype(FenceLifecycleApi::intersect)>(
             image + 0x4cd12e0),
         .slice = reinterpret_cast<decltype(FenceLifecycleApi::slice)>(
             image + 0x4cddec0),
         .clear_instances = clear_fence_instances,
         .remove_fence_refs =
             reinterpret_cast<decltype(FenceLifecycleApi::remove_fence_refs)>(
                 image + 0x4c4c5c0)});
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&reconcile_owner),
                            &pinned)) {
      log_placement(L"Could not retain callbacks safely; no hooks installed.");
      return;
    }
    if (!manor_hooks_.install(
            image, reinterpret_cast<const void *>(&select_manor_restore),
            reinterpret_cast<const void *>(&begin_manor_restore),
            reinterpret_cast<const void *>(&manor_load_end))) {
      log_placement(
          L"Manor hook installation failed. Fully restart before retrying.");
      return;
    }
    if (!hooks_.install(
            image, reinterpret_cast<const void *>(&reconcile_loaded_world),
            reinterpret_cast<const void *>(&reconcile_owner),
            reinterpret_cast<const void *>(&assembly_completed),
            reinterpret_cast<const void *>(&house_placement_rebuild),
            reinterpret_cast<const void *>(&house_placement_choose_primary),
            reinterpret_cast<const void *>(&house_placement_choose_secondary),
            reinterpret_cast<const void *>(&select_market_stall))) {
      const bool restored = manor_hooks_.restore();
      log_placement(
          restored
              ? L"Placement hooks failed; Manor hooks restored. Fully restart "
                L"before retrying."
              : L"Hook rollback incomplete. Fully restart before retrying.");
      return;
    }
    enabled.store(true, std::memory_order_release);
    set_house_placement_enabled(true);
    if (native_is_game_thread != nullptr &&
        fence_hooks_.install(
            image,
            reinterpret_cast<const void *>(
                &fence_lifecycle_observe_intersection),
            reinterpret_cast<const void *>(&fence_lifecycle_slice),
            reinterpret_cast<const void *>(&fence_lifecycle_finish))) {
      set_fence_lifecycle_enabled(true);
      log_placement(
          L"Enabled native fence failure parity: construction, upgrades and "
          L"loading retain no previous fence after an invalid house fit or "
          L"failed native plot slice. Uses native instance/grid cleanup; "
          L"house poses, plot geometry and native generation results are "
          L"unchanged.");
    } else {
      log_placement(
          L"Fence lifecycle validation/installation failed; fence parity is "
          L"disabled. Existing placement protections remain active.");
    }
    log_placement(
        L"Enabled for 0.8.117: source-based Manor wall/gate/tower restoration "
        L"and one native tag/bounds priority policy after "
        L"load, construction, assembly, extension changes and completed "
        L"construction-visual updates. Restores ordinary completed modules "
        L"and their native collision mode, then honors native overlap "
        L"priority. Late construction helpers are retired through native "
        L"module completion cleanup after reciprocal ownership validation. "
        L"Level-4 artisan house regeneration uses native selective cleanup, "
        L"retains extension actors and cached house models, and requires "
        L"CityEditor's verified assembly barrier. "
        L"Marketplace hover defers to its own stall under the pointer; "
        L"shared grid order, empty-market selection and native click guards "
        L"remain unchanged. "
        L"No construction completion, automatic Commit or save-file edits. "
        L"Replaces MLLoadFix; do not enable both mods.");
  }

  auto on_update() -> void override {
    // UE4SS update only reports atomics. All game changes run at native
    // game-thread lifecycle boundaries, never from this reporting callback.
    const auto redirects = market_redirects.load(std::memory_order_relaxed);
    const auto now = GetTickCount64();
    const auto fence_cleanups = fence_lifecycle_cleanup_count();
    if (fence_cleanups != reported_fence_cleanups_) {
      reported_fence_cleanups_ = fence_cleanups;
      log_placement(
          L"Removed stale fences after native generation failure: total=" +
          std::to_wstring(fence_cleanups));
    }
    if (redirects != reported_market_redirects_ &&
        now - last_market_report_ms_ >=
            5000) { // Throttle per-frame hover counts.
      reported_market_redirects_ = redirects;
      last_market_report_ms_ = now;
      log_placement(L"Marketplace own-stall hover corrections: total=" +
                    std::to_wstring(redirects));
    }
    const auto house_repairs = house_placement_repair_count();
    const auto house_skips = house_placement_skip_count();
    if (house_repairs != reported_house_repairs_ ||
        house_skips != reported_house_skips_) {
      reported_house_repairs_ = house_repairs;
      reported_house_skips_ = house_skips;
      const auto diagnostic = house_placement_diagnostic();
      log_placement(
          L"Native artisan house regeneration: repaired=" +
          std::to_wstring(house_repairs) + L", safety-skipped/stopped=" +
          std::to_wstring(house_skips) + L", last failure=" +
          house_placement_failure_name(diagnostic.failure) +
          L", last failed building=" + std::to_wstring(diagnostic.owner) +
          L". Failed preflight retains the original native path; identity "
          L"changes stop further edits. No forced cleanup or delayed retry.");
    }
    const auto manors = restored_manors.load(std::memory_order_relaxed);
    const auto roads = road_parts.load(std::memory_order_relaxed);
    if (manors != reported_manors_ || roads != reported_roads_) {
      reported_manors_ = manors;
      reported_roads_ = roads;
      log_placement(L"Manor source restoration: manors=" +
                    std::to_wstring(manors) + L", planned parts=" +
                    std::to_wstring(planned_parts.load()) +
                    L", direct road parts=" + std::to_wstring(roads));
    }
    const auto count = passes.load(std::memory_order_acquire);
    if (count == reported_passes_)
      return;
    reported_passes_ = count;
    log_placement(
        L"Reconciled passes=" + std::to_wstring(count) + L", buildings=" +
        std::to_wstring(buildings.load()) + L", hidden=" +
        std::to_wstring(hidden.load()) + L", restored=" +
        std::to_wstring(shown.load()) + L", module collision restored=" +
        std::to_wstring(collisions.load()) + L", late helpers retired=" +
        std::to_wstring(retired_helpers.load()) +
        L", skipped incomplete/invalid=" + std::to_wstring(skipped.load()));
  }
};

extern "C" __declspec(dllexport) RC::CppUserModBase *start_mod() {
  return new MLPlacementFixMod();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase *mod) {
  delete mod;
}
