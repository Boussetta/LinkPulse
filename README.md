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

This deploys both `linkpulse.exe` and `linkpulse-tray.exe` to `out/windows/`. Run
from the Windows side — WSL2 sits behind a NAT'd virtual NIC, so counters read
from inside WSL are not the host's real internet traffic.

`linkpulse.exe` is the CLI (`--list`, `--watch`, and a debug-friendly `--tray`
with visible log output). `linkpulse-tray.exe` is a separate, argument-free,
GUI-subsystem binary meant for real use and autostart — it never allocates or
attaches a console, so it never flashes one on launch. They're two binaries
rather than one dual-mode executable because `AttachConsole` (the usual way to
keep CLI output working after switching an exe to the GUI subsystem) does not
see a usable console when the process is launched through WSL interop, which
would have broken this project's whole dev/test workflow.

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

## Packaging

### Installer (Inno Setup — primary, free)

`packaging/LinkPulse.iss` builds a normal `LinkPulseSetup.exe`: install to
`%LocalAppData%\Programs\LinkPulse` (no admin/UAC needed), Start Menu
shortcut, optional desktop shortcut, and an uninstaller registered in
"Add/Remove Programs" that also cleans up the autostart registry entry if it
was enabled. Requires [Inno Setup](https://jrsoftware.org/isdl.php) on
Windows:

```powershell
cmake --preset msvc
cmake --build --preset msvc-release
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" packaging\LinkPulse.iss
```

Produces `build\installer\LinkPulseSetup.exe`.

### Hosting releases on GitHub

The `Release` workflow builds the MSVC Release binaries, runs the tests,
compiles the Inno Setup installer, and attaches `LinkPulseSetup.exe` to a
GitHub Release. To publish a version, push a tag from `main`:

```powershell
git tag v0.1.0
git push origin v0.1.0
```

The finished installer is then available from the repository's **Releases**
page. GitHub provides the hosting and download bandwidth for free.

### Update detection

The tray app checks the public GitHub Releases API in the background at startup.
When a newer semantic version is available, the tooltip reports it and the
context menu offers **Download update**, opening the GitHub release page. The
app never replaces its own executable or installs updates silently.

### MSIX (Microsoft Store — deferred)

`packaging/` also contains a `Package.appxmanifest`, placeholder tile art
under `Assets/` (see [packaging/Assets/README.md](packaging/Assets/README.md)),
and `build-msix.ps1` for a future Store submission. This path needs a paid
Partner Center developer account and is deliberately not the near-term
priority — see [ROADMAP.md](ROADMAP.md). Run on Windows, from a Developer
PowerShell:

```powershell
cmake --preset msvc
cmake --build --preset msvc-release
.\packaging\build-msix.ps1
```

Before a real Store submission, replace the placeholder `Publisher`/
`PublisherDisplayName` in the manifest with the values from your Partner
Center app reservation, and replace the placeholder tile art.

## License

MIT — see [LICENSE](LICENSE).


