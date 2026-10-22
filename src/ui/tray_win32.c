#include "linkpulse/tray.h"

#include "linkpulse/autostart.h"
#include "linkpulse/capture.h"
#include "linkpulse/clock.h"
#include "linkpulse/config.h"
#include "linkpulse/device_store.h"
#include "linkpulse/discovery.h"
#include "linkpulse/format.h"
#include "linkpulse/isp.h"
#include "linkpulse/log.h"
#include "linkpulse/net.h"
#include "linkpulse/notification.h"
#include "linkpulse/sampler.h"
#include "linkpulse/update.h"

#include "device_details_win32.h"
#include "dpi_win32.h"
#include "isp_view_win32.h"
#include "network_map_win32.h"
#include "speed_meter_win32.h"
#include "this_pc_win32.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <shellapi.h>

#define WM_LP_TRAYICON (WM_APP + 1)
#define WM_LP_UPDATE_RESULT (WM_APP + 2)
#define WM_LP_DISCOVERY_RESULT (WM_APP + 3)
#define WM_LP_SPEED_METER_OPEN (WM_APP + 4)
#define WM_LP_NETWORK_MAP_OPEN (WM_APP + 5)
#define WM_LP_THIS_PC_OPEN (WM_APP + 6)
#define WM_LP_DEVICE_DETAILS_OPEN (WM_APP + 7)
#define WM_LP_DEVICE_REFRESH_DONE (WM_APP + 8)
#define WM_LP_ISP_VIEW_OPEN (WM_APP + 9)
#define WM_LP_ISP_RESULT (WM_APP + 10)
#define LP_TRAY_TIMER_ID 1
#define LP_TRAY_ICON_UID 1
#define LP_UPDATE_VERSION_MAX 32
#define LP_UPDATE_THREAD_SHUTDOWN_TIMEOUT_MS 1000
#define LP_UPDATE_CHECK_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)
#define LP_DISCOVERY_INTERVAL_MS (30UL * 1000UL)
#define LP_ISP_LOOKUP_INTERVAL_MS (60UL * 60UL * 1000UL)

#define IDM_PAUSE 2001
#define IDM_UNITS_BITS 2002
#define IDM_AUTOSTART 2003
#define IDM_EXIT 2004
#define IDM_UPDATE 2005
#define IDM_SUPPORT 2006
#define IDM_PACKET_CAPTURE 2007

/* Enough points to fill the icon at the largest realistic SM_CXSMICON (64px at
   400% scaling, now that the process is DPI aware); render_icon only ever reads
   the last `size` of them. */
#define LP_TRAY_GRAPH_POINTS 64

/* Cross-thread shared state, guarded by `lock`. Everything else below (nid,
   icon handles, timer/thread handles) is only ever touched by the UI thread. */
typedef struct {
    CRITICAL_SECTION lock;
    lp_rate_sample_t latest;
    lp_status_t latest_status;
    bool has_sample;
    uint64_t rx_history[LP_TRAY_GRAPH_POINTS]; /* oldest first */
    size_t rx_history_count;
    uint64_t this_pc_rx_history[LP_THIS_PC_GRAPH_POINTS]; /* oldest first */
    uint64_t this_pc_tx_history[LP_THIS_PC_GRAPH_POINTS]; /* oldest first */
    size_t this_pc_history_count;

    volatile LONG stop_requested;
    volatile LONG paused;
    volatile LONG use_bits;
    volatile LONG packet_capture; /* user opt-in for Npcap device capture */
    bool update_available;
    char update_version[LP_UPDATE_VERSION_MAX];
    bool isp_available;
    bool isp_lookup_failed; /* the latest lookup failed and none has succeeded since */
    lp_isp_info_t isp_info;
    char last_gateway_mac[LP_MAC_STR_MAX];
    bool last_gateway_mac_set;

    lp_sampler_t sampler;
    lp_discovery_t discovery;
    void *device_store;
    unsigned interval_ms;
    HANDLE thread;
    HANDLE update_thread;
    HANDLE discovery_thread;
    HANDLE isp_thread;
    HANDLE device_refresh_thread;
    HANDLE download_thread;
    HANDLE update_stop_event;
    HANDLE isp_refresh_event;
    lp_discovery_event_t discovery_events[LP_DISCOVERY_MAX_EVENTS];
    size_t discovery_event_count;
    /* Join/leave events that did not fit in discovery_events; summarized in one toast. */
    size_t discovery_dropped_count;
    lp_neighbor_list_t map_neighbors;
    lp_local_network_list_t map_networks;
    lp_capture_activity_t selected_device_capture;
    char selected_device_capture_ip[LP_IP_STR_MAX]; /* device the capture above belongs to */
    bool has_selected_device_capture;
    volatile LONG device_refresh_cancel; /* asks the running device refresh to stop capturing */

    HWND hwnd;
    HWND network_map_hwnd;
    HWND speed_meter_hwnd;
    HWND this_pc_hwnd;
    HWND device_details_hwnd;
    HWND isp_view_hwnd;
    char pending_speed_meter_ip[LP_IP_STR_MAX];
    char pending_device_ip[LP_IP_STR_MAX];
    char device_refresh_running_ip[LP_IP_STR_MAX]; /* "" when no refresh thread is running */
    char device_refresh_queued_ip[LP_IP_STR_MAX];  /* latest request made while one was running */
    NOTIFYICONDATAA nid;
    HICON current_icon;
    UINT wm_taskbar_created;
} lp_tray_state_t;

/* One tray per process (enforced by the single-instance mutex), so a single
   global avoids threading GWLP_USERDATA through every message handler. */
static lp_tray_state_t g_tray;

#ifndef LP_VERSION
#define LP_VERSION "0.0.0-unknown"
#endif

/* The taskbar (and its clock/tray icons) follow "SystemUsesLightTheme", not the
   separate "AppsUseLightTheme" value that only affects app windows. Defaults
   to light (Windows' own default) if the value is missing. */
/* Reads the taskbar theme value rather than the unrelated app-window setting. */
static bool is_taskbar_light_theme(void)
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LONG result = RegGetValueA(
        HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        "SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &value, &size);
    return result != ERROR_SUCCESS || value != 0;
}

/* Renders a small 32bpp icon showing the download-rate history as a filled
   area graph -- the same shape as Task Manager's throughput view, scaled to
   the largest value currently visible. `history` is oldest-first; only its
   last `size` entries are drawn (older ones scroll off, same as Task Manager).
   Caller destroys the returned icon. */
static HICON render_icon(const uint64_t *history, size_t history_count)
{
    const int cx = GetSystemMetrics(SM_CXSMICON);
    const int cy = GetSystemMetrics(SM_CYSMICON);
    const int size = (cx > 0 && cy > 0) ? ((cx < cy) ? cx : cy) : 0;
    if (size <= 0) {
        return NULL;
    }

    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size; /* negative: top-down, origin at the top-left */
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void *bits = NULL;
    HDC screen_dc = GetDC(NULL);
    HBITMAP color_bmp =
        CreateDIBSection(screen_dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen_dc);
    if (color_bmp == NULL) {
        return NULL;
    }
    memset(bits, 0, (size_t)size * (size_t)size * 4); /* fully transparent background */

    uint64_t max_value = 1; /* avoid a divide by zero; also keeps an all-zero window flat */
    for (size_t i = 0; i < history_count; ++i) {
        if (history[i] > max_value) {
            max_value = history[i];
        }
    }

    const COLORREF theme_color = is_taskbar_light_theme() ? RGB(0, 0, 0) : RGB(255, 255, 255);
    const uint8_t theme_r = GetRValue(theme_color);
    const uint8_t theme_g = GetGValue(theme_color);
    const uint8_t theme_b = GetBValue(theme_color);
    uint8_t *pixels = (uint8_t *)bits;

    for (int x = 0; x < size; ++x) {
        /* Right-aligned: column `size - 1` is the newest point, matching Task
           Manager's scrolling-graph convention. Columns before any history
           exists (idx < 0) are left fully transparent. */
        const long long idx = (long long)history_count - size + x;
        const uint64_t value = (idx >= 0) ? history[idx] : 0;
        const int bar_height = (int)((double)value * (size - 1) / (double)max_value + 0.5);
        const int top_y = size - 1 - bar_height;

        for (int y = top_y; y < size; ++y) {
            if (y < 0) {
                continue;
            }
            uint8_t *pixel = pixels + ((size_t)y * (size_t)size + (size_t)x) * 4; /* B,G,R,A */
            pixel[0] = theme_b;
            pixel[1] = theme_g;
            pixel[2] = theme_r;
            pixel[3] = 0xFF;
        }
    }

    /* Content is irrelevant for a 32bpp icon with a real alpha channel, but
       CreateIconIndirect still requires a mask bitmap of matching dimensions. */
    HBITMAP mask_bmp = CreateBitmap(size, size, 1, 1, NULL);

    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmMask = mask_bmp;
    ii.hbmColor = color_bmp;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(mask_bmp);
    DeleteObject(color_bmp);
    return icon;
}

/* Polls core sampler state and publishes the latest values under the shared lock. */
static DWORD WINAPI sampler_thread_proc(LPVOID param)
{
    lp_tray_state_t *state = (lp_tray_state_t *)param;
    lp_status_t previous_status = LP_OK;
    bool had_previous_status = false;

    while (!InterlockedCompareExchange(&state->stop_requested, 0, 0)) {
        if (!InterlockedCompareExchange(&state->paused, 0, 0)) {
            lp_rate_sample_t sample;
            const lp_status_t status = lp_sampler_poll(&state->sampler, &sample);

            if (!had_previous_status || status != previous_status) {
                if (status == LP_OK) {
                    LP_INFO("sampler recovered");
                } else {
                    LP_WARN("sampler status changed: %s", lp_status_str(status));
                }
                previous_status = status;
                had_previous_status = true;
            }

            EnterCriticalSection(&state->lock);
            state->latest_status = status;
            if (status == LP_OK) {
                state->latest = sample;
                state->has_sample = true;

                if (state->rx_history_count < LP_TRAY_GRAPH_POINTS) {
                    state->rx_history[state->rx_history_count++] = sample.rx_bytes_per_sec;
                } else {
                    memmove(state->rx_history, state->rx_history + 1,
                            (LP_TRAY_GRAPH_POINTS - 1) * sizeof(state->rx_history[0]));
                    state->rx_history[LP_TRAY_GRAPH_POINTS - 1] = sample.rx_bytes_per_sec;
                }

                if (state->this_pc_history_count < LP_THIS_PC_GRAPH_POINTS) {
                    state->this_pc_rx_history[state->this_pc_history_count] =
                        sample.rx_bytes_per_sec;
                    state->this_pc_tx_history[state->this_pc_history_count] =
                        sample.tx_bytes_per_sec;
                    ++state->this_pc_history_count;
                } else {
                    memmove(state->this_pc_rx_history, state->this_pc_rx_history + 1,
                            (LP_THIS_PC_GRAPH_POINTS - 1) * sizeof(state->this_pc_rx_history[0]));
                    memmove(state->this_pc_tx_history, state->this_pc_tx_history + 1,
                            (LP_THIS_PC_GRAPH_POINTS - 1) * sizeof(state->this_pc_tx_history[0]));
                    state->this_pc_rx_history[LP_THIS_PC_GRAPH_POINTS - 1] =
                        sample.rx_bytes_per_sec;
                    state->this_pc_tx_history[LP_THIS_PC_GRAPH_POINTS - 1] =
                        sample.tx_bytes_per_sec;
                }
            }
            LeaveCriticalSection(&state->lock);
        }
        Sleep(state->interval_ms);
    }
    return 0;
}

/* Checks GitHub on a long interval and notifies the UI only for a new version. */
static DWORD WINAPI update_thread_proc(LPVOID param)
{
    lp_tray_state_t *state = (lp_tray_state_t *)param;
    LP_DEBUG("update-check thread started");
    for (;;) {
        char latest_version[LP_UPDATE_VERSION_MAX];
        const lp_status_t status =
            lp_update_check_latest(LP_VERSION, latest_version, sizeof(latest_version));
        if (status == LP_OK) {
            LP_INFO("update available: version=%s", latest_version);
            bool notify = false;
            EnterCriticalSection(&state->lock);
            notify = !state->update_available || strcmp(state->update_version, latest_version) != 0;
            snprintf(state->update_version, sizeof(state->update_version), "%s", latest_version);
            state->update_available = true;
            LeaveCriticalSection(&state->lock);
            PostMessageA(state->hwnd, WM_LP_UPDATE_RESULT, notify ? 1 : 0, 0);
        } else if (status == LP_ERR_NOT_FOUND) {
            LP_DEBUG("update check complete: already current");
        } else {
            LP_WARN("update check failed: %s", lp_status_str(status));
        }

        if (WaitForSingleObject(state->update_stop_event, LP_UPDATE_CHECK_INTERVAL_MS) ==
            WAIT_OBJECT_0) {
            break;
        }
    }
    LP_DEBUG("update-check thread stopped");
    return 0;
}

/* Looks up the public IP/ISP on a long interval, or immediately when the
   discovery thread signals isp_refresh_event after seeing the gateway MAC
   change (e.g. switching Wi-Fi networks); failures are silently retried. */
static DWORD WINAPI isp_thread_proc(LPVOID param)
{
    lp_tray_state_t *state = (lp_tray_state_t *)param;
    LP_DEBUG("ISP lookup thread started");
    for (;;) {
        lp_isp_info_t info;
        const lp_status_t status = lp_isp_lookup(&info);
        if (status == LP_OK) {
            LP_INFO("ISP lookup complete: isp=%s asn=%s public_ip=%s hostname=%s city=%s "
                    "region=%s country=%s postal=%s timezone=%s loc=%s",
                    info.isp[0] != '\0' ? info.isp : "(unknown)",
                    info.asn[0] != '\0' ? info.asn : "(unknown)",
                    info.public_ip[0] != '\0' ? info.public_ip : "(unknown)",
                    info.hostname[0] != '\0' ? info.hostname : "(unknown)",
                    info.city[0] != '\0' ? info.city : "(unknown)",
                    info.region[0] != '\0' ? info.region : "(unknown)",
                    info.country[0] != '\0' ? info.country : "(unknown)",
                    info.postal[0] != '\0' ? info.postal : "(unknown)",
                    info.timezone[0] != '\0' ? info.timezone : "(unknown)",
                    info.loc[0] != '\0' ? info.loc : "(unknown)");
            EnterCriticalSection(&state->lock);
            state->isp_info = info;
            state->isp_available = true;
            state->isp_lookup_failed = false;
            LeaveCriticalSection(&state->lock);
        } else {
            LP_WARN("ISP lookup failed: %s", lp_status_str(status));
            EnterCriticalSection(&state->lock);
            state->isp_lookup_failed = true;
            LeaveCriticalSection(&state->lock);
        }
        /* Lets an open ISP view pick up the new result or failure. */
        if (InterlockedCompareExchange(&state->stop_requested, 0, 0) == 0) {
            PostMessageA(state->hwnd, WM_LP_ISP_RESULT, 0, 0);
        }

        const HANDLE wait_handles[2] = {state->update_stop_event, state->isp_refresh_event};
        const DWORD wait_result =
            WaitForMultipleObjects(2, wait_handles, FALSE, LP_ISP_LOOKUP_INTERVAL_MS);
        if (wait_result == WAIT_OBJECT_0) {
            break; /* stop event */
        }
        /* WAIT_OBJECT_0 + 1 (refresh) or WAIT_TIMEOUT both fall through to loop and re-lookup. */
    }
    LP_DEBUG("ISP lookup thread stopped");
    return 0;
}

/* Polls passive discovery and publishes map/events data to the UI thread. */
static DWORD WINAPI discovery_thread_proc(LPVOID param)
{
    lp_tray_state_t *state = (lp_tray_state_t *)param;
    LP_DEBUG("discovery thread started");
    for (;;) {
        lp_discovery_event_t events[LP_DISCOVERY_MAX_EVENTS];
        size_t event_count = 0;
        const lp_status_t status =
            lp_discovery_poll(&state->discovery, events, LP_DISCOVERY_MAX_EVENTS, &event_count);
        if (status == LP_OK) {
            lp_local_network_list_t networks = {0};
            if (state->discovery.sources.local_networks_fn != NULL) {
                (void)state->discovery.sources.local_networks_fn(&networks);
            }
            /* Scopes the device store to whichever router is currently the
               default gateway, so switching networks (home, work, a
               relative's house) keeps separate device lists and ISP identity. */
            const char *gateway_mac = "";
            const char *gateway_hostname = "";
            const char *gateway_vendor = "";
            for (size_t i = 0; i < networks.count; ++i) {
                if (networks.items[i].gateway_mac[0] != '\0') {
                    gateway_mac = networks.items[i].gateway_mac;
                    gateway_hostname = networks.items[i].gateway_hostname;
                    gateway_vendor = networks.items[i].gateway_vendor;
                    break;
                }
            }
            /* last_gateway_mac is only ever read/written on this thread, so no
               lock is needed here. A momentary gap in gateway resolution
               (e.g. the adapter briefly reporting no default gateway) should
               not scatter this poll's devices into the "unknown network"
               bucket; keep attributing them to the last confirmed network
               instead. A genuine change refreshes the ISP lookup immediately
               rather than waiting for the hourly timer. */
            if (gateway_mac[0] == '\0' && state->last_gateway_mac_set) {
                gateway_mac = state->last_gateway_mac;
            } else if (gateway_mac[0] != '\0' &&
                       (!state->last_gateway_mac_set ||
                        strcmp(state->last_gateway_mac, gateway_mac) != 0)) {
                LP_INFO("gateway changed to %s; refreshing ISP lookup", gateway_mac);
                snprintf(state->last_gateway_mac, sizeof(state->last_gateway_mac), "%s",
                         gateway_mac);
                state->last_gateway_mac_set = true;
                if (state->isp_refresh_event != NULL) {
                    SetEvent(state->isp_refresh_event);
                }
            }
            /* Applies previously learned identity to notifications/logging too,
               not just the map, so a transient DNS miss doesn't hide a known name. */
            for (size_t i = 0; i < event_count; ++i) {
                lp_win32_device_store_apply(state->device_store, gateway_mac, &events[i].neighbor);
            }
            size_t active_count = 0;
            for (size_t i = 0; i < state->discovery.known_count; ++i) {
                if (state->discovery.known[i].active) {
                    ++active_count;
                }
            }
            LP_DEBUG("discovery poll complete: active=%llu retained=%llu events=%llu",
                     (unsigned long long)active_count,
                     (unsigned long long)state->discovery.known_count,
                     (unsigned long long)event_count);
            const size_t dropped_count = state->discovery.dropped_events;
            if (dropped_count > 0) {
                LP_WARN("discovery poll produced more events than fit: dropped=%llu",
                        (unsigned long long)dropped_count);
            }
            for (size_t i = 0; i < event_count; ++i) {
                LP_INFO("discovery event: type=%s ip=%s mac=%s hostname=%s",
                        events[i].type == LP_DISCOVERY_EVENT_JOINED ? "joined" : "left",
                        events[i].neighbor.ip[0] != '\0' ? events[i].neighbor.ip : "(unknown)",
                        events[i].neighbor.mac[0] != '\0' ? events[i].neighbor.mac : "(unknown)",
                        events[i].neighbor.hostname[0] != '\0' ? events[i].neighbor.hostname
                                                               : "(unknown)");
            }
            EnterCriticalSection(&state->lock);
            const lp_network_context_t network_context = {
                gateway_mac, gateway_hostname, gateway_vendor,
                state->isp_available ? &state->isp_info : NULL};
            state->map_neighbors.count = 0;
            for (size_t i = 0; i < state->discovery.known_count &&
                               state->map_neighbors.count < LP_DISCOVERY_MAX_NEIGHBORS;
                 ++i) {
                if (state->discovery.known[i].active) {
                    lp_neighbor_t neighbor = state->discovery.known[i];
                    lp_win32_device_store_apply(state->device_store, gateway_mac, &neighbor);
                    lp_win32_device_store_observe(state->device_store, &network_context, &neighbor);
                    state->map_neighbors.items[state->map_neighbors.count++] = neighbor;
                }
            }
            if (state->isp_available) {
                networks.isp_info = state->isp_info;
            }
            state->map_networks = networks;
            /* Append rather than overwrite: the UI thread may not have drained the
               previous poll's events yet. Whatever does not fit is summarized. */
            const size_t queued = state->discovery_event_count;
            const size_t room = LP_DISCOVERY_MAX_EVENTS - queued;
            const size_t appended = event_count < room ? event_count : room;
            if (appended > 0) {
                memcpy(&state->discovery_events[queued], events, appended * sizeof(events[0]));
            }
            state->discovery_event_count = queued + appended;
            state->discovery_dropped_count += dropped_count + (event_count - appended);
            LeaveCriticalSection(&state->lock);
            /* One disk write per poll, outside the tray lock, instead of one per device. */
            lp_win32_device_store_flush(state->device_store);
            if (event_count > 0 ||
                (state->network_map_hwnd != NULL && IsWindowVisible(state->network_map_hwnd))) {
                PostMessageA(state->hwnd, WM_LP_DISCOVERY_RESULT, 0, 0);
            }
        } else {
            LP_WARN("discovery poll failed: %s", lp_status_str(status));
        }

        if (WaitForSingleObject(state->update_stop_event, LP_DISCOVERY_INTERVAL_MS) ==
            WAIT_OBJECT_0) {
            break;
        }
    }
    LP_DEBUG("discovery thread stopped");
    return 0;
}

/* Downloads and launches an update without blocking the tray message loop. */
static DWORD WINAPI download_thread_proc(LPVOID param)
{
    lp_tray_state_t *state = (lp_tray_state_t *)param;
    LP_INFO("update download started");
    char installer_path[MAX_PATH];
    if (lp_update_download_latest(installer_path, sizeof(installer_path)) != LP_OK) {
        MessageBoxA(NULL, "Could not download the LinkPulse update.", "LinkPulse update",
                    MB_OK | MB_ICONERROR);
    } else {
        const lp_status_t launched = lp_update_launch_installer(installer_path);
        if (launched == LP_ERR_IO) {
            MessageBoxA(state->hwnd, "Could not launch the LinkPulse installer.",
                        "LinkPulse update", MB_OK | MB_ICONERROR);
        } else if (launched == LP_ERR_UNSUPPORTED) {
            MessageBoxA(state->hwnd,
                        "The downloaded installer has no valid signature, so it was not run. "
                        "The release page was opened in your browser instead.",
                        "LinkPulse update", MB_OK | MB_ICONWARNING);
        }
    }
    return 0;
}

/* Ensures only one user-triggered installer download runs at a time. */
static void start_update_download(lp_tray_state_t *state)
{
    if (state->download_thread != NULL) {
        const DWORD wait_result = WaitForSingleObject(state->download_thread, 0);
        if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_FAILED) {
            if (wait_result == WAIT_FAILED) {
                LP_WARN("waiting for downloader thread failed");
            }
            CloseHandle(state->download_thread);
            state->download_thread = NULL;
        } else {
            LP_DEBUG("update download already in progress");
            return;
        }
    }
    state->download_thread = CreateThread(NULL, 0, download_thread_proc, state, 0, NULL);
    if (state->download_thread == NULL) {
        MessageBoxA(state->hwnd, "Could not start the LinkPulse downloader.", "LinkPulse update",
                    MB_OK | MB_ICONERROR);
    } else {
        LP_DEBUG("update download thread started");
    }
}

/* Copies the synchronized version string before invoking Windows toast APIs. */
static void show_update_notification(lp_tray_state_t *state)
{
    char version[LP_UPDATE_VERSION_MAX];
    EnterCriticalSection(&state->lock);
    snprintf(version, sizeof(version), "%s", state->update_version);
    LeaveCriticalSection(&state->lock);

    if (!lp_win32_show_update_toast(version)) {
        LP_WARN("failed to show Windows notification-center toast");
    }
}

/* Drains queued join/leave events and renders one toast per event. */
static void show_discovery_notifications(lp_tray_state_t *state)
{
    lp_discovery_event_t events[LP_DISCOVERY_MAX_EVENTS];
    size_t event_count;
    size_t dropped_count;
    EnterCriticalSection(&state->lock);
    event_count = state->discovery_event_count;
    memcpy(events, state->discovery_events, event_count * sizeof(events[0]));
    state->discovery_event_count = 0;
    dropped_count = state->discovery_dropped_count;
    state->discovery_dropped_count = 0;
    LeaveCriticalSection(&state->lock);

    if (dropped_count > 0) {
        char summary[128];
        snprintf(summary, sizeof(summary), "%llu more device%s joined or left the network.",
                 (unsigned long long)dropped_count, dropped_count == 1 ? "" : "s");
        if (!lp_win32_show_toast("Network devices changed", summary)) {
            LP_WARN("failed to show network discovery summary notification");
        }
    }

    for (size_t i = 0; i < event_count; ++i) {
        if (events[i].neighbor.known_device) {
            LP_DEBUG("suppressing notification for known device: mac=%s ip=%s",
                     events[i].neighbor.mac[0] != '\0' ? events[i].neighbor.mac : "(unknown)",
                     events[i].neighbor.ip[0] != '\0' ? events[i].neighbor.ip : "(unknown)");
            continue;
        }
        const bool joined = events[i].type == LP_DISCOVERY_EVENT_JOINED;
        const char *name = events[i].neighbor.label[0] != '\0'      ? events[i].neighbor.label
                           : events[i].neighbor.hostname[0] != '\0' ? events[i].neighbor.hostname
                                                                    : NULL;
        char message[512];
        if (name != NULL && events[i].neighbor.mac[0] != '\0') {
            snprintf(message, sizeof(message), "%s\n%s\nIP: %s\nMAC: %s",
                     joined ? "Connected" : "Disconnected", name, events[i].neighbor.ip,
                     events[i].neighbor.mac);
        } else if (name != NULL) {
            snprintf(message, sizeof(message), "%s\n%s\nIP: %s",
                     joined ? "Connected" : "Disconnected", name, events[i].neighbor.ip);
        } else if (events[i].neighbor.mac[0] != '\0') {
            snprintf(message, sizeof(message), "%s\nIP: %s\nMAC: %s",
                     joined ? "Connected" : "Disconnected", events[i].neighbor.ip,
                     events[i].neighbor.mac);
        } else {
            snprintf(message, sizeof(message), "%s\nIP: %s", joined ? "Connected" : "Disconnected",
                     events[i].neighbor.ip);
        }
        if (!lp_win32_show_toast(joined ? "Network device joined" : "Network device left",
                                 message)) {
            LP_WARN("failed to show network discovery notification");
        }
    }
}

/* Toggles the map popup using a consistent locked snapshot of discovery state. */
static void show_network_map(lp_tray_state_t *state)
{
    if (state->network_map_hwnd == NULL) {
        return;
    }
    /* UI thread only; static because a full neighbor list is ~180 KB. */
    static lp_neighbor_list_t neighbors;
    static lp_local_network_list_t networks;
    EnterCriticalSection(&state->lock);
    neighbors = state->map_neighbors;
    networks = state->map_networks;
    LeaveCriticalSection(&state->lock);
    lp_network_map_show(state->network_map_hwnd, &neighbors, &networks);
}

static void toggle_network_map(lp_tray_state_t *state)
{
    if (state->network_map_hwnd == NULL) {
        return;
    }
    if (IsWindowVisible(state->network_map_hwnd)) {
        ShowWindow(state->network_map_hwnd, SW_HIDE);
        return;
    }
    show_network_map(state);
}

/* Pushes the latest ISP lookup result to the ISP view. `activate` opens, positions, and focuses
   it (explicit user opens); otherwise it only repaints. */
static void show_isp_view(lp_tray_state_t *state, bool activate)
{
    if (state->isp_view_hwnd == NULL) {
        return;
    }
    lp_isp_info_t info;
    EnterCriticalSection(&state->lock);
    const bool available = state->isp_available;
    const bool failed = state->isp_lookup_failed;
    info = state->isp_info;
    LeaveCriticalSection(&state->lock);
    if (activate) {
        lp_isp_view_show(state->isp_view_hwnd, available ? &info : NULL, failed);
    } else {
        lp_isp_view_update(state->isp_view_hwnd, available ? &info : NULL, failed);
    }
}

/* Opens the speed meter layer for the gateway the map reported as clicked. */
static void show_speed_meter(lp_tray_state_t *state, const char *gateway_ip)
{
    if (state->speed_meter_hwnd == NULL || gateway_ip == NULL || gateway_ip[0] == '\0') {
        LP_WARN("speed meter unavailable: window=%p gateway=%s", (void *)state->speed_meter_hwnd,
                gateway_ip != NULL && gateway_ip[0] != '\0' ? gateway_ip : "(none)");
        return;
    }

    char label[LP_HOSTNAME_MAX];
    char isp[LP_ISP_NAME_MAX];
    label[0] = '\0';
    isp[0] = '\0';
    EnterCriticalSection(&state->lock);
    for (size_t i = 0; i < state->map_networks.count; ++i) {
        if (strcmp(state->map_networks.items[i].gateway, gateway_ip) != 0) {
            continue;
        }
        if (state->map_networks.items[i].gateway_hostname[0] != '\0') {
            snprintf(label, sizeof(label), "%s", state->map_networks.items[i].gateway_hostname);
        } else if (state->map_networks.items[i].gateway_vendor[0] != '\0') {
            snprintf(label, sizeof(label), "%s", state->map_networks.items[i].gateway_vendor);
        }
        break;
    }
    if (state->isp_available) {
        snprintf(isp, sizeof(isp), "%s", state->isp_info.isp);
    }
    LeaveCriticalSection(&state->lock);

    const bool use_bits = InterlockedCompareExchange(&state->use_bits, 0, 0) != 0;
    lp_speed_meter_show(state->speed_meter_hwnd, label[0] != '\0' ? label : "Router", gateway_ip,
                        isp, use_bits);
}

/* Pushes the current throughput history and adapter/Wi-Fi snapshot to the "This PC" view.
   `activate` opens, positions, and focuses it (explicit user opens); otherwise it only repaints,
   so periodic refreshes never move the window or steal focus. */
static void show_this_pc(lp_tray_state_t *state, bool activate)
{
    if (state->this_pc_hwnd == NULL) {
        return;
    }
    lp_local_network_list_t networks;
    uint64_t rx_history[LP_THIS_PC_GRAPH_POINTS];
    uint64_t tx_history[LP_THIS_PC_GRAPH_POINTS];
    size_t history_count;
    EnterCriticalSection(&state->lock);
    networks = state->map_networks;
    history_count = state->this_pc_history_count;
    memcpy(rx_history, state->this_pc_rx_history, history_count * sizeof(rx_history[0]));
    memcpy(tx_history, state->this_pc_tx_history, history_count * sizeof(tx_history[0]));
    LeaveCriticalSection(&state->lock);

    const bool use_bits = InterlockedCompareExchange(&state->use_bits, 0, 0) != 0;
    if (activate) {
        lp_this_pc_show(state->this_pc_hwnd, networks.local_hostname, &networks, rx_history,
                        tx_history, history_count, use_bits);
    } else {
        lp_this_pc_update(state->this_pc_hwnd, networks.local_hostname, &networks, rx_history,
                          tx_history, history_count, use_bits);
    }
}

/* Pushes the selected device's snapshot to the details view; `activate` as for show_this_pc. */
static void show_device_details(lp_tray_state_t *state, const char *device_ip, bool activate)
{
    if (state->device_details_hwnd == NULL || device_ip == NULL || device_ip[0] == '\0') {
        return;
    }
    lp_neighbor_t neighbor = {0};
    bool found = false;
    EnterCriticalSection(&state->lock);
    for (size_t i = 0; i < state->map_neighbors.count; ++i) {
        if (strcmp(state->map_neighbors.items[i].ip, device_ip) == 0) {
            neighbor = state->map_neighbors.items[i];
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&state->lock);
    if (found) {
        lp_capture_activity_t capture = {0};
        bool has_capture;
        EnterCriticalSection(&state->lock);
        /* A capture finished for a previously selected device must never be shown for this one. */
        has_capture = state->has_selected_device_capture &&
                      strcmp(state->selected_device_capture_ip, device_ip) == 0;
        if (has_capture) {
            capture = state->selected_device_capture;
        }
        LeaveCriticalSection(&state->lock);
        lp_device_capture_state_t capture_state = LP_DEVICE_CAPTURE_OFF;
        if (InterlockedCompareExchange(&state->packet_capture, 0, 0) != 0) {
            const bool refresh_pending = strcmp(state->device_refresh_running_ip, device_ip) == 0 ||
                                         strcmp(state->device_refresh_queued_ip, device_ip) == 0;
            if (has_capture) {
                capture_state = LP_DEVICE_CAPTURE_DONE;
            } else if (refresh_pending) {
                capture_state = LP_DEVICE_CAPTURE_RUNNING;
            } else {
                capture_state = LP_DEVICE_CAPTURE_DONE;
                snprintf(capture.detail, sizeof(capture.detail), "Capture did not start");
            }
        }
        if (activate) {
            lp_device_details_show(state->device_details_hwnd, &neighbor, capture_state,
                                   &capture);
        } else {
            lp_device_details_update(state->device_details_hwnd, &neighbor, capture_state,
                                     &capture);
        }
    }
}

typedef struct {
    lp_tray_state_t *state;
    char ip[LP_IP_STR_MAX];
} lp_device_refresh_request_t;

static void network_context_from_snapshot(const lp_local_network_list_t *networks,
                                          const lp_isp_info_t *isp, lp_network_context_t *out)
{
    memset(out, 0, sizeof(*out));
    out->gateway_mac = "";
    out->gateway_hostname = "";
    out->gateway_vendor = "";
    out->isp = isp;
    for (size_t i = 0; i < networks->count; ++i) {
        if (networks->items[i].gateway_mac[0] != '\0') {
            out->gateway_mac = networks->items[i].gateway_mac;
            out->gateway_hostname = networks->items[i].gateway_hostname;
            out->gateway_vendor = networks->items[i].gateway_vendor;
            return;
        }
    }
}

static DWORD WINAPI device_refresh_thread_proc(LPVOID param)
{
    lp_device_refresh_request_t *request = (lp_device_refresh_request_t *)param;
    lp_tray_state_t *state = request->state;
    lp_neighbor_t neighbor = {0};
    lp_local_network_list_t networks = {0};
    lp_isp_info_t isp = {0};
    bool found = false;
    bool isp_available = false;
    EnterCriticalSection(&state->lock);
    for (size_t i = 0; i < state->map_neighbors.count; ++i) {
        if (strcmp(state->map_neighbors.items[i].ip, request->ip) == 0) {
            neighbor = state->map_neighbors.items[i];
            found = true;
            break;
        }
    }
    networks = state->map_networks;
    if (state->isp_available) {
        isp = state->isp_info;
        isp_available = true;
    }
    LeaveCriticalSection(&state->lock);

    /* A failed identity refresh must not skip the capture: the IP alone is enough to sample. */
    bool identity_refreshed = false;
    if (found) {
        LP_DEBUG("refreshing selected device identity: ip=%s mac=%s", neighbor.ip,
                 neighbor.mac[0] != '\0' ? neighbor.mac : "(unknown)");
        const lp_status_t status = lp_net_refresh_neighbor_identity(&neighbor);
        identity_refreshed = status == LP_OK;
        if (!identity_refreshed) {
            LP_DEBUG("selected device identity refresh failed: ip=%s status=%s", neighbor.ip,
                     lp_status_str(status));
        }
    } else {
        LP_DEBUG("selected device is no longer on the map: ip=%s", request->ip);
    }

    lp_capture_activity_t capture = {0};
    const bool capture_enabled = InterlockedCompareExchange(&state->packet_capture, 0, 0) != 0;
    if (capture_enabled && InterlockedCompareExchange(&state->device_refresh_cancel, 0, 0) == 0) {
        const lp_status_t capture_status = lp_win32_capture_device_activity(
            request->ip, 5000, &state->device_refresh_cancel, &capture);
        LP_INFO("device capture: ip=%s status=%s visible=%d packets=%u rx_bytes=%llu "
                "tx_bytes=%llu detail=%s",
                request->ip, lp_status_str(capture_status), capture.device_visible ? 1 : 0,
                capture.packet_count, (unsigned long long)capture.rx_bytes,
                (unsigned long long)capture.tx_bytes,
                capture.detail[0] != '\0' ? capture.detail : "(none)");
    }
    const bool cancelled = InterlockedCompareExchange(&state->device_refresh_cancel, 0, 0) != 0;

    /* Shutdown may have given up waiting for this thread; the lock is then deliberately never
       deleted, but the device store and window must not be touched again. */
    EnterCriticalSection(&state->lock);
    const bool stopping = InterlockedCompareExchange(&state->stop_requested, 0, 0) != 0;
    if (!stopping) {
        if (capture_enabled && !cancelled) {
            state->selected_device_capture = capture;
            snprintf(state->selected_device_capture_ip, sizeof(state->selected_device_capture_ip),
                     "%s", request->ip);
            state->has_selected_device_capture = true;
        }
        if (identity_refreshed) {
            for (size_t i = 0; i < state->map_neighbors.count; ++i) {
                if (strcmp(state->map_neighbors.items[i].ip, neighbor.ip) == 0) {
                    state->map_neighbors.items[i] = neighbor;
                    break;
                }
            }
            lp_network_context_t network_context;
            network_context_from_snapshot(&networks, isp_available ? &isp : NULL,
                                          &network_context);
            lp_win32_device_store_observe(state->device_store, &network_context, &neighbor);
        }
    }
    LeaveCriticalSection(&state->lock);
    if (!stopping) {
        lp_win32_device_store_flush(state->device_store);
        PostMessageA(state->hwnd, WM_LP_DEVICE_REFRESH_DONE, 0, 0);
    }
    free(request);
    return 0;
}

/* Starts a refresh thread for `ip`; the caller guarantees none is running. */
static void start_device_refresh(lp_tray_state_t *state, const char *ip)
{
    lp_device_refresh_request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        return;
    }
    request->state = state;
    snprintf(request->ip, sizeof(request->ip), "%s", ip);
    InterlockedExchange(&state->device_refresh_cancel, 0);
    state->device_refresh_thread =
        CreateThread(NULL, 0, device_refresh_thread_proc, request, 0, NULL);
    if (state->device_refresh_thread == NULL) {
        LP_WARN("could not start the selected device refresh: ip=%s", ip);
        free(request);
        return;
    }
    snprintf(state->device_refresh_running_ip, sizeof(state->device_refresh_running_ip), "%s",
             ip);
}

/* Refreshes identity and (when opted in) captures traffic for the selected device. If a refresh
   for another device is running, it is cancelled and this request runs as soon as it exits. */
static void refresh_selected_device_identity(lp_tray_state_t *state, const char *ip)
{
    if (ip == NULL || ip[0] == '\0') {
        return;
    }
    if (state->device_refresh_thread != NULL) {
        if (strcmp(state->device_refresh_running_ip, ip) == 0) {
            state->device_refresh_queued_ip[0] = '\0';
            LP_DEBUG("selected device refresh already running: ip=%s", ip);
            return;
        }
        snprintf(state->device_refresh_queued_ip, sizeof(state->device_refresh_queued_ip), "%s",
                 ip);
        InterlockedExchange(&state->device_refresh_cancel, 1);
        LP_DEBUG("queued selected device refresh: ip=%s (cancelling %s)", ip,
                 state->device_refresh_running_ip);
        return;
    }
    start_device_refresh(state, ip);
}

/* Joins the finished refresh thread and starts the most recent queued request, if any. */
static void finish_device_refresh(lp_tray_state_t *state)
{
    if (state->device_refresh_thread != NULL) {
        /* The worker posts completion just before returning, so this wait is brief. */
        WaitForSingleObject(state->device_refresh_thread, INFINITE);
        CloseHandle(state->device_refresh_thread);
        state->device_refresh_thread = NULL;
    }
    state->device_refresh_running_ip[0] = '\0';
    if (state->device_refresh_queued_ip[0] != '\0') {
        char next_ip[LP_IP_STR_MAX];
        snprintf(next_ip, sizeof(next_ip), "%s", state->device_refresh_queued_ip);
        state->device_refresh_queued_ip[0] = '\0';
        start_device_refresh(state, next_ip);
    }
}

/* Redraws the network map and device details with the latest shared state, if open. */
static void refresh_open_device_views(lp_tray_state_t *state)
{
    if (state->network_map_hwnd != NULL && IsWindowVisible(state->network_map_hwnd)) {
        /* UI thread only; static because a full neighbor list is ~180 KB. */
        static lp_neighbor_list_t neighbors;
        static lp_local_network_list_t networks;
        EnterCriticalSection(&state->lock);
        neighbors = state->map_neighbors;
        networks = state->map_networks;
        LeaveCriticalSection(&state->lock);
        lp_network_map_show(state->network_map_hwnd, &neighbors, &networks);
    }
    if (state->device_details_hwnd != NULL && IsWindowVisible(state->device_details_hwnd)) {
        show_device_details(state, state->pending_device_ip, false);
    }
    if (state->isp_view_hwnd != NULL && IsWindowVisible(state->isp_view_hwnd)) {
        show_isp_view(state, false);
    }
}

/* Builds the dynamic tray menu and dispatches the selected user command. */
static void show_context_menu(lp_tray_state_t *state)
{
    HMENU menu = CreatePopupMenu();
    if (menu == NULL) {
        return;
    }

    const bool paused = InterlockedCompareExchange(&state->paused, 0, 0) != 0;
    const bool use_bits = InterlockedCompareExchange(&state->use_bits, 0, 0) != 0;
    const bool autostart = lp_autostart_is_enabled();
    const bool packet_capture = InterlockedCompareExchange(&state->packet_capture, 0, 0) != 0;
    bool update_available;
    EnterCriticalSection(&state->lock);
    update_available = state->update_available;
    LeaveCriticalSection(&state->lock);

    AppendMenuA(menu, MF_STRING | (paused ? MF_CHECKED : 0), IDM_PAUSE,
                paused ? "Resume" : "Pause");
    AppendMenuA(menu, MF_STRING | (use_bits ? MF_CHECKED : 0), IDM_UNITS_BITS, "Show bits/s");
    AppendMenuA(menu, MF_STRING | (autostart ? MF_CHECKED : 0), IDM_AUTOSTART,
                "Start with Windows");
    AppendMenuA(menu, MF_STRING | (packet_capture ? MF_CHECKED : 0), IDM_PACKET_CAPTURE,
                "Capture device traffic (Npcap)");
    if (update_available) {
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, MF_STRING, IDM_UPDATE, "Download update");
    }
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    /* Plain MF_STRING text is drawn in a single theme color, so a heart glyph
       there renders black/monochrome, not GitHub's pink. Owner-draw the item
       instead so the heart can be painted in color; see WM_MEASUREITEM /
       WM_DRAWITEM below. (A custom MIIM_BITMAP icon was tried first, but
       Windows does not reliably alpha-blend menu item bitmaps, which left an
       opaque black square instead of a transparent heart.) */
    AppendMenuW(menu, MF_OWNERDRAW, IDM_SUPPORT, NULL);
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_EXIT, "Exit");

    POINT cursor;
    GetCursorPos(&cursor);

    /* Required so the menu closes when the user clicks elsewhere; see the
       Shell_NotifyIcon documentation's "About Notification Area Icons" remarks. */
    SetForegroundWindow(state->hwnd);
    const int selected = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, cursor.x, cursor.y,
                                        0, state->hwnd, NULL);
    PostMessageA(state->hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (selected) {
    case IDM_PAUSE:
        InterlockedExchange(&state->paused, paused ? 0 : 1);
        break;
    case IDM_UNITS_BITS:
        InterlockedExchange(&state->use_bits, use_bits ? 0 : 1);
        break;
    case IDM_AUTOSTART:
        if (lp_autostart_set(!autostart) != LP_OK) {
            LP_WARN("failed to %s start-on-login", autostart ? "disable" : "enable");
        }
        break;
    case IDM_PACKET_CAPTURE:
        InterlockedExchange(&state->packet_capture, packet_capture ? 0 : 1);
        LP_INFO("packet capture %s by user", packet_capture ? "disabled" : "enabled");
        if (packet_capture && state->device_refresh_thread != NULL) {
            InterlockedExchange(&state->device_refresh_cancel, 1);
        }
        if (state->device_details_hwnd != NULL && IsWindowVisible(state->device_details_hwnd)) {
            if (!packet_capture) {
                EnterCriticalSection(&state->lock);
                state->has_selected_device_capture = false;
                LeaveCriticalSection(&state->lock);
                if (state->device_refresh_thread != NULL) {
                    /* The running refresh may already be past its capture step; rerun it. */
                    snprintf(state->device_refresh_queued_ip,
                             sizeof(state->device_refresh_queued_ip), "%s",
                             state->pending_device_ip);
                } else {
                    refresh_selected_device_identity(state, state->pending_device_ip);
                }
            }
            show_device_details(state, state->pending_device_ip, false);
        }
        break;
    case IDM_UPDATE:
        start_update_download(state);
        break;
    case IDM_SUPPORT: {
        /* Opt-in only: never shown automatically, no telemetry, just a link. */
        const INT_PTR rc =
            (INT_PTR)ShellExecuteA(state->hwnd, "open", "https://github.com/sponsors/Boussetta",
                                   NULL, NULL, SW_SHOWNORMAL);
        if (rc <= 32) {
            MessageBoxA(state->hwnd, "Could not open the LinkPulse support page.", "LinkPulse",
                        MB_OK | MB_ICONERROR);
        }
        break;
    }
    case IDM_EXIT:
        DestroyWindow(state->hwnd);
        break;
    default:
        break;
    }
}

/* Reads shared sampler state and updates the icon graph and tray tooltip. */
static void refresh_icon_and_tooltip(lp_tray_state_t *state)
{
    lp_rate_sample_t sample = {0, 0, 0};
    lp_status_t status = LP_ERR_NOT_FOUND;
    bool has_sample = false;
    bool update_available = false;
    char update_version[LP_UPDATE_VERSION_MAX];
    uint64_t history[LP_TRAY_GRAPH_POINTS];
    size_t history_count = 0;

    EnterCriticalSection(&state->lock);
    sample = state->latest;
    status = state->latest_status;
    has_sample = state->has_sample;
    update_available = state->update_available;
    snprintf(update_version, sizeof(update_version), "%s", state->update_version);
    history_count = state->rx_history_count;
    memcpy(history, state->rx_history, history_count * sizeof(history[0]));
    LeaveCriticalSection(&state->lock);

    const bool paused = InterlockedCompareExchange(&state->paused, 0, 0) != 0;
    const bool use_bits = InterlockedCompareExchange(&state->use_bits, 0, 0) != 0;

    HICON new_icon = render_icon(history, history_count);
    if (new_icon != NULL) {
        state->nid.hIcon = new_icon;
        if (state->current_icon != NULL) {
            DestroyIcon(state->current_icon); /* the classic leak this guards against */
        }
        state->current_icon = new_icon;
    }

    char rx_str[32];
    char tx_str[32];
    if (has_sample) {
        lp_format_rate(sample.rx_bytes_per_sec, use_bits, rx_str, sizeof(rx_str));
        lp_format_rate(sample.tx_bytes_per_sec, use_bits, tx_str, sizeof(tx_str));
        snprintf(state->nid.szTip, sizeof(state->nid.szTip), "LinkPulse\ndown: %s\nup: %s%s",
                 rx_str, tx_str, paused ? " (paused)" : "");
    } else if (status == LP_ERR_NOT_FOUND) {
        snprintf(state->nid.szTip, sizeof(state->nid.szTip), "LinkPulse\nwaiting for interface...");
    } else {
        snprintf(state->nid.szTip, sizeof(state->nid.szTip), "LinkPulse\n%s",
                 lp_status_str(status));
    }
    if (update_available) {
        const size_t tip_length = strlen(state->nid.szTip);
        if (tip_length < sizeof(state->nid.szTip)) {
            snprintf(state->nid.szTip + tip_length, sizeof(state->nid.szTip) - tip_length,
                     "\nupdate available: %s", update_version);
        }
    }

    Shell_NotifyIconA(NIM_MODIFY, &state->nid);
}

static const wchar_t LP_SPONSOR_ITEM_TEXT[] = L"\u2764  Sponsor";
#define LP_SPONSOR_HEART_LEN 1 /* just the "\u2764" glyph, colored separately from the label */

/* Supplies the owner-drawn support item dimensions to the Windows menu system. */
static void measure_sponsor_item(MEASUREITEMSTRUCT *item)
{
    HDC dc = GetDC(NULL);
    SIZE extent = {0};
    if (dc != NULL) {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT previous_font = (HFONT)SelectObject(dc, font);
        GetTextExtentPoint32W(dc, LP_SPONSOR_ITEM_TEXT,
                              (int)(sizeof(LP_SPONSOR_ITEM_TEXT) / sizeof(wchar_t)) - 1, &extent);
        SelectObject(dc, previous_font);
        ReleaseDC(NULL, dc);
    }

    const int xpad = GetSystemMetrics(SM_CXEDGE);
    const int gutter = GetSystemMetrics(SM_CXMENUCHECK) + 2 * xpad;
    item->itemWidth = (UINT)extent.cx + (UINT)gutter + (UINT)xpad;

    const UINT height = (UINT)(extent.cy + 2 * GetSystemMetrics(SM_CYEDGE));
    const UINT min_height = (UINT)GetSystemMetrics(SM_CYMENU);
    item->itemHeight = height > min_height ? height : min_height;
}

/* Paints the support item so the heart can use its accent color. */
static void draw_sponsor_item(const DRAWITEMSTRUCT *item)
{
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    FillRect(item->hDC, &item->rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_MENU));

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT previous_font = (HFONT)SelectObject(item->hDC, font);
    SetBkMode(item->hDC, TRANSPARENT);

    RECT text_rect = item->rcItem;
    text_rect.left += 12;

    /* GitHub Sponsor button pink; the label keeps the normal (selected vs.
       not) menu text color instead of also being tinted pink. */
    SetTextColor(item->hDC, RGB(0xEA, 0x4A, 0xAA));
    DrawTextW(item->hDC, LP_SPONSOR_ITEM_TEXT, LP_SPONSOR_HEART_LEN, &text_rect,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    SIZE heart_extent;
    GetTextExtentPoint32W(item->hDC, LP_SPONSOR_ITEM_TEXT, LP_SPONSOR_HEART_LEN, &heart_extent);
    text_rect.left += heart_extent.cx;

    SetTextColor(item->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT));
    DrawTextW(item->hDC, LP_SPONSOR_ITEM_TEXT + LP_SPONSOR_HEART_LEN,
              (int)(sizeof(LP_SPONSOR_ITEM_TEXT) / sizeof(wchar_t)) - 1 - LP_SPONSOR_HEART_LEN,
              &text_rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    SelectObject(item->hDC, previous_font);
}

/* Dispatches taskbar, timer, menu, notification, theme, and shutdown messages. */
static LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    lp_tray_state_t *state = &g_tray;

    if (msg == state->wm_taskbar_created) {
        Shell_NotifyIconA(NIM_ADD, &state->nid);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, LP_TRAY_TIMER_ID, state->interval_ms, NULL);
        return 0;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lparam;
        if (mi != NULL && mi->CtlType == ODT_MENU && mi->itemID == IDM_SUPPORT) {
            measure_sponsor_item(mi);
            return TRUE;
        }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lparam;
        if (di != NULL && di->CtlType == ODT_MENU && di->itemID == IDM_SUPPORT) {
            draw_sponsor_item(di);
            return TRUE;
        }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
    case WM_SETTINGCHANGE:
        /* Windows broadcasts this with lParam pointing to "ImmersiveColorSet"
           when the user toggles light/dark mode; without it, the icon would
           only pick up the new theme on the next timer tick (up to
           interval_ms late). Other setting-change broadcasts fall through to
           DefWindowProcA below instead of being silently swallowed. */
        if (lparam != 0 && lstrcmpiA((LPCSTR)lparam, "ImmersiveColorSet") == 0) {
            refresh_icon_and_tooltip(state);
            return 0;
        }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    case WM_TIMER:
        if (wparam == LP_TRAY_TIMER_ID) {
            refresh_icon_and_tooltip(state);
            if (state->this_pc_hwnd != NULL && IsWindowVisible(state->this_pc_hwnd)) {
                show_this_pc(state, false);
            }
        }
        return 0;
    case WM_LP_TRAYICON:
        if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
            show_context_menu(state);
        } else if (lparam == WM_LBUTTONUP) {
            toggle_network_map(state);
        }
        return 0;
    case WM_LP_UPDATE_RESULT:
        refresh_icon_and_tooltip(state);
        if (wparam != 0) {
            show_update_notification(state);
        }
        return 0;
    case WM_LP_DISCOVERY_RESULT:
        show_discovery_notifications(state);
        refresh_open_device_views(state);
        return 0;
    case WM_LP_DEVICE_REFRESH_DONE:
        finish_device_refresh(state);
        refresh_open_device_views(state);
        return 0;
    case WM_LP_ISP_RESULT:
        if (state->isp_view_hwnd != NULL && IsWindowVisible(state->isp_view_hwnd)) {
            show_isp_view(state, false);
        }
        return 0;
    case WM_COPYDATA: {
        const COPYDATASTRUCT *copy_data = (const COPYDATASTRUCT *)lparam;
        if (copy_data == NULL || copy_data->lpData == NULL || copy_data->cbData == 0) {
            return FALSE;
        }
        if (copy_data->dwData == LP_NETWORK_MAP_COPYDATA_DEVICE_SELECTED) {
            const size_t data_size = copy_data->cbData < sizeof(state->pending_device_ip)
                                         ? copy_data->cbData
                                         : sizeof(state->pending_device_ip);
            const char *terminator = (const char *)memchr(copy_data->lpData, '\0', data_size);
            if (terminator == NULL) {
                return FALSE;
            }
            const size_t ip_size = (size_t)(terminator - (const char *)copy_data->lpData);
            memcpy(state->pending_device_ip, copy_data->lpData, ip_size + 1);
            EnterCriticalSection(&state->lock);
            state->has_selected_device_capture = false;
            LeaveCriticalSection(&state->lock);
            refresh_selected_device_identity(state, state->pending_device_ip);
            PostMessageA(hwnd, WM_LP_DEVICE_DETAILS_OPEN, 0, 0);
            return TRUE;
        }
        if (copy_data->dwData == LP_NETWORK_MAP_COPYDATA_ROUTER_SELECTED) {
            /* Posted rather than handled inline: this arrives inside the map's
               SendMessage, and activating a window from there re-enters the
               map's own deactivate handling. */
            const size_t data_size = copy_data->cbData < sizeof(state->pending_speed_meter_ip)
                                         ? copy_data->cbData
                                         : sizeof(state->pending_speed_meter_ip);
            const char *terminator = (const char *)memchr(copy_data->lpData, '\0', data_size);
            if (terminator == NULL) {
                return FALSE;
            }
            const size_t ip_size = (size_t)(terminator - (const char *)copy_data->lpData);
            memcpy(state->pending_speed_meter_ip, copy_data->lpData, ip_size + 1);
            LP_DEBUG("router selected on map: gateway=%s", state->pending_speed_meter_ip);
            PostMessageA(hwnd, WM_LP_SPEED_METER_OPEN, 0, 0);
            return TRUE;
        }
        if (copy_data->dwData == LP_SPEED_METER_COPYDATA_BACK) {
            PostMessageA(hwnd, WM_LP_NETWORK_MAP_OPEN, 0, 0);
            return TRUE;
        }
        if (copy_data->dwData == LP_NETWORK_MAP_COPYDATA_LOCAL_SELECTED) {
            PostMessageA(hwnd, WM_LP_THIS_PC_OPEN, 0, 0);
            return TRUE;
        }
        if (copy_data->dwData == LP_THIS_PC_COPYDATA_BACK) {
            PostMessageA(hwnd, WM_LP_NETWORK_MAP_OPEN, 0, 0);
            return TRUE;
        }
        if (copy_data->dwData == LP_DEVICE_DETAILS_COPYDATA_BACK) {
            PostMessageA(hwnd, WM_LP_NETWORK_MAP_OPEN, 0, 0);
            return TRUE;
        }
        if (copy_data->dwData == LP_NETWORK_MAP_COPYDATA_INTERNET_SELECTED) {
            /* Posted for the same re-entrancy reason as the router selection above. */
            PostMessageA(hwnd, WM_LP_ISP_VIEW_OPEN, 0, 0);
            return TRUE;
        }
        if (copy_data->dwData == LP_ISP_VIEW_COPYDATA_BACK) {
            PostMessageA(hwnd, WM_LP_NETWORK_MAP_OPEN, 0, 0);
            return TRUE;
        }
        return FALSE;
    }
    case WM_LP_SPEED_METER_OPEN:
        show_speed_meter(state, state->pending_speed_meter_ip);
        return 0;
    case WM_LP_THIS_PC_OPEN:
        show_this_pc(state, true);
        return 0;
    case WM_LP_DEVICE_DETAILS_OPEN:
        show_device_details(state, state->pending_device_ip, true);
        return 0;
    case WM_LP_ISP_VIEW_OPEN:
        show_isp_view(state, true);
        return 0;
    case WM_LP_NETWORK_MAP_OPEN:
        /* Map first: it inherits the foreground from the still-visible meter. */
        show_network_map(state);
        if (state->speed_meter_hwnd != NULL) {
            ShowWindow(state->speed_meter_hwnd, SW_HIDE);
        }
        if (state->this_pc_hwnd != NULL) {
            ShowWindow(state->this_pc_hwnd, SW_HIDE);
        }
        if (state->device_details_hwnd != NULL) {
            ShowWindow(state->device_details_hwnd, SW_HIDE);
        }
        if (state->isp_view_hwnd != NULL) {
            ShowWindow(state->isp_view_hwnd, SW_HIDE);
        }
        return 0;
    case WM_DESTROY: {
        bool can_delete_lock = true;
        lp_config_t config_to_save;
        config_to_save.mode = state->sampler.config.mode;
        memcpy(config_to_save.iface_name, state->sampler.config.iface_name,
               sizeof(config_to_save.iface_name));
        config_to_save.iface_name[sizeof(config_to_save.iface_name) - 1] = '\0';
        config_to_save.include_virtual = state->sampler.config.include_virtual;
        config_to_save.use_bits = InterlockedCompareExchange(&state->use_bits, 0, 0) != 0;
        config_to_save.interval_ms = state->interval_ms;
        config_to_save.packet_capture =
            InterlockedCompareExchange(&state->packet_capture, 0, 0) != 0;
        if (lp_config_save(&config_to_save) != LP_OK) {
            LP_WARN("failed to save config file");
        }

        KillTimer(hwnd, LP_TRAY_TIMER_ID);
        Shell_NotifyIconA(NIM_DELETE, &state->nid);
        if (state->network_map_hwnd != NULL) {
            DestroyWindow(state->network_map_hwnd);
            state->network_map_hwnd = NULL;
        }
        if (state->speed_meter_hwnd != NULL) {
            DestroyWindow(state->speed_meter_hwnd);
            state->speed_meter_hwnd = NULL;
        }
        if (state->this_pc_hwnd != NULL) {
            DestroyWindow(state->this_pc_hwnd);
            state->this_pc_hwnd = NULL;
        }
        if (state->device_details_hwnd != NULL) {
            DestroyWindow(state->device_details_hwnd);
            state->device_details_hwnd = NULL;
        }
        if (state->isp_view_hwnd != NULL) {
            DestroyWindow(state->isp_view_hwnd);
            state->isp_view_hwnd = NULL;
        }
        if (state->current_icon != NULL) {
            DestroyIcon(state->current_icon);
            state->current_icon = NULL;
        }
        InterlockedExchange(&state->stop_requested, 1);
        InterlockedExchange(&state->device_refresh_cancel, 1);
        lp_net_cancel_name_lookups();
        /* The device store is only closed once every thread that uses it has been joined;
           otherwise it is leaked, like the lock, rather than freed under a running worker. */
        bool can_close_device_store = true;
        if (state->thread != NULL) {
            WaitForSingleObject(state->thread, INFINITE);
            CloseHandle(state->thread);
            state->thread = NULL;
        }
        bool can_close_update_stop_event = true;
        if (state->discovery_thread != NULL) {
            SetEvent(state->update_stop_event);
            const DWORD wait_result =
                WaitForSingleObject(state->discovery_thread, LP_UPDATE_THREAD_SHUTDOWN_TIMEOUT_MS);
            if (wait_result == WAIT_TIMEOUT) {
                LP_WARN("discovery thread did not exit within %u ms; continuing shutdown",
                        (unsigned)LP_UPDATE_THREAD_SHUTDOWN_TIMEOUT_MS);
                can_close_update_stop_event = false;
                can_delete_lock = false;
                can_close_device_store = false;
            } else if (wait_result != WAIT_OBJECT_0) {
                LP_WARN("waiting for discovery thread failed during shutdown");
                can_close_update_stop_event = false;
                can_delete_lock = false;
                can_close_device_store = false;
            }
            CloseHandle(state->discovery_thread);
            state->discovery_thread = NULL;
        }
        if (state->update_thread != NULL) {
            SetEvent(state->update_stop_event);
            const DWORD wait_result =
                WaitForSingleObject(state->update_thread, LP_UPDATE_THREAD_SHUTDOWN_TIMEOUT_MS);
            if (wait_result == WAIT_TIMEOUT) {
                LP_WARN("update-check thread did not exit within %u ms; continuing shutdown",
                        (unsigned)LP_UPDATE_THREAD_SHUTDOWN_TIMEOUT_MS);
                can_delete_lock = false;
            } else if (wait_result == WAIT_OBJECT_0) {
            } else {
                LP_WARN("waiting for update-check thread failed during shutdown");
                can_delete_lock = false;
            }
            CloseHandle(state->update_thread);
            state->update_thread = NULL;
        }
        if (state->isp_thread != NULL) {
            SetEvent(state->update_stop_event);
            const DWORD wait_result =
                WaitForSingleObject(state->isp_thread, LP_UPDATE_THREAD_SHUTDOWN_TIMEOUT_MS);
            if (wait_result == WAIT_TIMEOUT) {
                LP_WARN("ISP lookup thread did not exit within %u ms; continuing shutdown",
                        (unsigned)LP_UPDATE_THREAD_SHUTDOWN_TIMEOUT_MS);
                can_close_update_stop_event = false;
                can_delete_lock = false;
            } else if (wait_result != WAIT_OBJECT_0) {
                LP_WARN("waiting for ISP lookup thread failed during shutdown");
                can_close_update_stop_event = false;
                can_delete_lock = false;
            }
            CloseHandle(state->isp_thread);
            state->isp_thread = NULL;
        }
        if (state->download_thread != NULL) {
            const DWORD wait_result =
                WaitForSingleObject(state->download_thread, LP_UPDATE_THREAD_SHUTDOWN_TIMEOUT_MS);
            if (wait_result == WAIT_TIMEOUT) {
                LP_WARN("update-download thread did not exit within %u ms; continuing shutdown",
                        (unsigned)LP_UPDATE_THREAD_SHUTDOWN_TIMEOUT_MS);
            } else if (wait_result != WAIT_OBJECT_0) {
                LP_WARN("waiting for update-download thread failed during shutdown");
            }
            CloseHandle(state->download_thread);
            state->download_thread = NULL;
        }
        if (state->device_refresh_thread != NULL) {
            const DWORD wait_result = WaitForSingleObject(state->device_refresh_thread,
                                                          LP_UPDATE_THREAD_SHUTDOWN_TIMEOUT_MS);
            if (wait_result == WAIT_TIMEOUT) {
                LP_WARN("device-refresh thread did not exit within %u ms; continuing shutdown",
                        (unsigned)LP_UPDATE_THREAD_SHUTDOWN_TIMEOUT_MS);
                can_delete_lock = false;
                can_close_device_store = false;
            } else if (wait_result != WAIT_OBJECT_0) {
                LP_WARN("waiting for device-refresh thread failed during shutdown");
                can_delete_lock = false;
                can_close_device_store = false;
            }
            CloseHandle(state->device_refresh_thread);
            state->device_refresh_thread = NULL;
        }
        if (state->update_stop_event != NULL && can_close_update_stop_event) {
            CloseHandle(state->update_stop_event);
            state->update_stop_event = NULL;
        }
        if (state->isp_refresh_event != NULL) {
            CloseHandle(state->isp_refresh_event);
            state->isp_refresh_event = NULL;
        }
        if (can_close_device_store) {
            lp_win32_device_store_close(state->device_store);
            state->device_store = NULL;
        } else {
            LP_WARN("leaving the device store open: a worker thread may still be using it");
        }
        if (can_delete_lock) {
            DeleteCriticalSection(&state->lock);
        }
        PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
}

/* Initializes the singleton tray process, starts workers, and runs its message loop. */
int lp_tray_run(const lp_sampler_config_t *config, bool use_bits, unsigned interval_ms,
                bool packet_capture)
{
    LP_INFO("initializing tray runtime");
    /* Before any window exists: popups and the tray icon then render at the monitor's real DPI
       instead of being bitmap-stretched by Windows. */
    lp_ui_enable_dpi_awareness();
    lp_win32_set_app_user_model_id();
    HANDLE single_instance_mutex = CreateMutexA(NULL, FALSE, "Local\\LinkPulse_SingleInstance");
    if (single_instance_mutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        LP_ERROR("LinkPulse is already running (check the system tray)");
        if (single_instance_mutex != NULL) {
            CloseHandle(single_instance_mutex);
        }
        return 1;
    }

    memset(&g_tray, 0, sizeof(g_tray));
    InitializeCriticalSection(&g_tray.lock);
    g_tray.interval_ms = (interval_ms == 0) ? 1000 : interval_ms;
    g_tray.use_bits = use_bits ? 1 : 0;
    g_tray.packet_capture = packet_capture ? 1 : 0;
    g_tray.wm_taskbar_created = RegisterWindowMessageA("TaskbarCreated");

    lp_sampler_init(&g_tray.sampler, config);
    const lp_sampler_sources_t sources = {lp_net_snapshot, lp_net_default_iface,
                                          lp_clock_monotonic_ns};
    lp_sampler_set_sources(&g_tray.sampler, &sources);
    lp_discovery_init(&g_tray.discovery);
    const lp_discovery_sources_t discovery_sources = {lp_net_neighbor_snapshot,
                                                      lp_net_local_networks};
    lp_discovery_set_sources(&g_tray.discovery, &discovery_sources);
    if (lp_win32_device_store_open(&g_tray.device_store) != 0) {
        LP_WARN("device metadata store unavailable; continuing without persistence");
    }

    const HINSTANCE instance = GetModuleHandleA(NULL);
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tray_wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = "LinkPulseTrayWindow";
    if (RegisterClassExA(&wc) == 0) {
        LP_ERROR("failed to register window class");
        DeleteCriticalSection(&g_tray.lock);
        CloseHandle(single_instance_mutex);
        return 1;
    }

    /* Never shown: it exists only to own the tray icon and receive messages. */
    g_tray.hwnd = CreateWindowExA(0, wc.lpszClassName, "LinkPulse", WS_OVERLAPPED, 0, 0, 0, 0, NULL,
                                  NULL, instance, NULL);
    if (g_tray.hwnd == NULL) {
        LP_ERROR("failed to create the tray window");
        DeleteCriticalSection(&g_tray.lock);
        CloseHandle(single_instance_mutex);
        return 1;
    }
    g_tray.network_map_hwnd = lp_network_map_create(instance, g_tray.hwnd);
    if (g_tray.network_map_hwnd == NULL) {
        LP_WARN("failed to create network map window");
    }
    g_tray.speed_meter_hwnd = lp_speed_meter_create(instance, g_tray.hwnd);
    if (g_tray.speed_meter_hwnd == NULL) {
        LP_WARN("failed to create speed meter window");
    }
    g_tray.this_pc_hwnd = lp_this_pc_create(instance, g_tray.hwnd);
    if (g_tray.this_pc_hwnd == NULL) {
        LP_WARN("failed to create this PC window");
    }
    g_tray.device_details_hwnd = lp_device_details_create(instance, g_tray.hwnd);
    if (g_tray.device_details_hwnd == NULL) {
        LP_WARN("failed to create device details window");
    }
    g_tray.isp_view_hwnd = lp_isp_view_create(instance, g_tray.hwnd);
    if (g_tray.isp_view_hwnd == NULL) {
        LP_WARN("failed to create ISP view window");
    }

    g_tray.nid.cbSize = sizeof(g_tray.nid);
    g_tray.nid.hWnd = g_tray.hwnd;
    g_tray.nid.uID = LP_TRAY_ICON_UID;
    g_tray.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.nid.uCallbackMessage = WM_LP_TRAYICON;
    g_tray.current_icon = render_icon(NULL, 0);
    if (g_tray.current_icon == NULL) {
        LP_ERROR("failed to render initial tray icon");
        DestroyWindow(g_tray.hwnd);
        CloseHandle(single_instance_mutex);
        return 1;
    }
    g_tray.nid.hIcon = g_tray.current_icon;
    snprintf(g_tray.nid.szTip, sizeof(g_tray.nid.szTip), "LinkPulse\nstarting...");
    if (!Shell_NotifyIconA(NIM_ADD, &g_tray.nid)) {
        LP_ERROR("failed to add tray icon");
        DestroyWindow(g_tray.hwnd);
        CloseHandle(single_instance_mutex);
        return 1;
    }
    LP_INFO("tray icon initialized");

    g_tray.update_stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (g_tray.update_stop_event == NULL) {
        LP_WARN("failed to create update-check stop event");
    }
    g_tray.isp_refresh_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (g_tray.isp_refresh_event == NULL) {
        LP_WARN("failed to create ISP refresh event");
    }

    g_tray.thread = CreateThread(NULL, 0, sampler_thread_proc, &g_tray, 0, NULL);
    if (g_tray.thread == NULL) {
        LP_ERROR("failed to start the sampler thread");
        DestroyWindow(g_tray.hwnd);
    } else {
        if (g_tray.update_stop_event != NULL) {
            g_tray.update_thread = CreateThread(NULL, 0, update_thread_proc, &g_tray, 0, NULL);
            g_tray.discovery_thread =
                CreateThread(NULL, 0, discovery_thread_proc, &g_tray, 0, NULL);
            g_tray.isp_thread = CreateThread(NULL, 0, isp_thread_proc, &g_tray, 0, NULL);
        }
        if (g_tray.update_stop_event != NULL && g_tray.update_thread == NULL) {
            LP_WARN("failed to start the update-check thread");
        }
        if (g_tray.update_stop_event != NULL && g_tray.discovery_thread == NULL) {
            LP_WARN("failed to start the discovery thread");
        }
        if (g_tray.update_stop_event != NULL && g_tray.isp_thread == NULL) {
            LP_WARN("failed to start the ISP lookup thread");
        }
        LP_INFO("tray worker threads started");
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    CloseHandle(single_instance_mutex);
    LP_INFO("tray runtime stopped");
    return (int)msg.wParam;
}
