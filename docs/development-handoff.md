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

## Device activity and risk detection

`include/linkpulse/activity.h` defines the router/sensor-facing flow observation contract.
`src/core/activity.c` can assess one device over a rolling time window for high destination
fan-out, horizontal port scans, and vertical port scans. The core detector is implemented and
unit-tested, but it is deliberately not wired to discovery: ARP/NDP entries contain no byte or
connection telemetry.

A gateway adapter must provide cumulative per-client RX/TX counters for upload/download rates
and outbound `(source IP, destination IP, destination port, timestamp)` observations for risk
analysis. Counter deltas must be computed at the adapter boundary, handling reset/wrap and device
identity changes. Service classification can later enrich these observations with DNS, TLS SNI,
or destination ASN evidence, each carrying an explicit confidence level.

## Independent packet capture

An independent capture backend can use Npcap's libpcap-compatible API, but it cannot be assumed
to see another Wi-Fi client's traffic. A normal Windows client is not on the forwarding path for
peer-to-router unicast frames; promiscuous mode is driver-dependent, and monitor mode usually
requires a compatible adapter and may still be blocked by the access point. The current machine's
Intel Wi-Fi 7 BE201 exposes neither a capture library nor a monitor-mode capability, so LinkPulse
must report packet capture as unavailable rather than infer activity from its own adapter rate.

The future backend should be optional and should:

- load Npcap/libpcap only when installed, without making the base build depend on it;
- enumerate capture interfaces and report whether monitor/promiscuous capture is available;
- filter LAN IPv4/IPv6 traffic by the selected device's IP/MAC;
- derive byte counters, DNS names, TLS hostnames where visible, destination ASN, and flow-risk
	observations without storing packet payloads;
- show explicit states such as `capture unavailable`, `capture sees local host only`, and
	`encrypted source unknown`.

The current preview is opt-in: it only runs after the user enables "Capture device traffic
(Npcap)" in the tray menu (persisted as `packet_capture=1`). It loads `wpcap.dll` from the system
Npcap directory only, opens the adapter in non-promiscuous mode, and captures 96-byte headers.

LinkPulse installs per user and runs without elevation, but Npcap can be installed with
"Restrict Npcap driver's access to Administrators only" (registry value
`HKLM\SYSTEM\CurrentControlSet\Services\npcap\Parameters\AdminOnly=1`). In that mode a
non-elevated process sees no adapters or gets "Access is denied" when opening one. The capture
backend detects this, from the registry value combined with the process token's elevation or from
the access-denied error, and sets `lp_capture_activity_t.restricted_to_admins`. Device details
then shows "Npcap needs administrator rights" instead of a generic failure. LinkPulse never
requests elevation itself.

Each captured packet is also checked against this PC's own unicast addresses
(`local_packets`). When every packet was exchanged with this PC, `sees_only_local_host` is set
and device details shows "Capture sees only traffic with this PC". That is the expected result on
a Wi-Fi client, and it must not be presented as the device's overall activity.

Installing Npcap alone is not sufficient for this use case. A supported monitor-mode adapter or a
capture point on the gateway/bridge is required to observe other devices reliably.

### Dependency policy

LinkPulse may vendor a permissively licensed packet parser or libpcap-compatible API and keep it
inside an optional capture module. That library parses packets; it does not create a Windows
capture path. Windows still needs either a compatible capture driver, a monitor-mode adapter, or
traffic delivered from a bridge/sensor. The base LinkPulse build must therefore remain driver-free
and continue to build and run without the optional module.

Do not copy a Wireshark installation or silently install a kernel driver. Any future installer
support must identify the driver separately, preserve its license/notice files, request the
required administrator consent, and make capture opt-in. A statically linked public parser alone
cannot make the Pixel's traffic visible from this PC.

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
