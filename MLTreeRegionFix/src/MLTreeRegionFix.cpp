#include <cstdint>
#include <string>
#include <string_view>

#include <Windows.h>

#include "TreeHooks.hpp"
#include "TreeRuntime.hpp"
#include "UE4SSABI.hpp"

static auto tree_log(std::wstring_view message) -> void {
  using Send = void (*)(std::wstring_view);
  const auto module = GetModuleHandleW(L"UE4SS.dll");
  if (module == nullptr) {
    return;
  }
  const auto send = reinterpret_cast<Send>(
      GetProcAddress(module, "?send@Output@RC@@YAXV?$basic_string_view@_WU?$"
                             "char_traits@_W@std@@@std@@@Z"));
  if (send != nullptr) {
    send(std::wstring(L"[MLTreeRegionFix] ") + std::wstring(message) + L"\n");
  }
}

class MLTreeRegionFixMod final : public RC::CppUserModBase {
  TreePatchSet patches_;

public:
  MLTreeRegionFixMod() {
    ModName = L"MLTreeRegionFix";
    ModVersion = L"0.1.0";
    ModDescription =
        L"Assigns new player-planted trees to their geographic region.";
    ModAuthors = L"hurryman2212";
  }

  ~MLTreeRegionFixMod() override {
    if (patches_.restore()) {
      configure_tree_api({});
      tree_image = 0;
    } else {
      tree_log(L"A hook could not be restored. Restart the game before "
               L"continuing without this mod.");
    }
  }

  auto on_unreal_init() -> void override {
    if (patches_.installed()) {
      return;
    }
    auto *image = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
    if (!tree_validate_image(image)) {
      tree_log(L"Unsupported executable or modified hook site; no changes "
               L"were applied. This build supports Steam 0.8.117 "
               L"(build 26185246) only.");
      return;
    }
    tree_image = reinterpret_cast<std::uintptr_t>(image);
    configure_tree_api({reinterpret_cast<decltype(TreeApi::region_at)>(
        tree_image + 0x4cc9ab0)});
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&tree_select_region),
                            &pinned) ||
        !patches_.install(image, 0x09dfd000, tree_make_patches())) {
      // Leave callbacks usable if an external patch prevents rollback.
      if (!patches_.installed()) {
        configure_tree_api({});
        tree_image = 0;
      }
      tree_log(L"Hook installation failed; original behavior was retained "
               L"or restored. Restart if another hook prevents restoration.");
      return;
    }
    tree_log(
        L"Installed two player tree-region hooks for Steam 0.8.117 "
        L"(build 26185246). New player tree "
        L"visuals and growth records use the planting coordinate's "
        L"region regardless of region ownership. Native planting-task "
        L"eligibility, worker ownership and existing trees are unchanged.");
  }
};

extern "C" __declspec(dllexport) RC::CppUserModBase *start_mod() {
  return new MLTreeRegionFixMod();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase *mod) {
  delete mod;
}
