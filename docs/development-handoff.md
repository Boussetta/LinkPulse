# LinkPulse Development Handoff

This document is the durable handoff for moving LinkPulse between WSL and native Windows. The repository, Git history, and GitHub pull requests are the source of truth; chat history is optional context.

## Current baseline

- Default branch: `main`
- Baseline: run `git log -1 --oneline main` to confirm the current tip before editing
- Current focus: M4 network discovery and the next gateway integration slice
- Primary runtime: Windows 10/11
- Native build: MSVC x64 through CMake
- Cross-build: MinGW-w64 from WSL

Check the current state before making changes:

```powershell
git switch main
git pull --ff-only
git status
gh pr list --state open
```

Use native `git` for branches, commits, pushes, and merges. Do not use GitKraken for repository operations.

## Native Windows setup

Install:

- Git for Windows
- Visual Studio 2022 or Build Tools with **Desktop development with C++**
- Windows 10/11 SDK
- CMake 3.21 or newer
- VS Code with the C/C++ and CMake Tools extensions
- Inno Setup 6 only when building the installer

Open the repository in Windows VS Code. The `msvc` CMake preset is Windows-only and uses Visual Studio 17 2022 x64.

Configure, build, and test:

```powershell
cmake --preset msvc
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

For a release build:

```powershell
cmake --build --preset msvc-release
ctest --preset msvc-release
```

The build copies the console executable and tray executable to `out/windows/`.

## Native debugging

The checked-in VS Code launch profiles use `cppvsdbg` and build MSVC Debug before launch:

- `LinkPulse: tray, default interface (MSVC Debug)` runs `linkpulse.exe --tray` with console output.
- `LinkPulse: tray Wi-Fi bits (MSVC Debug)` runs a selected interface configuration.
- `LinkPulse: list interfaces (MSVC Debug)` validates adapter enumeration.
- `LinkPulse: watch (MSVC Debug)` validates sampler timing and counters.
- `LinkPulse (MSVC Debug)` runs the normal CLI.

For the real GUI tray binary, run the generated executable from PowerShell:

```powershell
.\out\windows\linkpulse-tray.exe
```

Useful runtime locations:

- Log: `%LOCALAPPDATA%\LinkPulse\LinkPulse.log`
- Config: `%APPDATA%\LinkPulse\config.ini`
- Installer output: `build\installer\LinkPulseSetup.exe`

Native Windows is required for reliable debugging of tray behavior, WinRT notifications, ARP/NDP state, gateway selection, Winsock name resolution, and the network map. WSL can compile and test portable logic, but its virtual NIC does not represent the Windows host network.

## WSL validation

WSL is useful for fast cross-compilation and portable tests:

```bash
cmake --preset mingw-cross-release
cmake --build --preset mingw-cross-release -j2
ctest --test-dir build/mingw-release --output-on-failure
```

Run the generated `.exe` on the Windows side, not inside WSL, when testing real tray or network behavior.

## Discovery behavior

The current discovery layer combines:

- `GetIpNetTable2`: passive ARP/NDP neighbors
- `GetAdaptersAddresses`: local addresses and interface prefixes
- `GetBestRoute2`: default IPv4 gateway and owning interface
- Reverse DNS: best-effort hostnames
- Hostname heuristics: conservative device type/vendor/confidence hints
- Retained in-memory identity: transient cache misses do not immediately delete a device

Passive ARP/NDP is incomplete. Quiet Wi-Fi devices, including phones and watches, may be absent even while connected. The next authoritative source is a gateway adapter, beginning with Fritz!Box connected-device APIs. Keep gateway access optional and isolated behind an interface so unsupported routers continue to work.

## Safe handoff to a new chat or environment

Paste this short context when starting a new session:

```text
Continue LinkPulse from the repository state, not from chat memory.
OS: Windows or WSL
Branch: main (or name the active feature branch)
Latest merged PR: 45
Run git status and git log before editing.
Use native git only; do not use GitKraken.
Read docs/development-handoff.md, README.md, and ROADMAP.md.
Validate with the relevant CMake preset and CTest after edits.
```

Then identify the active work from GitHub:

```powershell
gh pr list --state open --limit 20
gh issue list --limit 20
```

Do not assume a later branch push was included in a merged PR. After a merge, verify it explicitly:

```bash
git fetch origin
git merge-base --is-ancestor origin/<feature-branch> origin/main
```

## Contribution workflow

Create one branch per coherent concern and prefer small commits:

```bash
git switch main
git pull --ff-only
git switch -c feat/example
git add <files>
git commit -m "feat: describe the change"
git push -u origin feat/example
gh pr create --base main --head feat/example
```

Run the focused test first, then the full build and test suite before opening or updating a PR.
