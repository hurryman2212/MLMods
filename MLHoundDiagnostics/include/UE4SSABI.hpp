#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace RC {
namespace GUI {
class GUITab;
}

namespace LuaMadeSimple {
class Lua;
}

using StringType = std::wstring;
using StringViewType = std::wstring_view;

// UE4SS 3.0.1 commit 69f1bd11222abc485c2f28b5fbe76c235ed0478b.
// The field and virtual-function order is the public C++ mod ABI consumed by
// CppMod.cpp. Keeping the definition local avoids requiring Epic's private
// UEPseudo headers just to compile a user mod.
class CppUserModBase {
protected:
  std::vector<std::shared_ptr<GUI::GUITab>> GUITabs{};

public:
  StringType ModName{};
  StringType ModVersion{};
  StringType ModDescription{};
  StringType ModAuthors{};
  StringType ModIntendedSDKVersion{L"3.0.1"};

  CppUserModBase() = default;
  virtual ~CppUserModBase() = default;

  virtual auto on_update() -> void {}
  virtual auto on_unreal_init() -> void {}
  virtual auto on_ui_init() -> void {}
  virtual auto on_program_start() -> void {}
  virtual auto on_lua_start(StringViewType, LuaMadeSimple::Lua &,
                            LuaMadeSimple::Lua &, LuaMadeSimple::Lua &,
                            std::vector<LuaMadeSimple::Lua *> &) -> void {}
  virtual auto on_lua_start(LuaMadeSimple::Lua &, LuaMadeSimple::Lua &,
                            LuaMadeSimple::Lua &,
                            std::vector<LuaMadeSimple::Lua *> &) -> void {}
  virtual auto on_lua_stop(StringViewType, LuaMadeSimple::Lua &,
                           LuaMadeSimple::Lua &, LuaMadeSimple::Lua &,
                           std::vector<LuaMadeSimple::Lua *> &) -> void {}
  virtual auto on_lua_stop(LuaMadeSimple::Lua &, LuaMadeSimple::Lua &,
                           LuaMadeSimple::Lua &,
                           std::vector<LuaMadeSimple::Lua *> &) -> void {}
  virtual auto on_dll_load(StringViewType) -> void {}
  virtual auto render_tab() -> void {}
  virtual auto on_lua_start(StringViewType, LuaMadeSimple::Lua &,
                            LuaMadeSimple::Lua &, LuaMadeSimple::Lua &,
                            LuaMadeSimple::Lua *) -> void {}
  virtual auto on_lua_start(LuaMadeSimple::Lua &, LuaMadeSimple::Lua &,
                            LuaMadeSimple::Lua &, LuaMadeSimple::Lua *)
      -> void {}
  virtual auto on_lua_stop(StringViewType, LuaMadeSimple::Lua &,
                           LuaMadeSimple::Lua &, LuaMadeSimple::Lua &,
                           LuaMadeSimple::Lua *) -> void {}
  virtual auto on_lua_stop(LuaMadeSimple::Lua &, LuaMadeSimple::Lua &,
                           LuaMadeSimple::Lua &, LuaMadeSimple::Lua *) -> void {
  }
  virtual auto on_cpp_mods_loaded() -> void {}
};

static_assert(sizeof(CppUserModBase) == 0xC0,
              "UE4SS 3.0.1 Shipping C++ mod ABI mismatch");
} // namespace RC
