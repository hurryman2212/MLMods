cmake_minimum_required(VERSION 3.22)
project(MLAllRegionRoads LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "MLAllRegionRoads must be built for Windows")
endif()

add_library(MLAllRegionRoads SHARED src/MLAllRegionRoads.cpp)
target_compile_features(MLAllRegionRoads PRIVATE cxx_std_23)
target_include_directories(MLAllRegionRoads PRIVATE include)
set_property(TARGET MLAllRegionRoads PROPERTY MSVC_RUNTIME_LIBRARY
                                              "MultiThreaded")
set(PACKAGE_MOD_DIR "${CMAKE_BINARY_DIR}/package/Mods/MLAllRegionRoads")
add_custom_command(
  TARGET MLAllRegionRoads
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_MOD_DIR}/dlls"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:MLAllRegionRoads>" "${PACKAGE_MOD_DIR}/dlls/main.dll"
  COMMAND "${CMAKE_COMMAND}" -E touch "${PACKAGE_MOD_DIR}/enabled.txt"
  VERBATIM)

option(ML_ALL_REGION_ROADS_TESTS "Build offline Windows verification tests" OFF)
if(ML_ALL_REGION_ROADS_TESTS)
  add_executable(road_patch_tests tests/road_patch_tests.cpp)
  target_compile_features(road_patch_tests PRIVATE cxx_std_23)
  target_include_directories(road_patch_tests PRIVATE include)
  set_property(TARGET road_patch_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                                "MultiThreaded")
endif()
