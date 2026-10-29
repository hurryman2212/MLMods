cmake_minimum_required(VERSION 3.22)

project(MLCrashFix LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "MLCrashFix must be built for Windows")
endif()

add_library(
  MLCrashFix SHARED
  "${CMAKE_CURRENT_SOURCE_DIR}/src/MLCrashFix.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/CacheUpdateGuard.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/HomeReferenceGuard.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/MeleeAssignmentGuard.cpp")
target_compile_features(MLCrashFix PRIVATE cxx_std_23)
target_include_directories(MLCrashFix
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(MSVC)
  set_property(TARGET MLCrashFix PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

set(PACKAGE_MOD_DIR "${CMAKE_BINARY_DIR}/package/Mods/MLCrashFix")
set(PACKAGE_DLL_DIR "${PACKAGE_MOD_DIR}/dlls")

add_custom_command(
  TARGET MLCrashFix
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_DLL_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:MLCrashFix>"
          "${PACKAGE_DLL_DIR}/main.dll"
  COMMAND "${CMAKE_COMMAND}" -E touch "${PACKAGE_MOD_DIR}/enabled.txt"
  VERBATIM)

option(ML_CRASH_FIX_TESTS "Build offline crash-guard regression tests" ON)
if(ML_CRASH_FIX_TESTS)
  enable_testing()
  set(ML_CRASH_FIX_GAME_EXE
      ""
      CACHE STRING "Read-only Windows game EXE path")
  add_executable(cache_update_guard_tests tests/cache_update_guard_tests.cpp
                                          src/CacheUpdateGuard.cpp)
  target_compile_features(cache_update_guard_tests PRIVATE cxx_std_23)
  target_include_directories(cache_update_guard_tests PRIVATE include)
  target_compile_options(cache_update_guard_tests PRIVATE /W4)
  set_property(TARGET cache_update_guard_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                                        "MultiThreaded")
  add_executable(
    banner_guard_tests tests/banner_guard_tests.cpp src/CacheUpdateGuard.cpp
                       src/MeleeAssignmentGuard.cpp src/HomeReferenceGuard.cpp)
  target_compile_features(banner_guard_tests PRIVATE cxx_std_23)
  target_include_directories(banner_guard_tests PRIVATE include)
  set_property(TARGET banner_guard_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                                  "MultiThreaded")
  add_executable(
    melee_assignment_guard_tests tests/melee_assignment_guard_tests.cpp
                                 src/MeleeAssignmentGuard.cpp)
  target_compile_features(melee_assignment_guard_tests PRIVATE cxx_std_23)
  target_include_directories(melee_assignment_guard_tests PRIVATE include)
  target_compile_options(melee_assignment_guard_tests PRIVATE /W4)
  set_property(TARGET melee_assignment_guard_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                                            "MultiThreaded")
  add_executable(home_reference_guard_tests tests/home_reference_guard_tests.cpp
                                            src/HomeReferenceGuard.cpp)
  target_compile_features(home_reference_guard_tests PRIVATE cxx_std_23)
  target_include_directories(home_reference_guard_tests PRIVATE include)
  target_compile_options(home_reference_guard_tests PRIVATE /W4)
  set_property(TARGET home_reference_guard_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                                          "MultiThreaded")
  if(ML_CRASH_FIX_GAME_EXE)
    add_test(NAME cache_update_guard_tests COMMAND cache_update_guard_tests
                                                   "${ML_CRASH_FIX_GAME_EXE}")
    add_test(NAME banner_guard_tests COMMAND banner_guard_tests
                                             "${ML_CRASH_FIX_GAME_EXE}")
    add_test(NAME melee_assignment_guard_tests
             COMMAND melee_assignment_guard_tests "${ML_CRASH_FIX_GAME_EXE}")
    add_test(NAME home_reference_guard_tests COMMAND home_reference_guard_tests
                                                     "${ML_CRASH_FIX_GAME_EXE}")
  endif()
endif()
