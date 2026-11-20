cmake_minimum_required(VERSION 3.28)

project(win-capture-audio VERSION 2.4.0 LANGUAGES CXX)

option(OBS_SDK_BUILD
       "Build against the local OBS SDK layout in deps/sdk (headers + libobs import library)"
       ON)

set(OBS_SDK_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/sdk"
    CACHE PATH "Local OBS SDK layout: <dir>/include and <dir>/lib/libobs.lib")

set(PLUGIN_VERSION "${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Version metadata
# ---------------------------------------------------------------------------

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT GIT_HASH)
  set(GIT_HASH "local")
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-macros.hpp.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/plugin-macros.generated.hpp" @ONLY)

# ---------------------------------------------------------------------------
# libobs
# ---------------------------------------------------------------------------

if(OBS_SDK_BUILD)
  if(NOT EXISTS "${OBS_SDK_DIR}/include/obs.h")
    message(
      FATAL_ERROR
        "OBS SDK not found in '${OBS_SDK_DIR}'. Run tools/prepare-sdk.ps1 first, or configure with -DOBS_SDK_BUILD=OFF."
    )
  endif()

  add_library(OBS::libobs SHARED IMPORTED)
  set_target_properties(
    OBS::libobs
    PROPERTIES IMPORTED_IMPLIB "${OBS_SDK_DIR}/lib/libobs.lib"
               IMPORTED_LOCATION "${OBS_SDK_DIR}/lib/obs.dll"
               INTERFACE_INCLUDE_DIRECTORIES "${OBS_SDK_DIR}/include")
else()
  find_package(libobs REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------

add_library(${PROJECT_NAME} MODULE
            src/plugin.cpp
            src/audio-capture.cpp
            src/audio-capture-helper.cpp
            src/session-monitor.cpp
            src/mixer.cpp
            src/win-capture-audio.rc)

target_include_directories(
  ${PROJECT_NAME}
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/deps/wil/include"
          "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs dwmapi psapi ksuser mmdevapi mfplat
                                             ole32 oleaut32 uuid)

set_target_properties(
  ${PROJECT_NAME}
  PROPERTIES CXX_STANDARD 23
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF
             PREFIX ""
             OUTPUT_NAME "${PROJECT_NAME}")

if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /W4 /permissive-)
  target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE
                                                  NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# Release layout (portable plugin zip)
# ---------------------------------------------------------------------------

set(RELEASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/release")

add_custom_command(
  TARGET ${PROJECT_NAME}
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${RELEASE_DIR}/obs-plugins/64bit"
          "${RELEASE_DIR}/data/obs-plugins/${PROJECT_NAME}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>"
          "${RELEASE_DIR}/obs-plugins/64bit/"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data"
          "${RELEASE_DIR}/data/obs-plugins/${PROJECT_NAME}"
  VERBATIM)
