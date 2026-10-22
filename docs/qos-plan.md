# QoS development plan

This plan turns the October 2026 project review into a sequence of small, reviewable fixes.
Every item below came from reading the code; the highest-impact ones were re-checked by hand.

## Branching model

- `QoS` is the integration branch. It starts from `main` and contains no unreleased
  feature work.
- Each fix lives on its own `fix/<topic>` branch cut from `QoS` and is merged back with
  `git merge --no-ff`, so every fix stays visible as one merge commit.
- Every branch must build with `/W4 /WX` and keep `ctest` green before it is merged.
- `QoS` is merged into `main` through a single pull request once the whole set is done.
- The device activity feature and the fixes that only concern it live on
  `feat/device-activity-capture`, which is built on top of `QoS`. It stays out of `main`
  until it is ready; see [Device activity feature](#device-activity-feature).

## Workstreams

### 1. Shutdown safety

| Branch | Fixes |
| --- | --- |
| `fix/device-store-shutdown-join` | Shutdown closes the device store even when the discovery or identity-refresh thread did not exit in time, so a slow worker can use freed memory. |

### 2. Notifications and updates

| Branch | Fixes |
| --- | --- |
| `fix/toast-xml-escaping` | Hostnames are injected into toast XML unescaped; non-ASCII text is lost; fixed 512-character buffer; two HSTRINGs leak per toast. |
| `fix/update-installer-verification` | The downloaded installer runs with no signature check, and the `linkpulse://download-update` handler runs it without confirmation. |
| `fix/update-download-before-exit` | The toast "Download update" action quits the process while the download thread is still running. |

### 3. Discovery

| Branch | Fixes |
| --- | --- |
| `fix/discovery-table-eviction` | Departed neighbors are never evicted; once the table is full, a JOINED event repeats every poll for neighbors that were never stored. |
| `fix/arp-probe-thread-lifetime` | ARP probe threads can outlive the stack array they write to. |
| `fix/adapter-enumeration-retry` | No retry when `GetAdaptersAddresses` reports a buffer overflow; wide-to-UTF-8 conversions can leave strings unterminated. |
| `fix/mdns-answer-validation` | Any PTR record is accepted as the answer and raw bytes are copied into hostnames. |
| `fix/router-vendor-label` | Any hostname containing "router" or "gateway" is labeled FRITZ!Box. |

### 4. Persistence

| Branch | Fixes |
| --- | --- |
| `fix/config-interval-validation` | `interval_ms=-1` and overflowing values are accepted on Windows; no bounds; a UTF-8 BOM breaks the first key. |
| `fix/atomic-config-save` | Config is overwritten in place, despite the comment calling the save atomic. |
| `fix/device-store-write-safety` | XmlWriter errors are ignored, so a failed save can replace a good file; the store is rewritten once per device on every poll. |
| `fix/device-store-locking` | The discovery thread updates the device store without holding the tray lock. |
| `fix/unicode-profile-paths` | Profile paths with characters outside the ANSI code page break the device store, config, log and autostart. |

### 5. Sampling, logging and UI

| Branch | Fixes |
| --- | --- |
| `fix/sampler-all-mode-accounting` | ALL mode counts filter pseudo-interfaces and silently ignores interfaces past 32; a zero-length sample drops its bytes. |
| `fix/log-sink-locking` | The log header promises stderr is replaced by the file sink; the spinlock is held across file I/O; the file-sink test cannot fail. |
| `fix/speed-meter-nonblocking` | Reopening the meter blocks the UI thread until the old test ends; cancel is not checked inside transfer loops; the destroy wait leaks. |
| `fix/network-map-userdata` | `GWLP_USERDATA` still points at freed state after `WM_DESTROY`. |

### 6. Build and CI

| Branch | Fixes |
| --- | --- |
| `chore/ci-mingw-tests` | The MinGW job builds but never runs the test suite. |

## Phase 2

Phase 2 covers the items deferred from the first pass, the release signing gap that the
installer verification exposed, and two user-reported UI problems. The same branching model
applies.

### 7. User-reported UI problems

| Branch | Fixes |
| --- | --- |
| `fix/speed-test-survives-gateway-view` | A running speed test and its result are lost when the user leaves the gateway view. |
| `feat/isp-details-view` | ISP details are drawn as text over the network map icons instead of in their own view. |
| `fix/per-monitor-dpi-awareness` | The process is DPI-unaware, so every window is bitmap-stretched above 100% scaling. |

### 8. Discovery

| Branch | Fixes |
| --- | --- |
| `fix/discovery-event-overflow` | Events beyond `max_events` are lost while their state is still committed. |
| `fix/discovery-poll-heap-scratch` | Large `lp_neighbor_list_t` values live on the stack during a poll. |
| `fix/dual-stack-mac-identity` | A MAC shared by a device's IPv4 and IPv6 entries is treated as ambiguous. |
| `fix/serial-name-resolution` | Neighbor names are resolved one at a time, up to 250 ms each, with no cache. |

### 9. Release

| Branch | Fixes |
| --- | --- |
| `chore/release-code-signing` | Release binaries are unsigned, so the update signature check always falls back to the release page. |

### 10. Formatting

| Branch | Fixes |
| --- | --- |
| `chore/clang-format-tree` | The tree does not match `.clang-format`, so CI cannot run `format-check`. Applied last, after every other branch is merged, to avoid conflicts. |

## Device activity feature

`feat/device-activity-capture` adds the flow-risk detector, the optional Npcap capture
preview, and the This PC and device details popups. It is kept out of `main` until it matures.
The branch starts from `QoS`, carries the feature commit, and merges these fixes with
`--no-ff`:

| Branch | Fixes |
| --- | --- |
| `fix/capture-frame-parsing` | Npcap device name built as `NPF_{{GUID}}`; direction decided from the first IP octet only; no link-type, EtherType, VLAN or IHL checks; capture errors spin the loop. Frame parsing moves into a portable, unit-tested core function. |
| `fix/capture-privacy-opt-in` | Capture runs without consent, in promiscuous mode, keeping whole packets. Adds an opt-in setting (off by default), header-only snaplen, non-promiscuous mode, and loads `wpcap.dll` from the Npcap system directory only. |
| `fix/device-refresh-lifecycle` | Shutdown frees the device store while the 5 s capture still runs; capture results are not tied to the device they belong to; a second click is dropped; a failed identity refresh skips capture and reports "unavailable". |
| `fix/popup-refresh-focus` | Periodic refreshes reposition the This PC and device details windows and steal focus. |
| `fix/npcap-admin-only-detection` | A per-user install cannot capture when Npcap is restricted to administrators, and the UI does not say why. |
| `fix/capture-local-host-only-state` | Capture does not distinguish traffic with this PC from traffic the device exchanges with others. |
