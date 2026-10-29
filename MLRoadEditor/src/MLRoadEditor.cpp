#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "CastlePreview.hpp"
#include "ConnectionBridges.hpp"
#include "Connections.hpp"
#include "ConnectionsReload.hpp"
#include "Geometry.hpp"
#include "Patches.hpp"
#include "RoadPlacement.hpp"
#include "RoadRuleBridge.hpp"
#include "UE4SSABI.hpp"

static constexpr std::uint64_t preview_log_interval_ms = 5000;
using FinishRoad = void *(*)(void *);
static FinishRoad native_finish_road{};

static std::atomic<std::uint64_t> connection_loads{};
static std::atomic<std::uint64_t> connection_regions{}, connection_buildings{},
    connection_recovered{}, connection_roads{}, connection_caches{},
    connection_skipped{};

static void refresh_connections_after_load(void *master) noexcept {
  castle_preview_reset();
  const auto result = connections_after_load(master);
  connection_regions.store(result.regions, std::memory_order_relaxed);
  connection_buildings.store(result.buildings, std::memory_order_relaxed);
  connection_recovered.store(result.recovered_points,
                             std::memory_order_relaxed);
  connection_roads.store(result.roads, std::memory_order_relaxed);
  connection_caches.store(result.caches, std::memory_order_relaxed);
  connection_skipped.store(result.skipped, std::memory_order_relaxed);
  connection_loads.fetch_add(1, std::memory_order_release);
}

static void *finish_castle_road(void *pawn) {
  // Both native callers consume the complete road pointer returned in RAX.
  // Refresh retained siblings before accepting the current placement.
  castle_preview_flush(pawn);
  return native_finish_road(pawn);
}

static auto log_message(std::wstring_view message) -> void {
  using Send = void (*)(std::wstring_view);
  const auto ue4ss = GetModuleHandleW(L"UE4SS.dll");
  if (ue4ss == nullptr) {
    return;
  }
  const auto send = reinterpret_cast<Send>(
      GetProcAddress(ue4ss, "?send@Output@RC@@YAXV?$basic_string_view@_WU?$"
                            "char_traits@_W@std@@@std@@@Z"));
  if (send != nullptr) {
    send(std::wstring(L"[MLRoadEditor] ") + std::wstring(message) + L"\n");
  }
}

static auto make_call_patch(std::uintptr_t site, std::uintptr_t destination,
                            std::vector<std::uint8_t> stub) -> CodePatch {
  std::vector<std::uint8_t> expected{0xe8, 0, 0, 0, 0};
  const auto displacement = static_cast<std::int32_t>(destination - site - 5);
  std::memcpy(expected.data() + 1, &displacement, sizeof(displacement));
  return {site, std::move(expected), std::move(stub), true};
}

static auto make_patches(std::uint8_t *image) -> std::vector<CodePatch> {
  std::vector<CodePatch> result;
  // Change only Castle Plan's obstacle-detour bypass for actual road editing.
  // Its independent wall/gate regeneration branch and editor state remain.
  result.push_back(
      {0x4b68dfa,
       {0x44, 0x38, 0xa7, 0x48, 0x08, 0, 0, 0x0f, 0x85, 0x29, 0x0c, 0, 0},
       make_road_rule_bridge(
           reinterpret_cast<const void *>(&road_use_normal_obstacle_path),
           image + 0x4b68e07, image + 0x4b69a30),
       false});
  for (const auto site : {0x4b687ad, 0x4b68806}) {
    result.push_back(make_call_patch(
        site, 0x4c90000,
        make_road_bridge(
            reinterpret_cast<const void *>(&road_endpoint_collision), false)));
  }
  for (const auto site : {0x4b68cf0, 0x4b68d59, 0x4b68db8}) {
    result.push_back(make_call_patch(
        site, 0x4b9dd70,
        make_road_bridge(
            reinterpret_cast<const void *>(&road_segment_collision), true)));
  }
  result.push_back(make_call_patch(
      0x4b6a194, 0x4b9c590,
      make_road_bridge(reinterpret_cast<const void *>(&road_point_collision),
                       false)));
  // The fallback curve/path solver must see the same Manor exception as the
  // direct preview checks. Its original algorithm and tangent rules remain.
  result.push_back(make_call_patch(
      0x4b69032, 0x4b90890,
      make_road_bridge(reinterpret_cast<const void *>(&road_path_query),
                       false)));
  for (const auto site : {0x4b90930, 0x4b90b12, 0x4b90bff}) {
    std::vector<std::uint8_t> segment;
    append_absolute_jump(
        segment, reinterpret_cast<const void *>(&road_path_segment_collision));
    result.push_back(make_call_patch(site, 0x4b9dd70, std::move(segment)));
  }
  std::vector<std::uint8_t> path_point;
  append_absolute_jump(
      path_point, reinterpret_cast<const void *>(&road_path_point_collision));
  result.push_back(
      make_call_patch(0x4b909ad, 0x4b9c590, std::move(path_point)));
  // Connection creation, footprint masking, and blocked-point recovery share
  // the native clearance rules; only the filled Manor area is substituted.
  result.push_back(
      make_call_patch(0x4d56619, 0x4c46b20,
                      make_connection_nav_bridge(reinterpret_cast<const void *>(
                          &connections_initial_point))));
  std::vector<std::uint8_t> nav_circle;
  append_absolute_jump(nav_circle,
                       reinterpret_cast<const void *>(&connections_nav_circle));
  result.push_back(
      make_call_patch(0x4c2e9d0, 0x4b16cf0, std::move(nav_circle)));
  result.push_back(make_call_patch(
      0x4d1ca47, 0x4b16cf0,
      make_connection_mask_bridge(
          reinterpret_cast<const void *>(&connections_mask_circle))));
  result.push_back(make_call_patch(
      0x4cff83f, 0x4b9c590,
      make_connection_restore_bridge(
          reinterpret_cast<const void *>(&connections_restore_point))));
  // Actual outer load completion, after native obstacle registration. The
  // earlier child-load delegate is too early even for entrance restoration.
  result.push_back(
      {0x4c84b94,
       {0xc6, 0x86, 0x18, 0x03, 0, 0, 1},
       make_connection_postload_bridge(
           reinterpret_cast<const void *>(&refresh_connections_after_load),
           image + 0x4c84b9b),
       false});
  // Only the cursor-motion sibling loop: final creation, undo, deletion and
  // Commit keep their separate original regeneration calls.
  result.push_back(
      make_call_patch(0x4b69eb9, 0x4cd6430,
                      make_castle_road_bridge(reinterpret_cast<const void *>(
                          &castle_preview_regenerate))));
  for (const auto site : {0x4abb29e, 0x4be9206}) {
    std::vector<std::uint8_t> finish;
    append_absolute_jump(finish,
                         reinterpret_cast<const void *>(&finish_castle_road));
    result.push_back(make_call_patch(site, 0x4b368c0, std::move(finish)));
  }
  return result;
}

static auto validate_image(const std::uint8_t *image,
                           const std::vector<CodePatch> &patches) -> bool {
  if (image == nullptr) {
    return false;
  }
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
      dos->e_lfanew < static_cast<LONG>(sizeof(*dos)) ||
      dos->e_lfanew > 0x1000) {
    return false;
  }
  const auto *nt =
      reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
  // Exact Steam 0.8.117 executable, SHA256
  // 560451df38cd91134026c25b69539449c570f3163e33bc8c45b4dc20b6952cee.
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt->FileHeader.TimeDateStamp != 0x6836c1ba ||
      nt->OptionalHeader.SizeOfImage != 0x09dfd000 ||
      nt->OptionalHeader.CheckSum != 0x09a2ba15) {
    return false;
  }
  for (const auto &patch : patches) {
    if (patch.rva + patch.expected.size() > nt->OptionalHeader.SizeOfImage ||
        std::memcmp(image + patch.rva, patch.expected.data(),
                    patch.expected.size()) != 0) {
      return false;
    }
  }
  static constexpr std::array<std::uint8_t, 17> castle_call_context{
      0x44, 0x38, 0xbb, 0xb3, 0x02, 0x00, 0x00, 0x74, 0x0a,
      0xb2, 0x01, 0x48, 0x8b, 0xcb, 0xe8, 0x72, 0xc5};
  static constexpr std::array<std::uint8_t, 21> castle_native_start{
      0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55,
      0x57, 0x41, 0x56, 0x48, 0x8d, 0x6c, 0x24, 0xb9, 0x48, 0x81};
  static constexpr std::array<std::uint8_t, 18> castle_iterator_context{
      0x48, 0x8b, 0xb0, 0xf0, 0x0e, 0x00, 0x00, 0x48, 0x63,
      0x80, 0xf8, 0x0e, 0x00, 0x00, 0x4c, 0x8d, 0x34, 0xc6};
  static constexpr std::array<std::uint8_t, 3> native_zero_register{
      0x45, 0x32, 0xe4}; // xor r12b,r12b before the obstacle scan.
  static constexpr std::array<std::uint8_t, 9> native_gate_branch{
      0x44, 0x38, 0xbf, 0x48, 0x08, 0x00, 0x00, 0x74, 0x6b};
  return std::memcmp(image + 0x4b68be6, native_zero_register.data(),
                     native_zero_register.size()) == 0 &&
         std::memcmp(image + 0x4b69e53, native_gate_branch.data(),
                     native_gate_branch.size()) == 0 &&
         std::memcmp(image + 0x4b69eab, castle_call_context.data(),
                     castle_call_context.size()) == 0 &&
         std::memcmp(image + 0x4cd6430, castle_native_start.data(),
                     castle_native_start.size()) == 0 &&
         std::memcmp(image + 0x4b69e68, castle_iterator_context.data(),
                     castle_iterator_context.size()) == 0 &&
         connection_profile_matches(image);
}

class MLRoadEditorMod final : public RC::CppUserModBase {
  PatchSet patches_;
  std::uint64_t reported_loads_{};
  std::uint64_t reported_preview_calls_{}, last_preview_report_ms_{};
  std::uint64_t reported_normal_paths_{}, last_normal_report_ms_{};

public:
  MLRoadEditorMod() {
    ModName = L"MLRoadEditor";
    ModVersion = L"0.1.0";
    ModDescription =
        L"Native road editing and connections inside and outside Manors.";
    ModAuthors = L"hurryman2212";
  }

  ~MLRoadEditorMod() override {
    if (!patches_.restore()) {
      log_message(L"Could not fully restore a hook. Restart the game before "
                  L"continuing without this mod.");
    }
  }

  auto on_unreal_init() -> void override {
    if (patches_.installed()) {
      return;
    }
    auto *image = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
    const auto plan = make_patches(image);
    if (!validate_image(image, plan)) {
      log_message(L"Unsupported executable or modified hook site. No changes "
                  L"were applied; this build supports Steam 0.8.117 only.");
      return;
    }
    // The code callbacks remain mapped even if an external hook prevents
    // rollback during a hot unload. Normal use is remove + full restart.
    HMODULE pinned{};
    configure_road_placement(reinterpret_cast<std::uintptr_t>(image));
    configure_connections(reinterpret_cast<std::uintptr_t>(image));
    configure_connection_reload(reinterpret_cast<std::uintptr_t>(image));
    configure_castle_preview(reinterpret_cast<std::uintptr_t>(image));
    native_finish_road = reinterpret_cast<FinishRoad>(image + 0x4b368c0);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&road_endpoint_collision),
                            &pinned) ||
        !patches_.install(image, plan)) {
      log_message(L"Hook installation failed. Original behavior was retained "
                  L"or restored.");
      return;
    }
    log_message(L"Road editing with native road/building snap "
                L"settings retained for 0.8.117. Entrance creation, masking "
                L"and restoration preserve actual Manor components. Each "
                L"load refreshes blocked entrances, native road associations "
                L"and field-snap links in owned Manor regions. Manor save "
                L"geometry is not rewritten. Remove and fully restart to "
                L"restore original behavior.");
    log_message(
        L"Direct road checks and the native curve/path solver share "
        L"the same scoped Manor-area exception. Native-clear results "
        L"are never vetoed. Ordinary roads retain physical wall "
        L"blocking; Castle Plan retains its native wall-crossing and "
        L"gate creation. Curve controls and wall editing are unchanged.");
    log_message(L"Castle Plan ordinary roads now enter the original ordinary "
                L"obstacle-detour branch; the independent wall/gate branch "
                L"and saved editor state are unchanged.");
    log_message(L"Connection recovery runs at true outer load completion, "
                L"after obstacle registration, and only queues road/snap "
                L"updates. No worker is started inside the loader. Plot "
                L"placement is blocked while obstacle queues remain pending.");
    log_message(L"Castle Plan preview optimization enabled: unchanged "
                L"sibling passes share one validated world/Manor snapshot; "
                L"unaffected wall previews refresh in chunks of at most two "
                L"per sibling pass, with a 250 ms quiet interval after each "
                L"complete refresh sweep. "
                L"Ordinary-road crossings, changed inputs and mouse-button "
                L"actions use native regeneration. Retained siblings refresh "
                L"before final road placement; native Commit/undo/cancel "
                L"logic remains unchanged.");
  }

  auto on_update() -> void override {
    const auto normal_paths = road_normal_path_count();
    const auto report_time = GetTickCount64();
    if (normal_paths != reported_normal_paths_ &&
        report_time - last_normal_report_ms_ >= preview_log_interval_ms) {
      reported_normal_paths_ = normal_paths;
      last_normal_report_ms_ = report_time;
      log_message(L"Castle Plan entered native ordinary-road obstacle "
                  L"routing: total=" +
                  std::to_wstring(normal_paths));
    }
    const auto loads = connection_loads.load(std::memory_order_acquire);
    if (loads != reported_loads_) {
      log_message(
          L"Road connections refreshed after load: regions=" +
          std::to_wstring(connection_regions.load(std::memory_order_relaxed)) +
          L", buildings checked for recovery=" +
          std::to_wstring(
              connection_buildings.load(std::memory_order_relaxed)) +
          L", restored entrances=" +
          std::to_wstring(
              connection_recovered.load(std::memory_order_relaxed)) +
          L", roads re-registered=" +
          std::to_wstring(connection_roads.load(std::memory_order_relaxed)) +
          L", caches queued for native processing=" +
          std::to_wstring(connection_caches.load(std::memory_order_relaxed)) +
          L", skipped invalid geometry/data=" +
          std::to_wstring(connection_skipped.load(std::memory_order_relaxed)));
      reported_loads_ = loads;
    }
    const auto preview = castle_preview_metrics();
    const auto now = GetTickCount64();
    if (preview.calls != reported_preview_calls_ &&
        now - last_preview_report_ms_ >= preview_log_interval_ms) {
      log_message(
          L"Castle Plan preview calls=" + std::to_wstring(preview.calls) +
          L", reused=" + std::to_wstring(preview.reused) +
          L", native rebuilds=" + std::to_wstring(preview.native) +
          L", pre-click refreshes=" + std::to_wstring(preview.flushed) +
          L", expired=" + std::to_wstring(preview.expired) + L", changed=" +
          std::to_wstring(preview.changed) + L", ineligible=" +
          std::to_wstring(preview.ineligible) + L", overlap=" +
          std::to_wstring(preview.overlap) + L", buttons=" +
          std::to_wstring(preview.buttons) + L", periodic=" +
          std::to_wstring(preview.periodic) + L", deferred=" +
          std::to_wstring(preview.deferred) + L", max periodic per pass=" +
          std::to_wstring(preview.max_periodic_per_pass));
      reported_preview_calls_ = preview.calls;
      last_preview_report_ms_ = now;
    }
  }
};

extern "C" __declspec(dllexport) RC::CppUserModBase *start_mod() {
  return new MLRoadEditorMod();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase *mod) {
  delete mod;
}
