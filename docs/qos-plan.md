# QoS development plan

This plan turns the October 2026 project review into a sequence of small, reviewable fixes.
Every item below came from reading the code; the highest-impact ones were re-checked by hand.

## Branching model

- `QoS` is the integration branch. It starts from `feat/device-activity-capture`, because
  several fixes target the device activity and capture code that branch introduces.
- Each fix lives on its own `fix/<topic>` branch cut from `QoS` and is merged back with
  `git merge --no-ff`, so every fix stays visible as one merge commit.
- Every branch must build with `/W4 /WX` and keep `ctest` green before it is merged.
- `QoS` is merged into `main` through a single pull request once the whole set is done.

## Workstreams

### 1. Device activity capture

| Branch | Fixes |
| --- | --- |
| `fix/capture-frame-parsing` | Npcap device name built as `NPF_{{GUID}}`; direction decided from the first IP octet only; no link-type, EtherType, VLAN or IHL checks; capture errors spin the loop. Frame parsing moves into a portable, unit-tested core function. |
| `fix/capture-privacy-opt-in` | Capture runs without consent, in promiscuous mode, keeping whole packets. Adds an opt-in setting (off by default), header-only snaplen, non-promiscuous mode, and loads `wpcap.dll` from the Npcap system directory only. |
| `fix/device-refresh-lifecycle` | Shutdown frees the device store while the 5 s capture still runs; capture results are not tied to the device they belong to; a second click is dropped; a failed identity refresh skips capture and reports "unavailable". |
| `fix/popup-refresh-focus` | Periodic refreshes reposition the This PC and device details windows and steal focus. |

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

## Deferred

These need design work or hardware to test against, so they are tracked here rather than
fixed in this pass:

- Per-monitor DPI awareness: declaring it requires scaling every hard-coded layout and font.
- Discovery events beyond `max_events` are lost; needs an overflow signal in the API.
- Neighbor name resolution runs serially (up to 250 ms per unresolved neighbor).
- Dual-stack devices have their MAC treated as ambiguous across IPv4 and IPv6 entries.
- Large `lp_neighbor_list_t` values on the stack in `lp_discovery_poll`.
- Running `format-check` in CI once the tree is formatted.
- The per-user installer and Npcap's administrator-only mode.
- Showing a separate state when capture sees only traffic between this PC and the device.
