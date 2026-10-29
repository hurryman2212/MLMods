cmake_minimum_required(VERSION 3.22)

project(MLSetRegionResource LANGUAGES CXX)

set(POLYHOOK_ROOT
    ""
    CACHE
      PATH
      "Path to PolyHook 2 at commit 298d56210b9d9e66cde8f96481d6053925c6ae15")
if(NOT WIN32)
  message(FATAL_ERROR "MLSetRegionResource must be built for Windows")
endif()

if(NOT POLYHOOK_ROOT)
  message(FATAL_ERROR "Set POLYHOOK_ROOT to the PolyHook 2 source tree")
endif()

set(POLYHOOK_BUILD_STATIC_RUNTIME
    ON
    CACHE BOOL "Use a private CRT to avoid the game's legacy MSVC runtime"
          FORCE)
set(POLYHOOK_FEATURE_EXCEPTION
    OFF
    CACHE BOOL "Disable unused exception hooks" FORCE)
set(POLYHOOK_FEATURE_INLINENTD
    OFF
    CACHE BOOL "Disable unused inline-N detours" FORCE)
set(POLYHOOK_FEATURE_PE
    OFF
    CACHE BOOL "Disable unused PE hooks" FORCE)
set(POLYHOOK_FEATURE_VIRTUALS
    OFF
    CACHE BOOL "Disable unused virtual hooks" FORCE)
add_subdirectory("${POLYHOOK_ROOT}" "${CMAKE_BINARY_DIR}/PolyHook_2")
set(TARGET MLSetRegionResource)

add_library(
  ${TARGET} SHARED "${CMAKE_CURRENT_SOURCE_DIR}/src/Config.cpp"
                   "${CMAKE_CURRENT_SOURCE_DIR}/src/MLSetRegionResource.cpp")

target_compile_features(${TARGET} PRIVATE cxx_std_23)
target_include_directories(${TARGET}
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${TARGET} PRIVATE PolyHook_2)

if(MSVC)
  set_property(TARGET ${TARGET} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

set(PACKAGE_MOD_DIR "${CMAKE_BINARY_DIR}/package/Mods/${TARGET}")
set(PACKAGE_DLL_DIR "${PACKAGE_MOD_DIR}/dlls")

add_custom_command(
  TARGET ${TARGET}
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_DLL_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${TARGET}>"
          "${PACKAGE_DLL_DIR}/main.dll"
  COMMAND
    "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/config.ini" "${PACKAGE_MOD_DIR}/config.ini"
  COMMAND "${CMAKE_COMMAND}" -E touch "${PACKAGE_MOD_DIR}/enabled.txt"
  VERBATIM)
