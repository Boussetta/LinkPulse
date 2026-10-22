#include "this_pc_win32.h"

#include "dpi_win32.h"

#include "linkpulse/format.h"
#include "linkpulse/log.h"

#include <stdio.h>
#include <string.h>

#define LP_THIS_PC_WIDTH 380
#define LP_THIS_PC_HEIGHT 740
#define LP_THIS_PC_LABEL_MAX (LP_HOSTNAME_MAX + LP_IP_STR_MAX + 8)
#define LP_THIS_PC_GRAPH_TOP 118
#define LP_THIS_PC_GRAPH_HEIGHT 160
#define LP_THIS_PC_INFO_ROW_HEIGHT 30

typedef struct {
    HFONT title_font;
    HFONT value_font;
    HFONT label_font;
    HFONT small_font;
    HBRUSH background_brush;

    char hostname[LP_HOSTNAME_MAX];
    lp_local_network_list_t networks;
    uint64_t rx_history[LP_THIS_PC_GRAPH_POINTS];
    uint64_t tx_history[LP_THIS_PC_GRAPH_POINTS];
    size_t history_count;
    bool use_bits;
} lp_this_pc_state_t;

static lp_this_pc_state_t *state_of(HWND window)
{
    return (lp_this_pc_state_t *)GetWindowLongPtrA(window, GWLP_USERDATA);
}

/* Reads the taskbar theme setting so the window matches the map and tray icon. */
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

/* Hides the popup and asks the owner to bring the network map back up. */
static void go_back(HWND window)
{
    HWND owner = GetWindow(window, GW_OWNER);
    if (owner == NULL) {
        ShowWindow(window, SW_HIDE);
        return;
    }
    COPYDATASTRUCT copy_data;
    memset(&copy_data, 0, sizeof(copy_data));
    copy_data.dwData = LP_THIS_PC_COPYDATA_BACK;
    copy_data.cbData = 1;
    copy_data.lpData = (PVOID) "";
    (void)SendMessageA(owner, WM_COPYDATA, (WPARAM)window, (LPARAM)&copy_data);
}

/* Draws a Task-Manager-style throughput graph: a filled area for download and
   a dashed line for upload, both scaled to the largest sample in view. */
static void draw_throughput_graph(HDC dc, RECT area, const lp_this_pc_state_t *state, bool light,
                                  COLORREF text, COLORREF muted, COLORREF line)
{
    HBRUSH panel_brush = CreateSolidBrush(light ? RGB(244, 246, 248) : RGB(36, 39, 45));
    FillRect(dc, &area, panel_brush);
    DeleteObject(panel_brush);

    const COLORREF grid = light ? RGB(224, 228, 234) : RGB(52, 57, 65);
    HPEN grid_pen = CreatePen(PS_SOLID, 1, grid);
    HPEN old_pen = (HPEN)SelectObject(dc, grid_pen);
    for (int i = 1; i < 4; ++i) {
        const int y = area.top + (area.bottom - area.top) * i / 4;
        MoveToEx(dc, area.left, y, NULL);
        LineTo(dc, area.right, y);
    }
    SelectObject(dc, old_pen);
    DeleteObject(grid_pen);

    uint64_t max_value = 1;
    for (size_t i = 0; i < state->history_count; ++i) {
        if (state->rx_history[i] > max_value) {
            max_value = state->rx_history[i];
        }
        if (state->tx_history[i] > max_value) {
            max_value = state->tx_history[i];
        }
    }
    /* Leaves headroom above the peak so the fill doesn't hug the top edge. */
    max_value = max_value + max_value / 4 + 1;

    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    const int point_count = (int)LP_THIS_PC_GRAPH_POINTS;
    if (state->history_count > 0 && width > 0) {
        POINT fill_points[LP_THIS_PC_GRAPH_POINTS + 2];
        int filled = 0;
        for (int i = 0; i < point_count; ++i) {
            const long long idx = (long long)state->history_count - point_count + i;
            const uint64_t value = idx >= 0 ? state->rx_history[idx] : 0;
            const int x = area.left + width * i / (point_count - 1);
            const int y = area.bottom - (int)((uint64_t)height * value / max_value);
            fill_points[filled].x = x;
            fill_points[filled].y = y;
            ++filled;
        }
        fill_points[filled].x = area.right;
        fill_points[filled].y = area.bottom;
        ++filled;
        fill_points[filled].x = area.left;
        fill_points[filled].y = area.bottom;
        ++filled;

        const COLORREF accent = light ? RGB(24, 140, 210) : RGB(88, 180, 245);
        HBRUSH fill_brush = CreateSolidBrush(accent);
        HPEN fill_pen = CreatePen(PS_SOLID, 2, accent);
        HBRUSH old_brush = (HBRUSH)SelectObject(dc, fill_brush);
        HPEN old_fill_pen = (HPEN)SelectObject(dc, fill_pen);
        Polygon(dc, fill_points, filled);
        SelectObject(dc, old_fill_pen);
        SelectObject(dc, old_brush);
        DeleteObject(fill_pen);
        DeleteObject(fill_brush);

        const COLORREF upload_color = light ? RGB(232, 145, 60) : RGB(232, 170, 96);
        HPEN upload_pen = CreatePen(PS_DASH, 2, upload_color);
        HPEN old_upload_pen = (HPEN)SelectObject(dc, upload_pen);
        bool has_moved = false;
        for (int i = 0; i < point_count; ++i) {
            const long long idx = (long long)state->history_count - point_count + i;
            const uint64_t value = idx >= 0 ? state->tx_history[idx] : 0;
            const int x = area.left + width * i / (point_count - 1);
            const int y = area.bottom - (int)((uint64_t)height * value / max_value);
            if (!has_moved) {
                MoveToEx(dc, x, y, NULL);
                has_moved = true;
            } else {
                LineTo(dc, x, y);
            }
        }
        SelectObject(dc, old_upload_pen);
        DeleteObject(upload_pen);
    }

    HBRUSH old_frame_brush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN frame_pen = CreatePen(PS_SOLID, 1, line);
    HPEN old_frame_pen = (HPEN)SelectObject(dc, frame_pen);
    Rectangle(dc, area.left, area.top, area.right, area.bottom);
    SelectObject(dc, old_frame_pen);
    SelectObject(dc, old_frame_brush);
    DeleteObject(frame_pen);
    (void)text;
    (void)muted;
}

static const char *connection_type_label(lp_connection_type_t type)
{
    switch (type) {
    case LP_CONNECTION_WIFI:
        return "Wi-Fi";
    case LP_CONNECTION_ETHERNET:
        return "Ethernet";
    case LP_CONNECTION_UNKNOWN:
    default:
        return "Unknown";
    }
}

/* Draws one "label ........ value" row and returns the y just below it. */
static int draw_info_row(HDC dc, HFONT label_font, HFONT value_font, COLORREF muted, COLORREF text,
                         const char *label, const char *value, RECT client, int y)
{
    RECT label_rect = {32, y, client.right / 2, y + LP_THIS_PC_INFO_ROW_HEIGHT};
    RECT value_rect = {client.right / 2, y, client.right - 32, y + LP_THIS_PC_INFO_ROW_HEIGHT};
    draw_text(dc, label_font, muted, label, label_rect, DT_LEFT | DT_VCENTER);
    draw_text(dc, value_font, text, value, value_rect, DT_RIGHT | DT_VCENTER);
    return y + LP_THIS_PC_INFO_ROW_HEIGHT;
}

static void paint_this_pc(HWND window, HDC target_dc)
{
    lp_this_pc_state_t *state = state_of(window);
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

    RECT close_rect = {client.right - 42, 8, client.right - 8, 42};
    draw_text(dc, state->label_font, muted, "x", close_rect, DT_CENTER | DT_VCENTER);

    const RECT back = back_rect();
    draw_text(dc, state->label_font, muted, "\x3C  Network", back, DT_LEFT | DT_VCENTER);

    RECT title_rect = {24, 46, client.right - 48, 74};
    draw_text(dc, state->title_font, text, "This PC", title_rect, DT_LEFT | DT_VCENTER);

    char subtitle[LP_THIS_PC_LABEL_MAX];
    const char *hostname = state->hostname[0] != '\0' ? state->hostname : "This PC";
    const char *local_ip = state->networks.local_ip[0] != '\0' ? state->networks.local_ip : "--";
    snprintf(subtitle, sizeof(subtitle), "%s \xB7 %s", hostname, local_ip);
    RECT subtitle_rect = {24, 74, client.right - 24, 96};
    draw_text(dc, state->small_font, muted, subtitle, subtitle_rect, DT_LEFT | DT_VCENTER);

    RECT graph_area = {24, LP_THIS_PC_GRAPH_TOP, client.right - 24,
                       LP_THIS_PC_GRAPH_TOP + LP_THIS_PC_GRAPH_HEIGHT};
    draw_throughput_graph(dc, graph_area, state, light, text, muted, line);

    const uint64_t rx_latest =
        state->history_count > 0 ? state->rx_history[state->history_count - 1] : 0;
    const uint64_t tx_latest =
        state->history_count > 0 ? state->tx_history[state->history_count - 1] : 0;
    char rx_str[32];
    char tx_str[32];
    lp_format_rate(rx_latest, state->use_bits, rx_str, sizeof(rx_str));
    lp_format_rate(tx_latest, state->use_bits, tx_str, sizeof(tx_str));

    const int rates_top = graph_area.bottom + 10;
    RECT receive_label = {32, rates_top, client.right / 2, rates_top + 20};
    RECT receive_value = {32, rates_top + 18, client.right / 2, rates_top + 40};
    draw_text(dc, state->small_font, muted, "Receive", receive_label, DT_LEFT | DT_VCENTER);
    draw_text(dc, state->label_font, text, rx_str, receive_value, DT_LEFT | DT_VCENTER);

    RECT send_label = {client.right / 2, rates_top, client.right - 32, rates_top + 20};
    RECT send_value = {client.right / 2, rates_top + 18, client.right - 32, rates_top + 40};
    draw_text(dc, state->small_font, muted, "Send", send_label, DT_RIGHT | DT_VCENTER);
    draw_text(dc, state->label_font, text, tx_str, send_value, DT_RIGHT | DT_VCENTER);

    const int divider_y = rates_top + 54;
    HPEN divider_pen = CreatePen(PS_SOLID, 1, line);
    HPEN old_divider_pen = (HPEN)SelectObject(dc, divider_pen);
    MoveToEx(dc, 24, divider_y, NULL);
    LineTo(dc, client.right - 24, divider_y);
    SelectObject(dc, old_divider_pen);
    DeleteObject(divider_pen);

    const lp_local_network_list_t *networks = &state->networks;
    const bool is_wifi = networks->local_connection_type == LP_CONNECTION_WIFI;
    int row_y = divider_y + 14;
    row_y =
        draw_info_row(dc, state->label_font, state->label_font, muted, text, "Adapter",
                      networks->local_adapter_name[0] != '\0' ? networks->local_adapter_name : "--",
                      client, row_y);
    row_y = draw_info_row(dc, state->label_font, state->label_font, muted, text, "Connection type",
                          connection_type_label(networks->local_connection_type), client, row_y);
    if (is_wifi) {
        row_y = draw_info_row(dc, state->label_font, state->label_font, muted, text, "SSID",
                              networks->local_has_ssid && networks->local_ssid[0] != '\0'
                                  ? networks->local_ssid
                                  : "--",
                              client, row_y);
        char signal[16];
        if (networks->local_has_signal) {
            snprintf(signal, sizeof(signal), "%d%%", networks->local_signal_percent);
        } else {
            snprintf(signal, sizeof(signal), "--");
        }
        row_y = draw_info_row(dc, state->label_font, state->label_font, muted, text,
                              "Signal strength", signal, client, row_y);
    }
    row_y = draw_info_row(dc, state->label_font, state->label_font, muted, text, "IPv4 address",
                          local_ip, client, row_y);
    row_y =
        draw_info_row(dc, state->label_font, state->label_font, muted, text, "IPv6 address",
                      networks->local_ipv6[0] != '\0' ? networks->local_ipv6 : "--", client, row_y);
    const char *dns_name =
        networks->local_dns_suffix[0] != '\0' ? networks->local_dns_suffix : hostname;
    row_y = draw_info_row(dc, state->label_font, state->label_font, muted, text, "DNS name",
                          dns_name, client, row_y);

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

static LRESULT CALLBACK this_pc_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        lp_this_pc_state_t *state =
            (lp_this_pc_state_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
        if (state == NULL) {
            return -1;
        }
        state->title_font = CreateFontA(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        state->value_font = CreateFontA(30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
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
        paint_this_pc(window, dc);
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
        lp_ui_handle_dpi_changed(window, wparam, LP_THIS_PC_WIDTH, LP_THIS_PC_HEIGHT);
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
            return 0;
        }
        return 0;
    }
    case WM_SETTINGCHANGE: {
        lp_this_pc_state_t *state = state_of(window);
        if (state != NULL) {
            DeleteObject(state->background_brush);
            state->background_brush =
                CreateSolidBrush(is_taskbar_light_theme() ? RGB(250, 251, 252) : RGB(28, 30, 34));
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    }
    case WM_DESTROY: {
        lp_this_pc_state_t *state = state_of(window);
        if (state != NULL) {
            DeleteObject(state->title_font);
            DeleteObject(state->value_font);
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

HWND lp_this_pc_create(HINSTANCE instance, HWND owner)
{
    WNDCLASSEXA window_class;
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = this_pc_wndproc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.lpszClassName = "LinkPulseThisPcWindow";
    if (RegisterClassExA(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return NULL;
    }

    HWND window = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, window_class.lpszClassName,
                                  "LinkPulse this PC", WS_POPUP, 0, 0, LP_THIS_PC_WIDTH,
                                  LP_THIS_PC_HEIGHT, owner, NULL, instance, NULL);
    if (window != NULL) {
        HRGN region = CreateRoundRectRgn(0, 0, LP_THIS_PC_WIDTH + 1, LP_THIS_PC_HEIGHT + 1, 24, 24);
        if (region != NULL && SetWindowRgn(window, region, FALSE) == 0) {
            DeleteObject(region);
        }
    }
    return window;
}

/* Copies the snapshot into the window state; returns false if the window is not ready. */
static bool store_snapshot(HWND window, const char *hostname,
                           const lp_local_network_list_t *networks, const uint64_t *rx_history,
                           const uint64_t *tx_history, size_t history_count, bool use_bits)
{
    if (window == NULL) {
        return false;
    }
    lp_this_pc_state_t *state = state_of(window);
    if (state == NULL) {
        return false;
    }
    snprintf(state->hostname, sizeof(state->hostname), "%s", hostname != NULL ? hostname : "");
    if (networks != NULL) {
        state->networks = *networks;
    } else {
        memset(&state->networks, 0, sizeof(state->networks));
    }
    state->history_count =
        history_count < LP_THIS_PC_GRAPH_POINTS ? history_count : LP_THIS_PC_GRAPH_POINTS;
    if (rx_history != NULL) {
        memcpy(state->rx_history, rx_history, state->history_count * sizeof(state->rx_history[0]));
    }
    if (tx_history != NULL) {
        memcpy(state->tx_history, tx_history, state->history_count * sizeof(state->tx_history[0]));
    }
    state->use_bits = use_bits;
    return true;
}

void lp_this_pc_update(HWND window, const char *hostname, const lp_local_network_list_t *networks,
                       const uint64_t *rx_history, const uint64_t *tx_history, size_t history_count,
                       bool use_bits)
{
    if (store_snapshot(window, hostname, networks, rx_history, tx_history, history_count,
                       use_bits)) {
        InvalidateRect(window, NULL, FALSE);
    }
}

void lp_this_pc_show(HWND window, const char *hostname, const lp_local_network_list_t *networks,
                     const uint64_t *rx_history, const uint64_t *tx_history, size_t history_count,
                     bool use_bits)
{
    if (!store_snapshot(window, hostname, networks, rx_history, tx_history, history_count,
                        use_bits)) {
        return;
    }

    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    UINT dpi = LP_UI_DEFAULT_DPI;
    const RECT rect = lp_ui_popup_rect(monitor, LP_THIS_PC_WIDTH, LP_THIS_PC_HEIGHT, &dpi);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    SetWindowPos(window, HWND_TOPMOST, rect.left, rect.top, width, height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    lp_ui_apply_popup_region(window, width, height, dpi);
    SetForegroundWindow(window);
    InvalidateRect(window, NULL, FALSE);
}
