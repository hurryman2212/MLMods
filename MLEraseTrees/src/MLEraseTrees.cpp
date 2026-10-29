#include <cstdint>
#include <string>
#include <string_view>

#include <Windows.h>

#include "EraserHooks.hpp"
#include "EraserRuntime.hpp"
#include "UE4SSABI.hpp"

static auto eraser_log(std::wstring_view message) -> void {
  using Send = void (*)(std::wstring_view);
  const auto module = GetModuleHandleW(L"UE4SS.dll");
  if (module == nullptr) {
    return;
  }
  const auto send = reinterpret_cast<Send>(
      GetProcAddress(module, "?send@Output@RC@@YAXV?$basic_string_view@_WU?$"
                             "char_traits@_W@std@@@std@@@Z"));
  if (send != nullptr) {
    send(std::wstring(L"[MLEraseTrees] ") + std::wstring(message) + L"\n");
  }
}

class MLEraseTreesMod final : public RC::CppUserModBase {
  EraserPatchSet patches_;

public:
  MLEraseTreesMod() {
    ModName = L"MLEraseTrees";
    ModVersion = L"0.1.0";
    ModDescription = L"Allows Erase Shrubbery to remove trees within its "
                     L"existing owned-region brush.";
    ModAuthors = L"hurryman2212";
  }

  ~MLEraseTreesMod() override {
    if (patches_.restore()) {
      configure_eraser_api({});
      eraser_image = 0;
    } else {
      eraser_log(L"A hook could not be restored. Restart before continuing "
                 L"without this mod.");
    }
  }

  auto on_unreal_init() -> void override {
    if (patches_.installed()) {
      return;
    }
    auto *image = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
    if (!eraser_validate_image(image)) {
      eraser_log(L"Unsupported executable or modified hook site; no changes "
                 L"applied. Supports Steam 0.8.111 only.");
      return;
    }
    eraser_image = reinterpret_cast<std::uintptr_t>(image);
    configure_eraser_api(
        {reinterpret_cast<decltype(EraserApi::queue_tree_removal)>(
            eraser_image + 0x4b7bd00)});
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&eraser_allow_tree),
                            &pinned) ||
        !patches_.install(image, 0x09deb000, eraser_make_patches())) {
      if (!patches_.installed()) {
        configure_eraser_api({});
        eraser_image = 0;
      }
      eraser_log(L"Hook installation failed; original behavior retained or "
                 L"restored. Restart if another hook prevents restoration.");
      return;
    }
    eraser_log(L"Installed tree inclusion and cleanup hooks for 0.8.111. Erase "
               L"Shrubbery now also removes trees inside its original "
               L"owned-region brush.");
  }
};

extern "C" __declspec(dllexport) RC::CppUserModBase *start_mod() {
  return new MLEraseTreesMod();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase *mod) {
  delete mod;
}
