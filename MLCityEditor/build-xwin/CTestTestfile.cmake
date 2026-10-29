# CMake generated Testfile for 
# Source directory: /home/jihong_min/Projects/MLMods/MLCityEditor
# Build directory: /home/jihong_min/Projects/MLMods/MLCityEditor/build-xwin
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[city_editor_tests]=] "/usr/bin/env" "WINEPREFIX=/home/jihong_min/codex-workspace-manorlords/ManorLordsSaveAnalysis/ooz-wine-prefix" "WINEDEBUG=-all" "/home/jihong_min/.local/share/Steam/steamapps/common/Proton 10.0/files/bin/wine64" "/home/jihong_min/Projects/MLMods/MLCityEditor/build-xwin/city_editor_tests.exe" "Z:/home/jihong_min/.local/share/Steam/steamapps/common/Manor Lords/ManorLords/Binaries/Win64/ManorLords-Win64-Shipping.exe")
set_tests_properties([=[city_editor_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jihong_min/Projects/MLMods/MLCityEditor/CMakeLists.txt;68;add_test;/home/jihong_min/Projects/MLMods/MLCityEditor/CMakeLists.txt;0;")
add_test([=[city_ui_tests]=] "/usr/bin/lua5.4" "/home/jihong_min/Projects/MLMods/MLCityEditor/tests/ui_tests.lua" "/home/jihong_min/Projects/MLMods/MLCityEditor/scripts/main.lua")
set_tests_properties([=[city_ui_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jihong_min/Projects/MLMods/MLCityEditor/CMakeLists.txt;76;add_test;/home/jihong_min/Projects/MLMods/MLCityEditor/CMakeLists.txt;0;")
