# LinkPulse — Roadmap

Local network activity monitor with a system tray UI. **Target: Windows 11.**

## Technical decisions

| Topic | Decision | Notes |
| --- | --- | --- |
| Language | **C11** | No GC, single native binary, no runtime to ship |
| Build | **CMake** + MSVC (primary), MinGW-w64 (WSL cross-build) | `cmake -B build && cmake --build build` |
| Counters | `GetIfTable2()` from `iphlpapi` | 64-bit counters, no privileges required |
| Default route | `GetBestRoute2()` | Identifies the internet-facing NIC |
| Tray | `Shell_NotifyIcon` + hidden message-only window | Icon built with GDI into a `CreateDIBSection`, then `CreateIconIndirect` |
| Deps policy | Keep optional integrations isolated; add SQLite, mDNS, and gateway adapters only when their runtime and licensing costs are understood | Keeps the base build small and testable |
| Tests | Plain C asserts driven by CTest | No framework dependency |
| API level | `_WIN32_WINNT=0x0A00` | Windows 10/11 only; no legacy fallbacks |

### Development workflow from WSL and Windows

WSL2 cannot call `Shell_NotifyIcon` and its virtual NIC is behind NAT, so its counters are
not the host's real traffic. Two options, both supported by the build:

- Cross-compile with MinGW-w64 from WSL, then run the `.exe` on the Windows side.
- Build natively on Windows with MSVC (required once resource scripts and toasts land).

Core logic stays free of `windows.h` so it can be unit-tested anywhere.

For native Windows setup, MSVC debugging, WSL validation, and environment handoff, see
[`docs/development-handoff.md`](docs/development-handoff.md).

## Architecture

```
include/linkpulse/   public headers = the module boundaries
src/core/            portable C11 only: rate math, ring buffer, formatting, config, event bus
src/platform/win32/  iphlpapi, GDI, clock, toasts, registry
src/ui/              tray window, menu, icon rendering
src/cli/             headless mode: print rates to stdout (the dev/validation target)
src/core/            discovery diffing and retained device identity
src/platform/win32/  ARP/NDP, gateway, hostname, and interface discovery
tests/               CTest
```

Rules:

- `src/core` includes **no** OS headers. It is where all unit-testable logic lives.
- All Win32 calls sit behind the headers in `include/linkpulse/`, so the logic can be driven
  by fake counter sources in tests.
- `core` never calls into `ui`. The UI consumes snapshots and events so later features (gateway adapters, known-device history, alerts) plug in without touching the sampler.

---

## Milestone 0 — Project scaffolding

- [x] CMake build, C11, warnings-as-errors (`/W4` · `-Wall -Wextra -Wconversion`)
- [x] Directory layout + public headers defining the platform boundary
- [x] `.gitignore`, `README.md` with build instructions
- [x] CTest harness with a tiny assert helper
- [x] MinGW-w64 cross-compile toolchain file for WSL
- [x] GitHub Actions: MSVC build + tests, plus a MinGW cross-build
- [x] `clang-format` config + a `format` target

## Milestone 1 — Bitrate engine (headless, testable)

- [x] `lp_net_snapshot()` via `GetIfTable2()`
- [x] `lp_net_default_iface()` via `GetBestRoute2()`
- [x] `lp_clock_monotonic_ns()` via `QueryPerformanceCounter`
- [x] `linkpulse --list` dumps interfaces and counters
- [x] `lp_sampler`: fixed-interval polling, rate = Δbytes / Δt on the monotonic clock
- [x] Handle counter wraparound and NICs appearing/disappearing mid-run
- [x] Interface selection: auto (default route), manual override, or sum-of-all
- [x] Filter loopback and virtual adapters (WSL vEthernet, Hyper-V, VPN) — configurable
- [x] Formatting: b/s → Kb/s → Mb/s → Gb/s, with a bits/bytes toggle
- [x] Fixed-capacity ring buffer of the last N samples (feeds the M6 sparkline)
- [x] `linkpulse --watch` prints live up/down rates once per second
- [x] Unit tests against an injected fake counter source (no real network needed)

## Milestone 2 — System tray application (the first real deliverable)

- [x] Hidden window + `Shell_NotifyIcon` with `NIF_ICON | NIF_MESSAGE | NIF_TIP` (a real hidden
      window, not `HWND_MESSAGE`: message-only windows don't reliably dismiss `TrackPopupMenu`)
- [x] Render rate text with GDI into a 32bpp `CreateDIBSection`, convert via
      `CreateIconIndirect`, and **`DestroyIcon` the previous icon every tick** — the classic
      GDI leak in this design
- [x] Handle `TaskbarCreated` (registered window message) to re-add the icon after Explorer restarts
- [ ] DPI awareness manifest; pick 16/20/24 px icons from `GetSystemMetrics(SM_CXSMICON)` --
      currently always uses `SM_CXSMICON` with no manifest, untested at non-100% scaling
- [x] Context menu (`TrackPopupMenu`): pause/resume, units toggle, quit -- interface selection
      punted to the M3 settings dialog rather than a growing context menu
- [x] Threading model: sampler on its own thread; result handed off through a
      `CRITICAL_SECTION` (simpler and sufficient here since the UI thread only ever *reads*
      the latest sample on a timer, rather than needing `PostMessage`'s ordered delivery)
- [x] Graceful shutdown, single-instance lock via a named mutex
- [ ] Smoke test: run a big download, confirm numbers match Task Manager -- needs verification
      on native Windows; only confirmed the process starts and stays alive from WSL

## Milestone 3 — Configuration, persistence & polish

- [x] Config file: flat `key=value` text under `%APPDATA%\LinkPulse\config.ini` -- not vendored
      `inih`, since a hand-rolled parser exactly matching our small flat schema (no sections
      needed) is simpler to keep fully unit-tested than pulling in a general-purpose INI parser
- [ ] ~~Settings dialog from a Win32 dialog resource~~ -- **deferred past v1**: CLI flags +
      context menu already cover configuration; revisit if M4+ features need more surface
      than a context menu can reasonably hold
- [x] Light/dark tray adaptation: `SystemUsesLightTheme` registry value (the one that actually
      governs the taskbar/tray, not `AppsUseLightTheme` which only affects app window chrome),
      re-checked instantly on `WM_SETTINGCHANGE` ("ImmersiveColorSet") rather than waiting for
      the next timer tick
- [ ] ~~Logging with rotation~~ -- **deferred past v1**: `--debug` already exists; rotation only
      matters for a long-running install, revisit alongside M7 packaging
- [x] Start-on-login via `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`

Release workflow moved to Milestone 7, where it actually belongs.

## Milestone 4 — Network host discovery

- [x] Enumerate local subnets + gateway via `GetAdaptersAddresses()` and `GetBestRoute2()`
- [x] Passive discovery first: neighbour table via `GetIpNetTable2()` — no privileges needed
- [x] Tray network-map flyout with gateway, retained neighbors, local device, and connection icons
- [x] Best-effort reverse DNS names and conservative hostname-based device classification
- [x] Retain device identities across transient neighbor-cache misses
- [x] Deduplicate gateway neighbor entries by MAC
- [ ] Optional active discovery: `SendARP()` sweep, or raw sockets/Npcap for anything deeper
      (needs admin) — document the requirement and degrade gracefully
- [ ] mDNS name resolution via vendored `mdns.h`, plus NetBIOS lookup
- [ ] MAC → vendor lookup against a bundled OUI table
- [ ] Device inventory in SQLite (amalgamation): first seen, last seen, label, trusted flag
- [x] **New host notification** via a WinRT toast
- [ ] "Known devices" list in the UI with rename + mark-as-trusted
- [ ] Gateway adapters, beginning with Fritz!Box connected-device APIs

## Milestone 5 — Per-connection / per-process insight

- [ ] Socket → PID mapping via `GetExtendedTcpTable()` / `GetExtendedUdpTable()`
- [ ] Top talkers view (true per-process byte counts need ETW — best-effort otherwise)
- [ ] Reverse DNS + optional GeoIP for remote endpoints
- [ ] Alerts on suspicious patterns (connection fan-out, unusual ports)

## Milestone 6 — History, stats & alerts

- [ ] Persist samples to SQLite with downsampling + retention
- [ ] Daily/weekly/monthly totals; data-cap warnings
- [ ] Sparkline popup drawn with GDI
- [ ] Threshold alerts (sustained high usage, link down, gateway unreachable)
- [ ] Export CSV/JSON

## Milestone 7 — Distribution

**Reprioritized ahead of M4-M6**: a real, installable build is needed before any of M4-M6's
added features matter.

**Near-term path: a free Inno Setup installer** (`packaging/LinkPulse.iss`), so day-to-day
integration testing doesn't depend on any paid/gated infrastructure. **Microsoft Store
distribution (MSIX) is deferred** until the installer path is proven out end-to-end --
it requires a paid ($19) Partner Center developer account, so it's deliberately not the
critical path right now.

- [x] No-console-flash launch: a dedicated `linkpulse-tray.exe` (GUI subsystem, no arguments,
      never touches stdio) rather than making `linkpulse.exe` itself GUI-subsystem --
      `AttachConsole(ATTACH_PARENT_PROCESS)` (the usual way to keep CLI output working after
      switching subsystems) does not see a usable console when launched through WSL interop,
      which would have broken this project's dev/test workflow. Autostart now points at
      `linkpulse-tray.exe` specifically, resolved next to whichever binary is running rather
      than the running binary itself, so toggling it from the debug CLI can't misfire.
- [ ] MSVC Release build validated on a machine without the dev toolchain installed (avoids
      MinGW runtime DLL dependencies that wouldn't exist on a clean install); checklist:
      `docs/release-checklist.md`
- [x] Inno Setup installer (`packaging/LinkPulse.iss`): per-user install (no admin/UAC), Start
      Menu shortcut, optional desktop shortcut, uninstaller registered in "Add/Remove
      Programs", cleans up the `HKCU\...\Run` autostart entry on uninstall if present. Script
      validated on a native Windows machine, including startup after reboot
- [ ] Documentation + screenshots: release checklist exists at `docs/release-checklist.md`;
      capture final Windows screenshots during the clean-machine validation

### Deferred: Microsoft Store (MSIX) -- picked back up once the installer path is proven

- [ ] MSIX packaging: `Package.appxmanifest`, Package Family Name, Store asset tiles
      (44x44, 150x150, 310x150, ...) -- separate from the tray icon. Manifest, a packaging
      script (`packaging/build-msix.ps1`), and placeholder tile art already exist under
      `packaging/` -- still needed: the real Publisher CN/Package Family Name from Partner
      Center, replacing the placeholder art, and a local sideload test producing an actual
      installable MSIX
- [ ] Windows App Certification Kit (WACK) pass
- [ ] Autostart via the MSIX `StartupTask` manifest extension instead of (or in addition to)
      the current `HKCU\...\Run` key -- more idiomatic for a packaged app, and avoids the
      WSL-path fragility discovered during dev-environment testing
- [x] Release workflow publishing `LinkPulseSetup.exe` to GitHub Releases on `v*` tags
- [x] Update detection: background check against GitHub Releases, with a tray action opening the
      release page; silent download/replacement is deliberately not performed
- [ ] *(Owner-only, not automatable)*: Partner Center developer account, reserved app name,
      Store listing content (description, screenshots, age rating, privacy policy URL)

---

## Next up

**Next M4 slice**: add an optional gateway adapter interface, beginning with Fritz!Box connected-
device data so Wi-Fi clients that do not appear in Windows ARP/NDP can still be listed. Keep the
adapter optional, isolated, and testable with injected responses. SQLite persistence follows once
the live sources are unified.

## Open questions

- Windows toolchain: MSVC as primary (needed for `.rc` resources, manifests, WinRT toasts), or
  stay on MinGW-w64 so everything builds from WSL? CI currently does both.
- M4: strictly passive discovery, or accept the admin requirement for active ARP sweeps?
- M5: is best-effort per-process attribution acceptable, or is ETW in scope?
- Tray display: text rendered into the icon, or icon + numbers only in the tooltip?
