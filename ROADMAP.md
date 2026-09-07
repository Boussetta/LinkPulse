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
| Deps policy | Vendored single-file libraries only (SQLite amalgamation, `inih`, `mdns.h`) | Keeps the build hermetic |
| Tests | Plain C asserts driven by CTest | No framework dependency |
| API level | `_WIN32_WINNT=0x0A00` | Windows 10/11 only; no legacy fallbacks |

### Development workflow from WSL

WSL2 cannot call `Shell_NotifyIcon` and its virtual NIC is behind NAT, so its counters are
not the host's real traffic. Two options, both supported by the build:

- Cross-compile with MinGW-w64 from WSL, then run the `.exe` on the Windows side.
- Build natively on Windows with MSVC (required once resource scripts and toasts land).

Core logic stays free of `windows.h` so it can be unit-tested anywhere.

## Architecture

```
include/linkpulse/   public headers = the module boundaries
src/core/            portable C11 only: rate math, ring buffer, formatting, config, event bus
src/platform/win32/  iphlpapi, GDI, clock, toasts, registry
src/ui/              tray window, menu, icon rendering
src/cli/             headless mode: print rates to stdout (the dev/validation target)
src/discovery/       (M4+) ARP table, mDNS, OUI lookup
tests/               CTest
```

Rules:

- `src/core` includes **no** OS headers. It is where all unit-testable logic lives.
- All Win32 calls sit behind the headers in `include/linkpulse/`, so the logic can be driven
  by fake counter sources in tests.
- `core` never calls into `ui`. The UI subscribes via callbacks so later features (new-host
  alerts, thresholds) plug in without touching the sampler.

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

- [ ] Message-only window (`HWND_MESSAGE`) + `Shell_NotifyIcon` with `NIF_ICON | NIF_TIP`
- [ ] Render rate text with GDI into a 32bpp `CreateDIBSection`, convert via
      `CreateIconIndirect`, and **`DestroyIcon` the previous icon every tick** — the classic
      GDI leak in this design
- [ ] Handle `TaskbarCreated` (registered window message) to re-add the icon after Explorer restarts
- [ ] DPI awareness manifest; pick 16/20/24 px icons from `GetSystemMetrics(SM_CXSMICON)`
- [ ] Context menu (`TrackPopupMenu`): pause/resume, choose interface, units, settings, quit
- [ ] Threading model: sampler on its own thread, UI updates marshalled with `PostMessage` —
      never touch the icon off-thread
- [ ] Graceful shutdown, single-instance lock via a named mutex
- [ ] Smoke test: run a big download, confirm numbers match Task Manager

## Milestone 3 — Configuration, persistence & polish

- [ ] Config file (INI via vendored `inih`) under `%APPDATA%\LinkPulse`
- [ ] Settings dialog from a Win32 dialog resource
- [ ] Light/dark tray adaptation (`AppsUseLightTheme` registry value + `WM_SETTINGCHANGE`)
- [ ] Logging with rotation; `--debug` flag
- [ ] Start-on-login via `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
- [ ] Release workflow publishing a signed-ready `linkpulse.exe`

## Milestone 4 — Network host discovery

- [ ] Enumerate local subnets + gateway via `GetAdaptersAddresses()`
- [ ] Passive discovery first: neighbour table via `GetIpNetTable2()` — no privileges needed
- [ ] Optional active discovery: `SendARP()` sweep, or raw sockets/Npcap for anything deeper
      (needs admin) — document the requirement and degrade gracefully
- [ ] mDNS name resolution via vendored `mdns.h`, plus NetBIOS lookup
- [ ] MAC → vendor lookup against a bundled OUI table
- [ ] Device inventory in SQLite (amalgamation): first seen, last seen, label, trusted flag
- [ ] **New host notification** via `Shell_NotifyIcon` balloon (`NIIF_INFO`), or a WinRT toast
- [ ] "Known devices" list in the UI with rename + mark-as-trusted

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

- [ ] Installer (Inno Setup or WiX) + code-signing consideration
- [ ] Auto-update check
- [ ] Documentation + screenshots

---

## Next up

**M1 sampler**: `lp_sampler` on top of the existing `lp_net_snapshot()`, plus `--watch` and unit
tests driven by a fake counter source. All of it is testable without a GUI.

## Open questions

- Windows toolchain: MSVC as primary (needed for `.rc` resources, manifests, WinRT toasts), or
  stay on MinGW-w64 so everything builds from WSL? CI currently does both.
- M4: strictly passive discovery, or accept the admin requirement for active ARP sweeps?
- M5: is best-effort per-process attribution acceptable, or is ETW in scope?
- Tray display: text rendered into the icon, or icon + numbers only in the tooltip?
