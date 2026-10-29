cmake_minimum_required(VERSION 3.22)
project(MLRoadEditor LANGUAGES CXX)
if(NOT WIN32)
  message(FATAL_ERROR "Build with a Windows x64 MSVC-compatible toolchain")
endif()
set(ROAD_SOURCES src/Geometry.cpp src/RoadPlacement.cpp src/Connections.cpp
                 src/ConnectionsReload.cpp src/CastlePreview.cpp)
add_library(MLRoadEditor SHARED src/MLRoadEditor.cpp ${ROAD_SOURCES})
target_compile_features(MLRoadEditor PRIVATE cxx_std_23)
target_include_directories(MLRoadEditor PRIVATE include)
target_compile_options(MLRoadEditor PRIVATE /W4)
target_compile_definitions(MLRoadEditor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_property(TARGET MLRoadEditor PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(PACKAGE_MOD_DIR "${CMAKE_BINARY_DIR}/package/Mods/MLRoadEditor")
add_custom_command(
  TARGET MLRoadEditor
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_MOD_DIR}/dlls"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:MLRoadEditor>"
          "${PACKAGE_MOD_DIR}/dlls/main.dll"
  COMMAND "${CMAKE_COMMAND}" -E touch "${PACKAGE_MOD_DIR}/enabled.txt"
  VERBATIM)
option(ML_ROAD_EDITOR_TESTS "Build offline verification tests" ON)
if(ML_ROAD_EDITOR_TESTS)
  enable_testing()
  add_executable(
    road_editor_tests
    tests/road_editor_tests.cpp tests/geometry_tests.cpp tests/stub_tests.cpp
    tests/connection_bridge_tests.cpp ${ROAD_SOURCES})
  add_executable(road_tests tests/road_tests.cpp src/RoadPlacement.cpp)
  add_executable(road_rule_bridge_tests tests/road_rule_bridge_tests.cpp)
  add_executable(geometry_tag_tests tests/geometry_tag_tests.cpp)
  add_executable(connection_tests tests/connection_tests.cpp
                                  src/Connections.cpp)
  add_executable(connection_reload_tests tests/connection_reload_tests.cpp
                                         src/ConnectionsReload.cpp)
  add_executable(castle_preview_tests tests/castle_preview_tests.cpp
                                      src/CastlePreview.cpp)
  foreach(
    test IN
    ITEMS road_editor_tests
          road_tests
          road_rule_bridge_tests
          geometry_tag_tests
          connection_tests
          connection_reload_tests
          castle_preview_tests)
    target_compile_features(${test} PRIVATE cxx_std_23)
    target_include_directories(${test} PRIVATE include)
    target_compile_options(${test} PRIVATE /W4)
    target_compile_definitions(${test} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_property(TARGET ${test} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  endforeach()
  foreach(test IN
          ITEMS road_tests road_rule_bridge_tests geometry_tag_tests
                connection_tests connection_reload_tests castle_preview_tests)
    add_test(NAME ${test} COMMAND ${test})
  endforeach()
  set(ML_ROAD_GAME_EXE
      ""
      CACHE STRING "Windows path to the supported game EXE (read-only)")
  if(ML_ROAD_GAME_EXE)
    add_test(NAME road_editor_tests COMMAND road_editor_tests
                                            "${ML_ROAD_GAME_EXE}")
  endif()
endif()
