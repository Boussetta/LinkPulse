#include "isp_view_win32.h"

#include "dpi_win32.h"

#include <stdio.h>
#include <string.h>

#define LP_ISP_VIEW_WIDTH 380
#define LP_ISP_VIEW_HEIGHT 740
#define LP_ISP_VIEW_ROW_HEIGHT 32
#define LP_ISP_VIEW_PLACE_MAX (LP_ISP_PLACE_MAX * 2 + LP_ISP_COUNTRY_MAX + 8)

typedef struct {
    HFONT title_font;
    HFONT label_font;
    HFONT small_font;
    HBRUSH background_brush;
    bool available;
    bool lookup_failed;
    lp_isp_info_t info;
} lp_isp_view_state_t;

static lp_isp_view_state_t *state_of(HWND window)
{
    return (lp_isp_view_state_t *)GetWindowLongPtrA(window, GWLP_USERDATA);
}

static bool is_taskbar_light_theme(void)
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    return RegGetValueA(HKEY_CURRENT_USER,
                        "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        "SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &value,
                        &size) != ERROR_SUCCESS ||
           value != 0;
}

static void draw_text(HDC dc, HFONT font, COLORREF color, const char *text, RECT rect, UINT flags)
{
    HFONT old_font = (HFONT)SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextA(dc, text, -1, &rect, flags | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
}

static RECT back_rect(void)
{
    RECT rect = {14, 10, 108, 44};
    return rect;
}

static void go_back(HWND window)
{
    HWND owner = GetWindow(window, GW_OWNER);
    if (owner == NULL) {
        ShowWindow(window, SW_HIDE);
        return;
    }
    COPYDATASTRUCT copy_data = {LP_ISP_VIEW_COPYDATA_BACK, 1, (PVOID) ""};
    (void)SendMessageA(owner, WM_COPYDATA, (WPARAM)window, (LPARAM)&copy_data);
}

static const char *or_dash(const char *value)
{
    return value[0] != '\0' ? value : "--";
}

static int draw_info_row(HDC dc, HFONT font, COLORREF muted, COLORREF text, const char *label,
                         const char *value, RECT client, int y)
{
    RECT label_rect = {32, y, client.right / 2 - 16, y + LP_ISP_VIEW_ROW_HEIGHT};
    RECT value_rect = {client.right / 2 - 16, y, client.right - 32, y + LP_ISP_VIEW_ROW_HEIGHT};
    draw_text(dc, font, muted, label, label_rect, DT_LEFT | DT_VCENTER);
    draw_text(dc, font, text, or_dash(value), value_rect, DT_RIGHT | DT_VCENTER);
    return y + LP_ISP_VIEW_ROW_HEIGHT;
}

/* Joins city, region and country, skipping whichever parts the lookup left empty. */
static void format_place(const lp_isp_info_t *info, char *out, size_t out_cap)
{
    const char *parts[3] = {info->city, info->region, info->country};
    size_t length = 0;
    out[0] = '\0';
    for (size_t i = 0; i < 3; ++i) {
        if (parts[i][0] == '\0' || length >= out_cap) {
            continue;
        }
        const int written =
            snprintf(out + length, out_cap - length, "%s%s", length > 0 ? ", " : "", parts[i]);
        if (written > 0) {
            length += (size_t)written;
        }
    }
}

static void paint_isp_view(HWND window, HDC target_dc)
{
    lp_isp_view_state_t *state = state_of(window);
    if (state == NULL) {
        return;
    }

    const UINT dpi = lp_ui_window_dpi(window);
    RECT physical;
    GetClientRect(window, &physical);
    const RECT client = lp_ui_layout_client_rect(window, dpi);
    HDC dc = CreateCompatibleDC(target_dc);
    HBITMAP bitmap = CreateCompatibleBitmap(target_dc, physical.right, physical.bottom);
    if (dc == NULL || bitmap == NULL) {
        if (bitmap != NULL) {
            DeleteObject(bitmap);
        }
        if (dc != NULL) {
            DeleteDC(dc);
        }
        return;
    }
    HBITMAP old_bitmap = (HBITMAP)SelectObject(dc, bitmap);
    lp_ui_begin_scaled_paint(dc, dpi);
    FillRect(dc, &client, state->background_brush);

    const bool light = is_taskbar_light_theme();
    const COLORREF text = light ? RGB(30, 35, 42) : RGB(240, 243, 247);
    const COLORREF muted = light ? RGB(90, 99, 112) : RGB(166, 174, 185);
    const COLORREF line = light ? RGB(151, 161, 174) : RGB(85, 97, 112);
    const COLORREF panel = light ? RGB(244, 246, 248) : RGB(36, 39, 45);

    RECT close_rect = {client.right - 42, 8, client.right - 8, 42};
    draw_text(dc, state->label_font, muted, "x", close_rect, DT_CENTER | DT_VCENTER);
    const RECT back = back_rect();
    draw_text(dc, state->label_font, muted, "\x3C  Network", back, DT_LEFT | DT_VCENTER);

    const lp_isp_info_t *info = &state->info;
    RECT title_rect = {24, 52, client.right - 24, 82};
    draw_text(dc, state->title_font, text, "Internet provider", title_rect, DT_LEFT | DT_VCENTER);
    const char *subtitle = state->available && info->isp[0] != '\0' ? info->isp
                           : state->available                       ? "Unknown provider"
                           : state->lookup_failed                   ? "Lookup failed"
                                                                    : "Looking up...";
    RECT subtitle_rect = {24, 82, client.right - 24, 106};
    draw_text(dc, state->small_font, muted, subtitle, subtitle_rect, DT_LEFT | DT_VCENTER);

    if (state->available) {
        char place[LP_ISP_VIEW_PLACE_MAX];
        format_place(info, place, sizeof(place));
        int row_y = 128;
        row_y =
            draw_info_row(dc, state->label_font, muted, text, "Provider", info->isp, client, row_y);
        row_y = draw_info_row(dc, state->label_font, muted, text, "ASN", info->asn, client, row_y);
        row_y = draw_info_row(dc, state->label_font, muted, text, "Public IP", info->public_ip,
                              client, row_y);
        row_y = draw_info_row(dc, state->label_font, muted, text, "Hostname", info->hostname,
                              client, row_y);
        row_y = draw_info_row(dc, state->label_font, muted, text, "Location", place, client, row_y);
        row_y = draw_info_row(dc, state->label_font, muted, text, "Postal code", info->postal,
                              client, row_y);
        row_y = draw_info_row(dc, state->label_font, muted, text, "Time zone", info->timezone,
                              client, row_y);
        (void)draw_info_row(dc, state->label_font, muted, text, "Coordinates", info->loc, client,
                            row_y);
    } else {
        RECT status_panel = {24, 128, client.right - 24, 238};
        HBRUSH panel_brush = CreateSolidBrush(panel);
        FillRect(dc, &status_panel, panel_brush);
        DeleteObject(panel_brush);
        RECT status_rect = {40, status_panel.top + 18, status_panel.right - 16,
                            status_panel.top + 44};
        draw_text(dc, state->label_font, text,
                  state->lookup_failed ? "ISP details unavailable" : "Fetching ISP details...",
                  status_rect, DT_LEFT | DT_VCENTER);
        RECT detail_rect = {40, status_panel.top + 48, status_panel.right - 24,
                            status_panel.bottom - 12};
        HFONT old_font = (HFONT)SelectObject(dc, state->small_font);
        SetTextColor(dc, muted);
        SetBkMode(dc, TRANSPARENT);
        DrawTextA(dc,
                  state->lookup_failed
                      ? "The public IP lookup did not answer. LinkPulse retries automatically."
                      : "Asking the lookup service for your public IP and provider.",
                  -1, &detail_rect, DT_LEFT | DT_WORDBREAK);
        SelectObject(dc, old_font);
    }

    HBRUSH old_brush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN frame_pen = CreatePen(PS_SOLID, 1, line);
    HPEN old_pen = (HPEN)SelectObject(dc, frame_pen);
    RoundRect(dc, 0, 0, client.right, client.bottom, 24, 24);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(frame_pen);

    lp_ui_end_scaled_paint(dc, dpi);
    BitBlt(target_dc, 0, 0, physical.right, physical.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

static LRESULT CALLBACK isp_view_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        lp_isp_view_state_t *state =
            (lp_isp_view_state_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
        if (state == NULL) {
            return -1;
        }
        state->title_font = CreateFontA(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        state->label_font = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        state->small_font = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        state->background_brush =
            CreateSolidBrush(is_taskbar_light_theme() ? RGB(250, 251, 252) : RGB(28, 30, 34));
        SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)state);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        paint_isp_view(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            ShowWindow(window, SW_HIDE);
        }
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            go_back(window);
        }
        return 0;
    case WM_DPICHANGED:
        lp_ui_handle_dpi_changed(window, wparam, LP_ISP_VIEW_WIDTH, LP_ISP_VIEW_HEIGHT);
        return 0;
    case WM_LBUTTONUP: {
        const UINT dpi = lp_ui_window_dpi(window);
        const POINT point = lp_ui_layout_point(lparam, dpi);
        const int x = point.x;
        const int y = point.y;
        const RECT client = lp_ui_layout_client_rect(window, dpi);
        if (x >= client.right - 58 && y <= 58) {
            ShowWindow(window, SW_HIDE);
            return 0;
        }
        const RECT back = back_rect();
        if (x >= back.left && x <= back.right && y >= back.top && y <= back.bottom) {
            go_back(window);
        }
        return 0;
    }
    case WM_SETTINGCHANGE: {
        lp_isp_view_state_t *state = state_of(window);
        if (state != NULL) {
            DeleteObject(state->background_brush);
            state->background_brush =
                CreateSolidBrush(is_taskbar_light_theme() ? RGB(250, 251, 252) : RGB(28, 30, 34));
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    }
    case WM_DESTROY: {
        lp_isp_view_state_t *state = state_of(window);
        if (state != NULL) {
            DeleteObject(state->title_font);
            DeleteObject(state->label_font);
            DeleteObject(state->small_font);
            DeleteObject(state->background_brush);
            SetWindowLongPtrA(window, GWLP_USERDATA, 0);
            HeapFree(GetProcessHeap(), 0, state);
        }
        return 0;
    }
    default:
        return DefWindowProcA(window, message, wparam, lparam);
    }
}

HWND lp_isp_view_create(HINSTANCE instance, HWND owner)
{
    WNDCLASSEXA window_class = {0};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = isp_view_wndproc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.lpszClassName = "LinkPulseIspViewWindow";
    if (RegisterClassExA(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return NULL;
    }
    HWND window = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, window_class.lpszClassName,
                                  "LinkPulse ISP", WS_POPUP, 0, 0, LP_ISP_VIEW_WIDTH,
                                  LP_ISP_VIEW_HEIGHT, owner, NULL, instance, NULL);
    if (window != NULL) {
        HRGN region =
            CreateRoundRectRgn(0, 0, LP_ISP_VIEW_WIDTH + 1, LP_ISP_VIEW_HEIGHT + 1, 24, 24);
        if (region != NULL && SetWindowRgn(window, region, FALSE) == 0) {
            DeleteObject(region);
        }
    }
    return window;
}

/* Copies the ISP snapshot into the window state; returns false if the window is not ready. */
static bool store_snapshot(HWND window, const lp_isp_info_t *info, bool lookup_failed)
{
    if (window == NULL) {
        return false;
    }
    lp_isp_view_state_t *state = state_of(window);
    if (state == NULL) {
        return false;
    }
    state->available = info != NULL;
    state->lookup_failed = lookup_failed;
    if (info != NULL) {
        state->info = *info;
    } else {
        memset(&state->info, 0, sizeof(state->info));
    }
    return true;
}

void lp_isp_view_update(HWND window, const lp_isp_info_t *info, bool lookup_failed)
{
    if (store_snapshot(window, info, lookup_failed)) {
        InvalidateRect(window, NULL, FALSE);
    }
}

void lp_isp_view_show(HWND window, const lp_isp_info_t *info, bool lookup_failed)
{
    if (!store_snapshot(window, info, lookup_failed)) {
        return;
    }

    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    UINT dpi = LP_UI_DEFAULT_DPI;
    const RECT rect = lp_ui_popup_rect(monitor, LP_ISP_VIEW_WIDTH, LP_ISP_VIEW_HEIGHT, &dpi);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    SetWindowPos(window, HWND_TOPMOST, rect.left, rect.top, width, height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    lp_ui_apply_popup_region(window, width, height, dpi);
    SetForegroundWindow(window);
    InvalidateRect(window, NULL, FALSE);
}
