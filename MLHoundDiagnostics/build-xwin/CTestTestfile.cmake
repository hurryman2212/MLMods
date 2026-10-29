# CMake generated Testfile for 
# Source directory: /home/jihong_min/Projects/MLMods/MLHoundDiagnostics
# Build directory: /home/jihong_min/Projects/MLMods/MLHoundDiagnostics/build-xwin
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[diagnostics_tests]=] "/home/jihong_min/.local/share/Steam/steamapps/common/Proton 11.0/files/bin/wine" "/home/jihong_min/Projects/MLMods/MLHoundDiagnostics/build-xwin/diagnostics_tests.exe")
set_tests_properties([=[diagnostics_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jihong_min/Projects/MLMods/MLHoundDiagnostics/CMakeLists.txt;37;add_test;/home/jihong_min/Projects/MLMods/MLHoundDiagnostics/CMakeLists.txt;0;")
add_test([=[native_tests]=] "/home/jihong_min/.local/share/Steam/steamapps/common/Proton 11.0/files/bin/wine" "/home/jihong_min/Projects/MLMods/MLHoundDiagnostics/build-xwin/native_tests.exe" "Z:\\home\\jihong_min\\.local\\share\\Steam\\steamapps\\common\\Manor Lords\\ManorLords\\Binaries\\Win64\\ManorLords-Win64-Shipping.exe")
set_tests_properties([=[native_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jihong_min/Projects/MLMods/MLHoundDiagnostics/CMakeLists.txt;50;add_test;/home/jihong_min/Projects/MLMods/MLHoundDiagnostics/CMakeLists.txt;0;")
