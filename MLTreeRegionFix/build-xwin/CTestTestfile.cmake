# CMake generated Testfile for 
# Source directory: /home/jihong_min/Projects/MLMods/MLTreeRegionFix
# Build directory: /home/jihong_min/Projects/MLMods/MLTreeRegionFix/build-xwin
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[tree_region_tests]=] "/usr/bin/env" "WINEPREFIX=/home/jihong_min/codex-workspace-manorlords/ManorLordsSaveAnalysis/ooz-wine-prefix" "WINEDEBUG=-all" "/home/jihong_min/.local/share/Steam/steamapps/common/Proton 10.0/files/bin/wine64" "/home/jihong_min/Projects/MLMods/MLTreeRegionFix/build-xwin/tree_region_tests.exe" "Z:/home/jihong_min/.local/share/Steam/steamapps/common/Manor Lords/ManorLords/Binaries/Win64/ManorLords-Win64-Shipping.exe")
set_tests_properties([=[tree_region_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jihong_min/Projects/MLMods/MLTreeRegionFix/CMakeLists.txt;48;add_test;/home/jihong_min/Projects/MLMods/MLTreeRegionFix/CMakeLists.txt;0;")
