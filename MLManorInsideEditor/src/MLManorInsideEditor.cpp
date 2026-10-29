#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "Geometry.hpp"
#include "Patches.hpp"
#include "Placement.hpp"
#include "Snap.hpp"
#include "UE4SSABI.hpp"

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
    send(std::wstring(L"[MLManorInsideEditor] ") + std::wstring(message) +
         L"\n");
  }
}

static auto selection_contains(const Vec3 *point,
                               const NativeArray<Vec3> *triangles,
                               void *building, void *pawn) noexcept -> bool {
  const auto image =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  using Contains = bool (*)(const Vec3 *, const NativeArray<Vec3> *);
  const auto original = reinterpret_cast<Contains>(image + 0x4b4d1f0);
  std::int32_t type{};
  SIZE_T copied{};
  if (building == nullptr ||
      !ReadProcessMemory(GetCurrentProcess(),
                         static_cast<const std::byte *>(building) + 0x3b8,
                         &type, sizeof(type), &copied) ||
      copied != sizeof(type) || type != 98) {
    return original(point, triangles);
  }
  try {
    const auto geometry = build_manor_geometry(building, image);
    if (!geometry.complete) {
      static std::atomic_flag warned = ATOMIC_FLAG_INIT;
      if (!warned.test_and_set()) {
        log_message(L"A Manor component could not be validated. Its original "
                    L"area restrictions and selection are retained.");
      }
      return original(point, triangles);
    }
    if (const auto hit = manor_component_hit(pawn, building, geometry, image);
        hit && *hit) {
      return true;
    }
    Vec3 position{};
    if (!ReadProcessMemory(GetCurrentProcess(), point, &position,
                           sizeof(position), &copied) ||
        copied != sizeof(position)) {
      return original(point, triangles);
    }
    return point_in_manor_modules(geometry, position);
  } catch (...) {
    return original(point, triangles);
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
  std::vector<std::uint8_t> selection{0x49, 0x89, 0xf0, 0x49, 0x89, 0xd9};
  append_absolute_jump(selection,
                       reinterpret_cast<const void *>(&selection_contains));
  result.push_back(make_call_patch(0x4b35394, 0x4b4d1f0, std::move(selection)));
  for (const auto site : {0x4b4c7c5, 0x4b4cc2e, 0x4b785b0}) {
    result.push_back(
        make_call_patch(site, 0x4b9d080,
                        make_collision_bridge(reinterpret_cast<const void *>(
                            &placement_collision))));
  }
  for (const auto &[site, continuation, skip, index_register] :
       std::array<std::array<std::uintptr_t, 4>, 4>{
           {{0x4b9d3a1, 0x4b9d3a8, 0x4b9d62b, 6},
            {0x4b9d86e, 0x4b9d875, 0x4b9dc0b, 8},
            {0x4bc79dd, 0x4bc79e4, 0x4bc7d87, 9},
            {0x4bc7f39, 0x4bc7f40, 0x4bc8112, 9}}}) {
    const std::vector<std::uint8_t> displaced{
        0x41,
        0x8b,
        static_cast<std::uint8_t>(index_register == 9 ? 0x8f : 0x87),
        static_cast<std::uint8_t>(index_register == 9 ? 0x18 : 0xd8),
        static_cast<std::uint8_t>(index_register == 9 ? 1 : 0),
        0,
        0};
    result.push_back(
        {site, displaced,
         make_record_guard(
             reinterpret_cast<const void *>(&placement_skip_record),
             static_cast<std::uint8_t>(index_register), displaced,
             image + continuation, image + skip),
         false});
  }
  std::vector<std::uint8_t> edge{0x49, 0x89, 0xf9};
  append_absolute_jump(
      edge, reinterpret_cast<const void *>(&placement_segment_collision));
  result.push_back(make_call_patch(0x4b7863c, 0x4bc73f0, std::move(edge)));
  std::vector<std::uint8_t> point{0x49, 0x89, 0xf8};
  append_absolute_jump(
      point, reinterpret_cast<const void *>(&placement_initial_point));
  result.push_back(make_call_patch(0x4b78651, 0x4cd5b20, std::move(point)));
  std::vector<std::uint8_t> bounds;
  append_absolute_jump(
      bounds, reinterpret_cast<const void *>(&placement_initial_point_bounds));
  result.push_back(make_call_patch(0x4cd5b89, 0x4d47920, std::move(bounds)));
  // The native plot-to-road path query has a fourth float argument in XMM3.
  // The bridge preserves it, copies mode (arg5), and appends RDI pawn (arg6).
  result.push_back(
      make_call_patch(0x4b583c0, 0x4bb1390,
                      make_collision_bridge(
                          reinterpret_cast<const void *>(&snap_project_path))));
  for (const auto site :
       {0x4b91fd2, 0x4b922a4, 0x4b92454, 0x4b925f6, 0x4b93c32}) {
    std::vector<std::uint8_t> stub;
    append_absolute_jump(
        stub, reinterpret_cast<const void *>(&snap_segment_collision));
    result.push_back(make_call_patch(site, 0x4bc73f0, std::move(stub)));
  }
  for (const auto site : {0x4b8b01f, 0x4b8b1b5, 0x4b8b598, 0x4b8b714}) {
    result.push_back(make_call_patch(
        site, 0x4bc73f0,
        make_snap_graph_bridge(
            reinterpret_cast<const void *>(&snap_graph_segment_collision),
            site)));
  }
  // The original thunk has already adjusted RCX to its batch work item.
  std::vector<std::uint8_t> graph_batch;
  append_absolute_jump(graph_batch,
                       reinterpret_cast<const void *>(&snap_build_graph));
  result.push_back(
      {0x4b8ab74, {0xe9, 0x37, 0, 0, 0}, std::move(graph_batch), false});
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
  static constexpr std::array<std::uint8_t, 4> graph_argument_adjustment{
      0x48, 0x83, 0xc1, 0x48};
  return std::memcmp(image + 0x4b8ab70, graph_argument_adjustment.data(),
                     graph_argument_adjustment.size()) == 0 &&
         snap_graph_profile_matches(image);
}

class MLManorInsideEditorMod final : public RC::CppUserModBase {
  PatchSet patches_;

public:
  MLManorInsideEditorMod() {
    ModName = L"MLManorInsideEditor";
    ModVersion = L"0.1.0";
    ModDescription =
        L"Courtyard building and plot editing; preserves Manor save data.";
    ModAuthors = L"hurryman2212";
  }

  ~MLManorInsideEditorMod() override {
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
    configure_snap_queries(reinterpret_cast<std::uintptr_t>(image));
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&selection_contains),
                            &pinned) ||
        !patches_.install(image, plan)) {
      log_message(L"Hook installation failed. Original behavior was retained "
                  L"or restored.");
      return;
    }
    log_message(L"Courtyard building and polygon-plot editing enabled for "
                L"0.8.117. Physical Manor components and native plot snapping "
                L"remain protected. Manor save geometry is not rewritten. "
                L"Remove and fully restart to restore original behavior.");
    log_message(L"Manor boundary snap links retain native collision rules for "
                L"Manor-created endpoint pairs, including side and pruning "
                L"probes. Plot snapping and actual construction collision "
                L"checks remain separate.");
    log_message(L"All polygon plots preserve native boundary acceptance for "
                L"the first point, edges and triangulated footprint. Physical "
                L"module overlap disables the courtyard exception without "
                L"adding a veto to the original game result.");
    log_message(L"Plot placement is blocked while obstacle queues remain "
                L"pending. Road editing and Castle Plan preview behavior "
                L"are provided by MLRoadEditor.");
    log_message(L"Plot and snap-graph module checks use conservative bounds "
                L"and query-local geometry memoization. Native obstacle "
                L"queries still run for every candidate; results are not "
                L"reused between previews or graph rebuilds.");
  }
};

extern "C" __declspec(dllexport) RC::CppUserModBase *start_mod() {
  return new MLManorInsideEditorMod();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase *mod) {
  delete mod;
}
