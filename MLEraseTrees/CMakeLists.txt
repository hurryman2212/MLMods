cmake_minimum_required(VERSION 3.22)
project(MLEraseTrees LANGUAGES CXX)

if(NOT WIN32)
  message(
    FATAL_ERROR "MLEraseTrees requires a Windows x64 MSVC-compatible toolchain")
endif()

set(ERASER_SOURCES src/EraserRuntime.cpp src/EraserHooks.cpp)
add_library(MLEraseTrees SHARED src/MLEraseTrees.cpp ${ERASER_SOURCES})
target_compile_features(MLEraseTrees PRIVATE cxx_std_23)
target_include_directories(MLEraseTrees PRIVATE include)
target_compile_options(MLEraseTrees PRIVATE /W4)
target_compile_definitions(MLEraseTrees PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_property(TARGET MLEraseTrees PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(PACKAGE_MOD_DIR "${CMAKE_BINARY_DIR}/package/Mods/MLEraseTrees")
add_custom_command(
  TARGET MLEraseTrees
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_MOD_DIR}/dlls"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:MLEraseTrees>"
          "${PACKAGE_MOD_DIR}/dlls/main.dll"
  COMMAND "${CMAKE_COMMAND}" -E touch "${PACKAGE_MOD_DIR}/enabled.txt"
  VERBATIM)

option(ML_ERASE_TREES_TESTS "Build offline native-path verification tests" ON)
if(ML_ERASE_TREES_TESTS)
  enable_testing()
  add_executable(
    eraser_tests tests/eraser_tests.cpp tests/eraser_bridge_tests.cpp
                 tests/eraser_profile_tests.cpp ${ERASER_SOURCES})
  target_compile_features(eraser_tests PRIVATE cxx_std_23)
  target_include_directories(eraser_tests PRIVATE include)
  target_compile_options(eraser_tests PRIVATE /W4)
  target_compile_definitions(eraser_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  set_property(TARGET eraser_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                            "MultiThreaded")
  set(ML_ERASER_GAME_EXE
      ""
      CACHE STRING
            "Windows path to supported game EXE for read-only verification")
  if(ML_ERASER_GAME_EXE)
    add_test(NAME eraser_tests COMMAND eraser_tests "${ML_ERASER_GAME_EXE}")
  endif()
endif()
