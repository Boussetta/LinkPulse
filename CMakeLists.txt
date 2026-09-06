cmake_minimum_required(VERSION 3.16)
project(linkpulse VERSION 0.1.0 LANGUAGES C)

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
  target_compile_options(lp_warnings INTERFACE /W4 $<$<BOOL:${LINKPULSE_WERROR}>:/WX>)
  target_compile_definitions(lp_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(lp_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
    $<$<BOOL:${LINKPULSE_WERROR}>:-Werror>)
endif()

# --- core: portable C11 only, no OS headers ------------------------------------
add_library(lp_core STATIC
  src/core/log.c
  src/core/net.c
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
  src/platform/win32/clock_win32.c
  src/platform/win32/net_win32.c
)
target_link_libraries(lp_platform PUBLIC lp_core PRIVATE lp_warnings)
target_link_libraries(lp_platform PUBLIC iphlpapi ws2_32)
# Windows 10/11 API surface (GetIfTable2, GetBestRoute2).
target_compile_definitions(lp_platform PUBLIC _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)

# --- executable ----------------------------------------------------------------
add_executable(linkpulse src/cli/main.c)
target_link_libraries(linkpulse PRIVATE lp_platform lp_warnings)

if(LINKPULSE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
