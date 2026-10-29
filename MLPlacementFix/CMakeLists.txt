cmake_minimum_required(VERSION 3.22)
project(MLPlacementFix LANGUAGES CXX)

if(NOT WIN32)
  message(
    FATAL_ERROR
      "MLPlacementFix requires a Windows x64 MSVC-compatible toolchain")
endif()

set(MANOR_SOURCES src/ManorLoadRuntime.cpp src/ManorLoadNative.cpp
                  src/ManorLoadPatch.cpp)
add_library(
  MLPlacementFix SHARED
  src/FenceLifecycle.cpp
  src/FenceLifecycleHooks.cpp
  src/MLPlacementFix.cpp
  src/MarketSelection.cpp
  src/HousePlacement.cpp
  src/PlacementRuntime.cpp
  src/PlacementHooks.cpp
  ${MANOR_SOURCES})
target_compile_features(MLPlacementFix PRIVATE cxx_std_23)
target_include_directories(MLPlacementFix PRIVATE include)
target_compile_options(MLPlacementFix PRIVATE /W4)
target_compile_definitions(MLPlacementFix PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_property(TARGET MLPlacementFix PROPERTY MSVC_RUNTIME_LIBRARY
                                            "MultiThreaded")
set(PACKAGE_MOD_DIR "${CMAKE_BINARY_DIR}/package/Mods/MLPlacementFix")
add_custom_command(
  TARGET MLPlacementFix
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_MOD_DIR}/dlls"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:MLPlacementFix>" "${PACKAGE_MOD_DIR}/dlls/main.dll"
  COMMAND "${CMAKE_COMMAND}" -E touch "${PACKAGE_MOD_DIR}/enabled.txt"
  VERBATIM)

option(ML_PLACEMENT_TESTS "Build offline runtime and binary-hook tests" ON)
if(ML_PLACEMENT_TESTS)
  enable_testing()
  add_executable(placement_runtime_tests tests/placement_runtime_tests.cpp
                                         src/PlacementRuntime.cpp)
  add_executable(house_placement_tests tests/house_placement_tests.cpp
                                       src/HousePlacement.cpp)
  add_executable(market_selection_tests tests/market_selection_tests.cpp
                                        src/MarketSelection.cpp)
  add_executable(
    fence_lifecycle_tests tests/fence_lifecycle_tests.cpp
                          src/FenceLifecycle.cpp src/FenceLifecycleHooks.cpp)
  add_executable(placement_hook_tests tests/placement_hook_tests.cpp
                                      src/PlacementHooks.cpp ${MANOR_SOURCES})
  add_executable(
    manor_load_tests
    tests/manor_load_tests.cpp tests/manor_load_runtime_tests.cpp
    tests/manor_load_bridge_tests.cpp ${MANOR_SOURCES})
  foreach(
    test IN
    ITEMS placement_runtime_tests house_placement_tests market_selection_tests
          placement_hook_tests manor_load_tests fence_lifecycle_tests)
    target_compile_features(${test} PRIVATE cxx_std_23)
    target_include_directories(${test} PRIVATE include)
    target_compile_options(${test} PRIVATE /W4)
    target_compile_definitions(${test} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_property(TARGET ${test} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  endforeach()
  add_test(NAME placement_runtime_tests COMMAND placement_runtime_tests)
  add_test(NAME house_placement_tests COMMAND house_placement_tests)
  add_test(NAME market_selection_tests COMMAND market_selection_tests)
  set(ML_PLACEMENT_GAME_EXE
      ""
      CACHE STRING "Read-only Windows path to supported game EXE")
  if(ML_PLACEMENT_GAME_EXE)
    add_test(NAME placement_hook_tests COMMAND placement_hook_tests
                                               "${ML_PLACEMENT_GAME_EXE}")
    add_test(NAME manor_load_tests COMMAND manor_load_tests
                                           "${ML_PLACEMENT_GAME_EXE}")
    add_test(NAME fence_lifecycle_tests COMMAND fence_lifecycle_tests
                                                "${ML_PLACEMENT_GAME_EXE}")
  endif()
endif()
