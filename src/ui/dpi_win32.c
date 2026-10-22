#include "dpi_win32.h"

#include <stdbool.h>
#include <string.h>

/* Corner radius of every popup frame, in layout units. */
#define LP_UI_POPUP_CORNER_RADIUS 24
/* Gap between a docked popup and the work area's right and bottom edges, in layout units. */
#define LP_UI_POPUP_MARGIN_RIGHT 12
#define LP_UI_POPUP_MARGIN_BOTTOM 8

/* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, spelled out so older SDK headers still build. */
#define LP_UI_DPI_CONTEXT_PER_MONITOR_V2 ((HANDLE)(LONG_PTR)-4)
#define LP_UI_PROCESS_PER_MONITOR_DPI_AWARE 2
#define LP_UI_MDT_EFFECTIVE_DPI 0

typedef BOOL(WINAPI *lp_set_dpi_context_fn)(HANDLE);
typedef HRESULT(WINAPI *lp_set_process_dpi_awareness_fn)(int);
typedef UINT(WINAPI *lp_get_dpi_for_window_fn)(HWND);
typedef HRESULT(WINAPI *lp_get_dpi_for_monitor_fn)(HMONITOR, int, UINT *, UINT *);

typedef void (*lp_proc_fn)(void);

/* Goes through void (*)(void) so GCC's -Wcast-function-type accepts the per-symbol casts. The
   functions are looked up at runtime so the build does not require Windows 10 import libraries. */
static lp_proc_fn resolve(const char *module_name, const char *symbol)
{
    HMODULE module = GetModuleHandleA(module_name);
    if (module == NULL) {
        module = LoadLibraryExA(module_name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    return module != NULL ? (lp_proc_fn)GetProcAddress(module, symbol) : NULL;
}

void lp_ui_enable_dpi_awareness(void)
{
    const lp_set_dpi_context_fn set_context =
        (lp_set_dpi_context_fn)resolve("user32.dll", "SetProcessDpiAwarenessContext");
    if (set_context != NULL && set_context(LP_UI_DPI_CONTEXT_PER_MONITOR_V2)) {
        return;
    }
    const lp_set_process_dpi_awareness_fn set_awareness =
        (lp_set_process_dpi_awareness_fn)resolve("shcore.dll", "SetProcessDpiAwareness");
    if (set_awareness != NULL && SUCCEEDED(set_awareness(LP_UI_PROCESS_PER_MONITOR_DPI_AWARE))) {
        return;
    }
    (void)SetProcessDPIAware();
}

static UINT valid_dpi(UINT dpi)
{
    return dpi != 0 ? dpi : LP_UI_DEFAULT_DPI;
}

UINT lp_ui_window_dpi(HWND window)
{
    static lp_get_dpi_for_window_fn get_dpi = NULL;
    static bool resolved = false;
    if (!resolved) {
        get_dpi = (lp_get_dpi_for_window_fn)resolve("user32.dll", "GetDpiForWindow");
        resolved = true;
    }
    if (window != NULL && get_dpi != NULL) {
        return valid_dpi(get_dpi(window));
    }
    HDC dc = GetDC(window);
    if (dc == NULL) {
        return LP_UI_DEFAULT_DPI;
    }
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(window, dc);
    return dpi > 0 ? (UINT)dpi : LP_UI_DEFAULT_DPI;
}

UINT lp_ui_monitor_dpi(HMONITOR monitor)
{
    static lp_get_dpi_for_monitor_fn get_dpi = NULL;
    static bool resolved = false;
    if (!resolved) {
        get_dpi = (lp_get_dpi_for_monitor_fn)resolve("shcore.dll", "GetDpiForMonitor");
        resolved = true;
    }
    UINT dpi_x = 0;
    UINT dpi_y = 0;
    if (monitor != NULL && get_dpi != NULL &&
        SUCCEEDED(get_dpi(monitor, LP_UI_MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
        return valid_dpi(dpi_x);
    }
    return lp_ui_window_dpi(NULL);
}

int lp_ui_scale(int value, UINT dpi)
{
    return MulDiv(value, (int)valid_dpi(dpi), (int)LP_UI_DEFAULT_DPI);
}

int lp_ui_unscale(int value, UINT dpi)
{
    return MulDiv(value, (int)LP_UI_DEFAULT_DPI, (int)valid_dpi(dpi));
}

void lp_ui_begin_scaled_paint(HDC dc, UINT dpi)
{
    if (dc == NULL || valid_dpi(dpi) == LP_UI_DEFAULT_DPI) {
        return;
    }
    const float factor = (float)valid_dpi(dpi) / (float)LP_UI_DEFAULT_DPI;
    XFORM transform;
    memset(&transform, 0, sizeof(transform));
    transform.eM11 = factor;
    transform.eM22 = factor;
    (void)SetGraphicsMode(dc, GM_ADVANCED);
    (void)SetWorldTransform(dc, &transform);
}

void lp_ui_end_scaled_paint(HDC dc, UINT dpi)
{
    if (dc == NULL || valid_dpi(dpi) == LP_UI_DEFAULT_DPI) {
        return;
    }
    (void)ModifyWorldTransform(dc, NULL, MWT_IDENTITY);
}

RECT lp_ui_layout_client_rect(HWND window, UINT dpi)
{
    RECT client;
    GetClientRect(window, &client);
    client.left = lp_ui_unscale(client.left, dpi);
    client.top = lp_ui_unscale(client.top, dpi);
    client.right = lp_ui_unscale(client.right, dpi);
    client.bottom = lp_ui_unscale(client.bottom, dpi);
    return client;
}

POINT lp_ui_layout_point(LPARAM lparam, UINT dpi)
{
    POINT point;
    point.x = lp_ui_unscale((short)LOWORD(lparam), dpi);
    point.y = lp_ui_unscale((short)HIWORD(lparam), dpi);
    return point;
}

RECT lp_ui_popup_rect(HMONITOR monitor, int layout_width, int layout_height, UINT *out_dpi)
{
    MONITORINFO monitor_info;
    memset(&monitor_info, 0, sizeof(monitor_info));
    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoA(monitor, &monitor_info);
    const UINT dpi = lp_ui_monitor_dpi(monitor);
    const int width = lp_ui_scale(layout_width, dpi);
    const int height = lp_ui_scale(layout_height, dpi);
    RECT rect;
    rect.right = monitor_info.rcWork.right - lp_ui_scale(LP_UI_POPUP_MARGIN_RIGHT, dpi);
    rect.bottom = monitor_info.rcWork.bottom - lp_ui_scale(LP_UI_POPUP_MARGIN_BOTTOM, dpi);
    rect.left = rect.right - width;
    rect.top = rect.bottom - height;
    if (out_dpi != NULL) {
        *out_dpi = dpi;
    }
    return rect;
}

void lp_ui_apply_popup_region(HWND window, int width, int height, UINT dpi)
{
    const int radius = lp_ui_scale(LP_UI_POPUP_CORNER_RADIUS, dpi);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    /* On success the system owns the region. */
    if (region != NULL && SetWindowRgn(window, region, IsWindowVisible(window)) == 0) {
        DeleteObject(region);
    }
}

void lp_ui_handle_dpi_changed(HWND window, WPARAM wparam, int layout_width, int layout_height)
{
    const UINT dpi = valid_dpi(HIWORD(wparam));
    const int width = lp_ui_scale(layout_width, dpi);
    const int height = lp_ui_scale(layout_height, dpi);
    RECT current;
    GetWindowRect(window, &current);
    SetWindowPos(window, NULL, current.right - width, current.bottom - height, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    lp_ui_apply_popup_region(window, width, height, dpi);
    InvalidateRect(window, NULL, FALSE);
}
