#include "linkpulse/tray.h"

#include "linkpulse/clock.h"
#include "linkpulse/format.h"
#include "linkpulse/log.h"
#include "linkpulse/net.h"
#include "linkpulse/sampler.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <shellapi.h>

#define WM_LP_TRAYICON (WM_APP + 1)
#define LP_TRAY_TIMER_ID 1
#define LP_TRAY_ICON_UID 1

#define IDM_PAUSE 2001
#define IDM_UNITS_BITS 2002
#define IDM_EXIT 2003

/* Enough points to fill the icon at the largest realistic SM_CXSMICON (24px at
   125% scaling); render_icon only ever reads the last `size` of them. */
#define LP_TRAY_GRAPH_POINTS 32

/* Cross-thread shared state, guarded by `lock`. Everything else below (nid,
   icon handles, timer/thread handles) is only ever touched by the UI thread. */
typedef struct {
    CRITICAL_SECTION lock;
    lp_rate_sample_t latest;
    lp_status_t latest_status;
    bool has_sample;
    uint64_t rx_history[LP_TRAY_GRAPH_POINTS]; /* oldest first */
    size_t rx_history_count;

    volatile LONG stop_requested;
    volatile LONG paused;
    volatile LONG use_bits;

    lp_sampler_t sampler;
    unsigned interval_ms;
    HANDLE thread;

    HWND hwnd;
    NOTIFYICONDATAA nid;
    HICON current_icon;
    UINT wm_taskbar_created;
} lp_tray_state_t;

/* One tray per process (enforced by the single-instance mutex), so a single
   global avoids threading GWLP_USERDATA through every message handler. */
static lp_tray_state_t g_tray;

/* The taskbar (and its clock/tray icons) follow "SystemUsesLightTheme", not the
   separate "AppsUseLightTheme" value that only affects app windows. Defaults
   to light (Windows' own default) if the value is missing. */
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

static DWORD WINAPI sampler_thread_proc(LPVOID param)
{
    lp_tray_state_t *state = (lp_tray_state_t *)param;

    while (!InterlockedCompareExchange(&state->stop_requested, 0, 0)) {
        if (!InterlockedCompareExchange(&state->paused, 0, 0)) {
            lp_rate_sample_t sample;
            const lp_status_t status = lp_sampler_poll(&state->sampler, &sample);

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
            }
            LeaveCriticalSection(&state->lock);
        }
        Sleep(state->interval_ms);
    }
    return 0;
}

static void show_context_menu(lp_tray_state_t *state)
{
    HMENU menu = CreatePopupMenu();
    if (menu == NULL) {
        return;
    }

    const bool paused = InterlockedCompareExchange(&state->paused, 0, 0) != 0;
    const bool use_bits = InterlockedCompareExchange(&state->use_bits, 0, 0) != 0;

    AppendMenuA(menu, MF_STRING | (paused ? MF_CHECKED : 0), IDM_PAUSE,
                paused ? "Resume" : "Pause");
    AppendMenuA(menu, MF_STRING | (use_bits ? MF_CHECKED : 0), IDM_UNITS_BITS, "Show bits/s");
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
    case IDM_EXIT:
        DestroyWindow(state->hwnd);
        break;
    default:
        break;
    }
}

static void refresh_icon_and_tooltip(lp_tray_state_t *state)
{
    lp_rate_sample_t sample = {0, 0, 0};
    lp_status_t status = LP_ERR_NOT_FOUND;
    bool has_sample = false;
    uint64_t history[LP_TRAY_GRAPH_POINTS];
    size_t history_count = 0;

    EnterCriticalSection(&state->lock);
    sample = state->latest;
    status = state->latest_status;
    has_sample = state->has_sample;
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

    Shell_NotifyIconA(NIM_MODIFY, &state->nid);
}

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
    case WM_SETTINGCHANGE:
        /* Windows broadcasts this with lParam pointing to "ImmersiveColorSet"
           when the user toggles light/dark mode; without it, the icon would
           only pick up the new theme on the next timer tick (up to
           interval_ms late). */
        if (lparam != 0 && lstrcmpiA((LPCSTR)lparam, "ImmersiveColorSet") == 0) {
            refresh_icon_and_tooltip(state);
        }
        return 0;
    case WM_TIMER:
        if (wparam == LP_TRAY_TIMER_ID) {
            refresh_icon_and_tooltip(state);
        }
        return 0;
    case WM_LP_TRAYICON:
        if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
            show_context_menu(state);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, LP_TRAY_TIMER_ID);
        Shell_NotifyIconA(NIM_DELETE, &state->nid);
        if (state->current_icon != NULL) {
            DestroyIcon(state->current_icon);
            state->current_icon = NULL;
        }
        InterlockedExchange(&state->stop_requested, 1);
        if (state->thread != NULL) {
            WaitForSingleObject(state->thread, INFINITE);
            CloseHandle(state->thread);
            state->thread = NULL;
        }
        DeleteCriticalSection(&state->lock);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
}

int lp_tray_run(const lp_sampler_config_t *config, bool use_bits, unsigned interval_ms)
{
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
    g_tray.wm_taskbar_created = RegisterWindowMessageA("TaskbarCreated");

    lp_sampler_init(&g_tray.sampler, config);
    const lp_sampler_sources_t sources = {lp_net_snapshot, lp_net_default_iface,
                                          lp_clock_monotonic_ns};
    lp_sampler_set_sources(&g_tray.sampler, &sources);

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

    g_tray.thread = CreateThread(NULL, 0, sampler_thread_proc, &g_tray, 0, NULL);
    if (g_tray.thread == NULL) {
        LP_ERROR("failed to start the sampler thread");
        DestroyWindow(g_tray.hwnd);
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    CloseHandle(single_instance_mutex);
    return (int)msg.wParam;
}
