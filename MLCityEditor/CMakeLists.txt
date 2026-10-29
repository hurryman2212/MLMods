cmake_minimum_required(VERSION 3.22)
project(MLCityEditor LANGUAGES CXX)

if(NOT WIN32)
  message(
    FATAL_ERROR "MLCityEditor requires a Windows x64 MSVC-compatible toolchain")
endif()

set(CITY_SOURCES
    src/CityRuntime.cpp
    src/Construction.cpp
    src/Demolition.cpp
    src/Upgrade.cpp
    src/CityHooks.cpp
    src/Eligibility.cpp
    src/ExtensionCancel.cpp
    src/Maturity.cpp
    src/Retinue.cpp
    src/Windmill.cpp)
add_library(MLCityEditor SHARED src/MLCityEditor.cpp ${CITY_SOURCES})
target_compile_features(MLCityEditor PRIVATE cxx_std_23)
target_include_directories(MLCityEditor PRIVATE include)
target_compile_options(MLCityEditor PRIVATE /W4)
target_compile_definitions(MLCityEditor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_property(TARGET MLCityEditor PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(PACKAGE_MOD_DIR "${CMAKE_BINARY_DIR}/package/Mods/MLCityEditor")
file(MAKE_DIRECTORY "${PACKAGE_MOD_DIR}/scripts")
configure_file(scripts/main.lua "${PACKAGE_MOD_DIR}/scripts/main.lua" COPYONLY)
add_custom_command(
  TARGET MLCityEditor
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_MOD_DIR}/dlls"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:MLCityEditor>"
          "${PACKAGE_MOD_DIR}/dlls/main.dll"
  COMMAND "${CMAKE_COMMAND}" -E touch "${PACKAGE_MOD_DIR}/enabled.txt"
  VERBATIM)

option(ML_CITY_EDITOR_TESTS "Build offline native-path verification tests" ON)
if(ML_CITY_EDITOR_TESTS)
  enable_testing()
  add_executable(
    city_editor_tests
    tests/city_editor_tests.cpp
    tests/construction_tests.cpp
    tests/demolition_tests.cpp
    tests/upgrade_tests.cpp
    tests/patch_tests.cpp
    tests/bridge_tests.cpp
    tests/retinue_tests.cpp
    tests/eligibility_tests.cpp
    tests/extension_cancel_tests.cpp
    tests/maturity_tests.cpp
    tests/windmill_tests.cpp
    ${CITY_SOURCES})
  target_compile_features(city_editor_tests PRIVATE cxx_std_23)
  target_include_directories(city_editor_tests PRIVATE include)
  target_compile_options(city_editor_tests PRIVATE /W4)
  target_compile_definitions(city_editor_tests PRIVATE NOMINMAX
                                                       WIN32_LEAN_AND_MEAN)
  set_property(TARGET city_editor_tests PROPERTY MSVC_RUNTIME_LIBRARY
                                                 "MultiThreaded")
  set(ML_CITY_GAME_EXE
      ""
      CACHE STRING
            "Windows path to the supported game EXE for read-only verification")
  if(ML_CITY_GAME_EXE)
    add_test(NAME city_editor_tests COMMAND city_editor_tests
                                            "${ML_CITY_GAME_EXE}")
  endif()
  find_program(
    ML_CITY_LUA
    NAMES lua5.4 lua
    NO_CMAKE_FIND_ROOT_PATH)
  if(ML_CITY_LUA)
    add_test(
      NAME city_ui_tests
      COMMAND "${ML_CITY_LUA}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_tests.lua"
              "${CMAKE_CURRENT_SOURCE_DIR}/scripts/main.lua")
  endif()
endif()
