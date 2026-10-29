cmake_minimum_required(VERSION 3.22)
project(MLTreeRegionFix LANGUAGES CXX)

if(NOT WIN32)
  message(
    FATAL_ERROR
      "MLTreeRegionFix requires a Windows x64 MSVC-compatible toolchain")
endif()

set(TREE_SOURCES src/TreeRuntime.cpp src/TreeHooks.cpp)
add_library(MLTreeRegionFix SHARED src/MLTreeRegionFix.cpp ${TREE_SOURCES})
target_compile_features(MLTreeRegionFix PRIVATE cxx_std_23)
target_include_directories(MLTreeRegionFix PRIVATE include)
target_compile_options(MLTreeRegionFix PRIVATE /W4)
target_compile_definitions(MLTreeRegionFix PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_property(TARGET MLTreeRegionFix PROPERTY MSVC_RUNTIME_LIBRARY
                                             "MultiThreaded")

set(PACKAGE_MOD_DIR "${CMAKE_BINARY_DIR}/package/Mods/MLTreeRegionFix")
add_custom_command(
  TARGET MLTreeRegionFix
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_MOD_DIR}/dlls"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:MLTreeRegionFix>" "${PACKAGE_MOD_DIR}/dlls/main.dll"
  COMMAND "${CMAKE_COMMAND}" -E touch "${PACKAGE_MOD_DIR}/enabled.txt"
  VERBATIM)

option(ML_TREE_REGION_FIX_TESTS "Build offline native-path verification tests"
       ON)
if(ML_TREE_REGION_FIX_TESTS)
  enable_testing()
  add_executable(
    tree_region_tests tests/tree_region_tests.cpp tests/tree_bridge_tests.cpp
                      tests/tree_profile_tests.cpp ${TREE_SOURCES})
  target_compile_features(tree_region_tests PRIVATE cxx_std_23)
  target_include_directories(tree_region_tests PRIVATE include)
  target_compile_options(tree_region_tests PRIVATE /W4)
  target_compile_definitions(tree_region_tests PRIVATE NOMINMAX
                                                       WIN32_LEAN_AND_MEAN)
  set_property(TARGET tree_region_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                                 "MultiThreaded")
  set(ML_TREE_GAME_EXE
      ""
      CACHE STRING
            "Windows path to supported game EXE for read-only verification")
  if(ML_TREE_GAME_EXE)
    add_test(NAME tree_region_tests COMMAND tree_region_tests
                                            "${ML_TREE_GAME_EXE}")
  endif()
endif()
