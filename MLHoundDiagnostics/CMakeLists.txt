cmake_minimum_required(VERSION 3.22)
project(MLHoundDiagnostics LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Build with a Windows x64 MSVC-compatible toolchain")
endif()

add_library(MLHoundDiagnostics SHARED src/MLHoundDiagnostics.cpp)
target_compile_features(MLHoundDiagnostics PRIVATE cxx_std_23)
target_include_directories(MLHoundDiagnostics PRIVATE include)
target_compile_options(MLHoundDiagnostics PRIVATE /W4)
target_compile_definitions(MLHoundDiagnostics PRIVATE NOMINMAX
                                                      WIN32_LEAN_AND_MEAN)
set_property(TARGET MLHoundDiagnostics PROPERTY MSVC_RUNTIME_LIBRARY
                                                "MultiThreaded")
set(PACKAGE_MOD_DIR "${CMAKE_BINARY_DIR}/package/Mods/MLHoundDiagnostics")
add_custom_command(
  TARGET MLHoundDiagnostics
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_MOD_DIR}/dlls"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:MLHoundDiagnostics>" "${PACKAGE_MOD_DIR}/dlls/main.dll"
  COMMAND "${CMAKE_COMMAND}" -E touch "${PACKAGE_MOD_DIR}/enabled.txt"
  VERBATIM)

option(ML_HOUND_DIAGNOSTICS_TESTS "Build offline diagnostics and hook tests" ON)
if(ML_HOUND_DIAGNOSTICS_TESTS)
  enable_testing()
  add_executable(diagnostics_tests tests/test_diagnostics.cpp)
  target_compile_features(diagnostics_tests PRIVATE cxx_std_23)
  target_include_directories(diagnostics_tests PRIVATE include)
  target_compile_options(diagnostics_tests PRIVATE /W4)
  target_compile_definitions(diagnostics_tests PRIVATE NOMINMAX
                                                       WIN32_LEAN_AND_MEAN)
  set_property(TARGET diagnostics_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                                 "MultiThreaded")
  add_test(NAME diagnostics_tests COMMAND diagnostics_tests)
  add_executable(native_tests tests/test_native.cpp)
  target_compile_features(native_tests PRIVATE cxx_std_23)
  target_include_directories(native_tests PRIVATE include)
  target_compile_options(native_tests PRIVATE /W4)
  target_compile_definitions(native_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  set_property(TARGET native_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                            "MultiThreaded")
  set(ML_HOUND_GAME_EXE
      ""
      CACHE STRING
            "Windows path to supported game EXE, read-only offline test input")
  if(ML_HOUND_GAME_EXE)
    add_test(NAME native_tests COMMAND native_tests "${ML_HOUND_GAME_EXE}")
  endif()
endif()
