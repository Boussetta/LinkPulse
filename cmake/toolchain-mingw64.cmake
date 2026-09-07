# Cross-compile Windows binaries from WSL with MinGW-w64.
#   sudo apt install mingw-w64
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
# Note: the resulting .exe cannot be run from WSL in a way that reports the
# Windows host's real traffic counters - copy it to Windows and run it there.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
