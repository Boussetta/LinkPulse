# LinkPulse Architecture

LinkPulse is a Windows 11 tray monitor with a portable C11 core. The core computes state from injected providers; Windows-specific code supplies those providers and owns the UI and operating-system integrations.

## Module Boundaries

```mermaid
flowchart TB
    cli[linkpulse.exe\nCLI and debug tray mode]
    tray[linkpulse-tray.exe\nGUI-subsystem entry point]
    ui[src/ui\ntray and network map]
    core[src/core\nconfig, sampler, discovery, format, log]
    api[include/linkpulse\npublic contracts]
    platform[src/platform/win32\nWindows adapters]
    os[(Windows APIs\niphlpapi, WinHTTP, registry, Shell, COM)]
    tests[tests\nCTest executables]

    cli --> api
    tray --> api
    ui --> api
    platform --> api
    core --> api
    tests --> core
    tests --> api
    cli --> core
    tray --> ui
    ui --> core
    cli --> platform
    tray --> platform
    platform --> os
```

The dependency direction is intentional:

- `src/core` must not include Windows headers or call UI code.
- `src/platform/win32` translates Windows APIs into the interfaces declared in `include/linkpulse`.
- `src/ui` owns presentation and consumes snapshots/events from the core.
- Tests inject fake providers into core modules, so rate and discovery behavior does not require a live network.

## CLI Runtime

```mermaid
sequenceDiagram
    participant User
    participant CLI as linkpulse.exe
    participant Config as config_win32
    participant Net as net_win32
    participant Sampler as lp_sampler
    participant Clock as clock_win32

    User->>CLI: --list / --watch / --tray
    CLI->>Config: load defaults and config.ini
    CLI->>Net: install snapshot and default-route providers
    CLI->>Clock: install monotonic clock provider
    alt --list
        CLI->>Net: enumerate interfaces
        Net-->>CLI: counters and adapter metadata
    else --watch or --tray
        loop polling interval
            CLI->>Sampler: poll current snapshot
            Sampler->>Net: read cumulative counters
            Sampler->>Clock: read monotonic time
            Sampler-->>CLI: rates and bounded history
        end
    end
```

The sampler works from cumulative byte counters. It computes deltas using monotonic time, handles counter resets and adapter churn, filters loopback/virtual adapters according to configuration, and keeps a fixed-capacity history.

## Tray Runtime

```mermaid
sequenceDiagram
    participant Entry as linkpulse-tray.exe
    participant UI as tray UI thread
    participant Worker as sampler/discovery workers
    participant Core as core state
    participant Win as Win32 adapters
    participant Explorer as Explorer/taskbar

    Entry->>UI: create hidden tray window and icon
    UI->>Worker: start sampler and discovery workers
    loop timer and worker updates
        Worker->>Win: read counters and neighbors
        Win-->>Worker: snapshots
        Worker->>Core: poll sampler/discovery
        Worker-->>UI: publish latest state under lock
        UI->>UI: render icon, tooltip, menu, and map
    end
    Explorer-->>UI: TaskbarCreated
    UI->>UI: recreate tray icon
    UI->>Worker: request shutdown
    Worker-->>UI: join and release state
```

The UI thread owns Windows windows, icons, menus, and message dispatch. Shared sampler/discovery results are protected before the UI reads them. Icon replacement must destroy the previous icon to avoid a GDI leak.

## Discovery and Update Flows

```mermaid
flowchart LR
    adapters[GetAdaptersAddresses\nlocal networks and gateway]
    neighbors[GetIpNetTable2\npassive neighbors]
    names[reverse DNS and hostname\nclassification]
    win_discovery[discovery_win32]
    discovery_core[discovery.c\nbaseline and events]
    map[network_map_win32]
    toast[notification_win32]

    adapters --> win_discovery
    neighbors --> win_discovery
    names --> win_discovery
    win_discovery --> discovery_core
    discovery_core --> map
    discovery_core --> toast
```

```mermaid
flowchart LR
    timer[background update timer] --> api[GitHub Releases API]
    api --> parser[update_win32\nsemantic version and asset selection]
    parser --> notify[native update toast]
    notify --> download[WinHTTP installer download]
    download --> launch[launch installer]
    activate[toast protocol activation] --> com[COM local server]
    com --> tray[linkpulse-tray.exe]
```

Discovery is passive by default. The core retains a baseline and emits a joined event immediately, but waits `LP_DISCOVERY_MISSING_POLLS_BEFORE_LEFT` polls before declaring a neighbor left. Updates never replace the running executable silently; they download and launch the installer.

## Build and Debugging

The supported native Windows development path is:

1. CMake configures the `msvc` preset with Visual Studio 2022 x64.
2. `msvc-debug` builds PDB-backed Debug binaries and copies the CLI binary to `out/windows`.
3. VS Code uses `cppvsdbg` from the Microsoft C/C++ extension to launch those MSVC binaries.
4. CTest runs the core-focused test executables from the same build tree.

MinGW remains a cross-build option, but it is not required for the primary Windows debugging workflow.
