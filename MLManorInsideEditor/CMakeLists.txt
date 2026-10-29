cmake_minimum_required(VERSION 3.22)
project(MLManorInsideEditor LANGUAGES CXX)

if(NOT WIN32)
  message(
    FATAL_ERROR "Build the mod with a Windows x64 MSVC-compatible toolchain")
endif()

add_library(
  MLManorInsideEditor SHARED src/MLManorInsideEditor.cpp src/Geometry.cpp
                             src/Placement.cpp src/Snap.cpp)
target_compile_features(MLManorInsideEditor PRIVATE cxx_std_23)
target_include_directories(MLManorInsideEditor PRIVATE include)
target_compile_options(MLManorInsideEditor PRIVATE /W4)
target_compile_definitions(MLManorInsideEditor PRIVATE NOMINMAX
                                                       WIN32_LEAN_AND_MEAN)
set_property(TARGET MLManorInsideEditor PROPERTY MSVC_RUNTIME_LIBRARY
                                                 "MultiThreaded")

set(PACKAGE_MOD_DIR "${CMAKE_BINARY_DIR}/package/Mods/MLManorInsideEditor")
add_custom_command(
  TARGET MLManorInsideEditor
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_MOD_DIR}/dlls"
  COMMAND
    "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:MLManorInsideEditor>"
    "${PACKAGE_MOD_DIR}/dlls/main.dll"
  COMMAND "${CMAKE_COMMAND}" -E touch "${PACKAGE_MOD_DIR}/enabled.txt"
  VERBATIM)

option(ML_MANOR_EDITOR_TESTS "Build offline verification tests" ON)
if(ML_MANOR_EDITOR_TESTS)
  enable_testing()
  add_executable(
    manor_editor_tests
    tests/manor_editor_tests.cpp tests/geometry_tests.cpp tests/stub_tests.cpp
    src/Geometry.cpp src/Placement.cpp src/Snap.cpp)
  target_compile_features(manor_editor_tests PRIVATE cxx_std_23)
  target_include_directories(manor_editor_tests PRIVATE include)
  target_compile_options(manor_editor_tests PRIVATE /W4)
  target_compile_definitions(manor_editor_tests PRIVATE NOMINMAX
                                                        WIN32_LEAN_AND_MEAN)
  set_property(TARGET manor_editor_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                                  "MultiThreaded")
  set(ML_MANOR_GAME_EXE
      ""
      CACHE STRING
            "Windows path to the supported game EXE (read-only test input)")
  if(ML_MANOR_GAME_EXE)
    add_test(NAME manor_editor_tests COMMAND manor_editor_tests
                                             "${ML_MANOR_GAME_EXE}")
  endif()
  add_executable(placement_tests tests/placement_tests.cpp src/Placement.cpp)
  target_compile_features(placement_tests PRIVATE cxx_std_23)
  target_include_directories(placement_tests PRIVATE include)
  target_compile_definitions(placement_tests PRIVATE NOMINMAX
                                                     WIN32_LEAN_AND_MEAN)
  target_compile_options(placement_tests PRIVATE /W4)
  set_property(TARGET placement_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                               "MultiThreaded")
  add_test(NAME placement_tests COMMAND placement_tests)
  add_executable(snap_tests tests/snap_tests.cpp src/Snap.cpp)
  target_compile_features(snap_tests PRIVATE cxx_std_23)
  target_include_directories(snap_tests PRIVATE include)
  target_compile_definitions(snap_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(snap_tests PRIVATE /W4)
  set_property(TARGET snap_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  add_test(NAME snap_tests COMMAND snap_tests)
  add_executable(geometry_tag_tests tests/geometry_tag_tests.cpp)
  target_compile_features(geometry_tag_tests PRIVATE cxx_std_23)
  target_include_directories(geometry_tag_tests PRIVATE include)
  target_compile_definitions(geometry_tag_tests PRIVATE NOMINMAX
                                                        WIN32_LEAN_AND_MEAN)
  target_compile_options(geometry_tag_tests PRIVATE /W4)
  set_property(TARGET geometry_tag_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                                  "MultiThreaded")
  add_test(NAME geometry_tag_tests COMMAND geometry_tag_tests)
endif()
