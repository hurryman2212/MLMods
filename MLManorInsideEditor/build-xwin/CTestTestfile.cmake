# CMake generated Testfile for 
# Source directory: /home/jihong_min/Projects/MLMods/MLManorInsideEditor
# Build directory: /home/jihong_min/Projects/MLMods/MLManorInsideEditor/build-xwin
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[manor_editor_tests]=] "/usr/bin/env" "WINEPREFIX=/home/jihong_min/codex-workspace-manorlords/ManorLordsSaveAnalysis/ooz-wine-prefix" "WINEDEBUG=-all" "/home/jihong_min/.local/share/Steam/steamapps/common/Proton 10.0/files/bin/wine64" "/home/jihong_min/Projects/MLMods/MLManorInsideEditor/build-xwin/manor_editor_tests.exe" "Z:/home/jihong_min/.local/share/Steam/steamapps/common/Manor Lords/ManorLords/Binaries/Win64/ManorLords-Win64-Shipping.exe")
set_tests_properties([=[manor_editor_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jihong_min/Projects/MLMods/MLManorInsideEditor/CMakeLists.txt;50;add_test;/home/jihong_min/Projects/MLMods/MLManorInsideEditor/CMakeLists.txt;0;")
add_test([=[placement_tests]=] "/usr/bin/env" "WINEPREFIX=/home/jihong_min/codex-workspace-manorlords/ManorLordsSaveAnalysis/ooz-wine-prefix" "WINEDEBUG=-all" "/home/jihong_min/.local/share/Steam/steamapps/common/Proton 10.0/files/bin/wine64" "/home/jihong_min/Projects/MLMods/MLManorInsideEditor/build-xwin/placement_tests.exe")
set_tests_properties([=[placement_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jihong_min/Projects/MLMods/MLManorInsideEditor/CMakeLists.txt;61;add_test;/home/jihong_min/Projects/MLMods/MLManorInsideEditor/CMakeLists.txt;0;")
add_test([=[snap_tests]=] "/usr/bin/env" "WINEPREFIX=/home/jihong_min/codex-workspace-manorlords/ManorLordsSaveAnalysis/ooz-wine-prefix" "WINEDEBUG=-all" "/home/jihong_min/.local/share/Steam/steamapps/common/Proton 10.0/files/bin/wine64" "/home/jihong_min/Projects/MLMods/MLManorInsideEditor/build-xwin/snap_tests.exe")
set_tests_properties([=[snap_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jihong_min/Projects/MLMods/MLManorInsideEditor/CMakeLists.txt;68;add_test;/home/jihong_min/Projects/MLMods/MLManorInsideEditor/CMakeLists.txt;0;")
add_test([=[geometry_tag_tests]=] "/usr/bin/env" "WINEPREFIX=/home/jihong_min/codex-workspace-manorlords/ManorLordsSaveAnalysis/ooz-wine-prefix" "WINEDEBUG=-all" "/home/jihong_min/.local/share/Steam/steamapps/common/Proton 10.0/files/bin/wine64" "/home/jihong_min/Projects/MLMods/MLManorInsideEditor/build-xwin/geometry_tag_tests.exe")
set_tests_properties([=[geometry_tag_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jihong_min/Projects/MLMods/MLManorInsideEditor/CMakeLists.txt;77;add_test;/home/jihong_min/Projects/MLMods/MLManorInsideEditor/CMakeLists.txt;0;")
