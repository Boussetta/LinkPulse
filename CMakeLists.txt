cmake_minimum_required(VERSION 3.21)
set(LINKPULSE_VERSION "0.1.0" CACHE STRING "LinkPulse version")
if(NOT LINKPULSE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "LINKPULSE_VERSION must be MAJOR.MINOR.PATCH (e.g. 0.1.0); got '${LINKPULSE_VERSION}'")
endif()
project(linkpulse VERSION ${LINKPULSE_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(LINKPULSE_WERROR "Treat compiler warnings as errors" ON)
option(LINKPULSE_BUILD_TESTS "Build the test suite" ON)

# Shared warning configuration, consumed by every target.
add_library(lp_warnings INTERFACE)
if(MSVC)
  target_compile_options(lp_warnings INTERFACE /W4 $<$<BOOL:${LINKPULSE_WERROR}>:/WX>
    /experimental:c11atomics) # required for <stdatomic.h> (used in src/core/log.c)
  target_compile_definitions(lp_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(lp_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
    $<$<BOOL:${LINKPULSE_WERROR}>:-Werror>)
endif()

# --- core: portable C11 only, no OS headers ------------------------------------
add_library(lp_core STATIC
  src/core/config.c
  src/core/discovery.c
  src/core/format.c
  src/core/log.c
  src/core/net.c
  src/core/sampler.c
  src/core/status.c
)
target_include_directories(lp_core PUBLIC include)
target_link_libraries(lp_core PRIVATE lp_warnings)

# --- platform: the only place OS APIs are allowed ------------------------------
if(NOT WIN32)
  message(FATAL_ERROR
    "LinkPulse targets Windows. From WSL, cross-compile with:\n"
    "  cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake")
endif()

add_library(lp_platform STATIC
  src/platform/win32/activation_win32.c
  src/platform/win32/autostart_win32.c
  src/platform/win32/clock_win32.c
  src/platform/win32/config_win32.c
  src/platform/win32/discovery_win32.c
  src/platform/win32/log_win32.c
  src/platform/win32/net_win32.c
  src/platform/win32/notification_win32.c
  src/platform/win32/shortcut_win32.c
  src/platform/win32/update_win32.c
)
target_link_libraries(lp_platform PUBLIC lp_core PRIVATE lp_warnings)
target_link_libraries(lp_platform PUBLIC iphlpapi ole32 runtimeobject winhttp ws2_32)
# Windows 10/11 API surface (GetIfTable2, GetBestRoute2).
target_compile_definitions(lp_platform PUBLIC _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)

# --- executable ----------------------------------------------------------------
add_executable(linkpulse src/cli/main.c src/ui/tray_win32.c)
target_link_libraries(linkpulse PRIVATE lp_platform lp_warnings)
target_link_libraries(linkpulse PRIVATE shell32 gdi32 user32 advapi32)
# Single source of truth for the version string, consumed by --version.
target_compile_definitions(linkpulse PRIVATE LP_VERSION="${PROJECT_VERSION}")

# --- tray-only executable --------------------------------------------------
# A second, dedicated binary rather than making `linkpulse` itself GUI-subsystem:
# AttachConsole(ATTACH_PARENT_PROCESS) -- the standard way to keep CLI output
# working after switching subsystems -- does not see a usable console when
# launched through WSL interop (confirmed: it attaches "successfully" to a
# console invisible to the WSL terminal, and the raw inherited stdout handle
# is not valid either). That would have broken this project's whole dev/test
# loop. linkpulse-tray.exe instead takes no arguments and never touches stdio,
# so it never needs a console in the first place; linkpulse.exe (this target,
# console subsystem, unchanged) remains the CLI/debug tool, including its own
# --tray for debugging with visible log output.
add_executable(linkpulse-tray src/cli/tray_main.c src/ui/tray_win32.c)
target_link_libraries(linkpulse-tray PRIVATE lp_platform lp_warnings)
target_link_libraries(linkpulse-tray PRIVATE shell32 gdi32 user32 advapi32)
target_compile_definitions(linkpulse-tray PRIVATE LP_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_link_options(linkpulse-tray PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
else()
  target_link_options(linkpulse-tray PRIVATE -mwindows)
endif()

# Keep the debugger's program path independent of the selected generator.
set(LINKPULSE_DEPLOY_DIR "${CMAKE_SOURCE_DIR}/out/windows")
add_custom_command(TARGET linkpulse POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LINKPULSE_DEPLOY_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:linkpulse>"
    "${LINKPULSE_DEPLOY_DIR}/$<TARGET_FILE_NAME:linkpulse>"
  VERBATIM)

if(MSVC)
  # TARGET_PDB_FILE expands empty for configs without debug info (e.g. plain
  # Release), so the copy is delegated to a script that skips missing files.
  add_custom_command(TARGET linkpulse POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      "-DLP_COPY_SRC=$<TARGET_PDB_FILE:linkpulse>"
      "-DLP_COPY_DST=${LINKPULSE_DEPLOY_DIR}/$<TARGET_PDB_FILE_NAME:linkpulse>"
      -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_exists.cmake"
    VERBATIM)
endif()

add_custom_command(TARGET linkpulse-tray POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LINKPULSE_DEPLOY_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:linkpulse-tray>"
    "${LINKPULSE_DEPLOY_DIR}/$<TARGET_FILE_NAME:linkpulse-tray>"
  VERBATIM)

if(MSVC)
  add_custom_command(TARGET linkpulse-tray POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      "-DLP_COPY_SRC=$<TARGET_PDB_FILE:linkpulse-tray>"
      "-DLP_COPY_DST=${LINKPULSE_DEPLOY_DIR}/$<TARGET_PDB_FILE_NAME:linkpulse-tray>"
      -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_exists.cmake"
    VERBATIM)
endif()

if(LINKPULSE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# --- formatting ------------------------------------------------------------
find_program(LINKPULSE_CLANG_FORMAT NAMES
  clang-format
  clang-format-18 clang-format-17 clang-format-16 clang-format-15 clang-format-14
  clang-format-13 clang-format-12 clang-format-11 clang-format-10)
if(LINKPULSE_CLANG_FORMAT)
  file(GLOB_RECURSE LINKPULSE_FORMAT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/include/*.h"
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.h"
    "${CMAKE_SOURCE_DIR}/tests/*.c"
    "${CMAKE_SOURCE_DIR}/tests/*.h")

  add_custom_target(format
    COMMAND ${LINKPULSE_CLANG_FORMAT} --style=file --fallback-style=none -i ${LINKPULSE_FORMAT_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Formatting sources with clang-format"
    VERBATIM)

  add_custom_target(format-check
    COMMAND ${LINKPULSE_CLANG_FORMAT} --style=file --fallback-style=none --dry-run --Werror ${LINKPULSE_FORMAT_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking formatting with clang-format"
    VERBATIM)
else()
  message(STATUS "clang-format not found: 'format'/'format-check' targets unavailable")
endif()
