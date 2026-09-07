# LinkPulse

A lightweight Windows 11 system tray monitor for local network activity.
Written in C11, no runtime dependencies.

Current status: **Milestone 0/1** — see [ROADMAP.md](ROADMAP.md).

## Building

### On Windows (primary target)

Requires Visual Studio 2022 Build Tools and CMake.

```powershell
cmake -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\linkpulse.exe --list
```

### Cross-compiling from WSL

```bash
sudo apt install mingw-w64 cmake ninja-build
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
cmake --build build
```

This deploys `linkpulse.exe` to `out/windows/linkpulse.exe`. Run that binary from
the Windows side — WSL2 sits behind a NAT'd virtual NIC, so counters read from
inside WSL are not the host's real internet traffic.

## Layout

```
include/linkpulse/   public headers = module boundaries
src/core/            portable C11 only, no OS headers - all unit-testable logic
src/platform/win32/  the only place Win32 APIs are called
src/cli/             headless mode, used for development and validation
tests/               CTest
```

`src/core` must never include `windows.h`. Keeping that boundary is what makes
the logic testable without a live network.

## License

MIT — see [LICENSE](LICENSE).
