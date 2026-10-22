#include "device_details_win32.h"

#include <stdio.h>
#include <string.h>

#define LP_DEVICE_DETAILS_WIDTH 380
#define LP_DEVICE_DETAILS_HEIGHT 740
#define LP_DEVICE_DETAILS_ROW_HEIGHT 32

typedef struct {
    HFONT title_font;
    HFONT label_font;
    HFONT small_font;
    HBRUSH background_brush;
    lp_neighbor_t neighbor;
    lp_device_capture_state_t capture_state;
    lp_capture_activity_t capture;
} lp_device_details_state_t;

static lp_device_details_state_t *state_of(HWND window)
{
    return (lp_device_details_state_t *)GetWindowLongPtrA(window, GWLP_USERDATA);
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
    COPYDATASTRUCT copy_data = {LP_DEVICE_DETAILS_COPYDATA_BACK, 1, (PVOID) ""};
    (void)SendMessageA(owner, WM_COPYDATA, (WPARAM)window, (LPARAM)&copy_data);
}

static const char *device_type_label(lp_device_type_t type)
{
    switch (type) {
    case LP_DEVICE_LAPTOP:
        return "Laptop";
    case LP_DEVICE_MOBILE:
        return "Mobile";
    case LP_DEVICE_SMARTWATCH:
        return "Smartwatch";
    case LP_DEVICE_PRINTER:
        return "Printer";
    case LP_DEVICE_TELEVISION:
        return "Television";
    case LP_DEVICE_ROUTER:
        return "Router";
    case LP_DEVICE_DESKTOP:
        return "Desktop";
    default:
        return "Unknown";
    }
}

static const char *connection_type_label(lp_connection_type_t type)
{
    switch (type) {
    case LP_CONNECTION_WIFI:
        return "Wi-Fi";
    case LP_CONNECTION_ETHERNET:
        return "Ethernet";
    default:
        return "Unknown";
    }
}

static int draw_info_row(HDC dc, HFONT font, COLORREF muted, COLORREF text, const char *label,
                         const char *value, RECT client, int y)
{
    RECT label_rect = {32, y, client.right / 2, y + LP_DEVICE_DETAILS_ROW_HEIGHT};
    RECT value_rect = {client.right / 2, y, client.right - 32, y + LP_DEVICE_DETAILS_ROW_HEIGHT};
    draw_text(dc, font, muted, label, label_rect, DT_LEFT | DT_VCENTER);
    draw_text(dc, font, text, value, value_rect, DT_RIGHT | DT_VCENTER);
    return y + LP_DEVICE_DETAILS_ROW_HEIGHT;
}

static void paint_device_details(HWND window, HDC target_dc)
{
    lp_device_details_state_t *state = state_of(window);
    if (state == NULL)
        return;

    RECT client;
    GetClientRect(window, &client);
    HDC dc = CreateCompatibleDC(target_dc);
    HBITMAP bitmap = CreateCompatibleBitmap(target_dc, client.right, client.bottom);
    if (dc == NULL || bitmap == NULL) {
        if (bitmap != NULL)
            DeleteObject(bitmap);
        if (dc != NULL)
            DeleteDC(dc);
        return;
    }
    HBITMAP old_bitmap = (HBITMAP)SelectObject(dc, bitmap);
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

    const lp_neighbor_t *neighbor = &state->neighbor;
    const char *name = neighbor->label[0] != '\0'      ? neighbor->label
                       : neighbor->hostname[0] != '\0' ? neighbor->hostname
                                                       : "Device";
    RECT title_rect = {24, 52, client.right - 24, 82};
    draw_text(dc, state->title_font, text, name, title_rect, DT_LEFT | DT_VCENTER);
    RECT subtitle_rect = {24, 82, client.right - 24, 106};
    draw_text(dc, state->small_font, muted, device_type_label(neighbor->device_type), subtitle_rect,
              DT_LEFT | DT_VCENTER);

    int row_y = 128;
    row_y = draw_info_row(dc, state->label_font, muted, text, "Status",
                          neighbor->active ? "Online" : "Offline", client, row_y);
    row_y = draw_info_row(dc, state->label_font, muted, text, "Trust",
                          neighbor->trusted ? "Trusted" : "Not trusted", client, row_y);
    row_y = draw_info_row(dc, state->label_font, muted, text, "Connection",
                          connection_type_label(neighbor->connection_type), client, row_y);
    row_y = draw_info_row(dc, state->label_font, muted, text, "IPv4 address",
                          neighbor->ip[0] != '\0' ? neighbor->ip : "--", client, row_y);
    row_y = draw_info_row(dc, state->label_font, muted, text, "MAC address",
                          neighbor->mac[0] != '\0' ? neighbor->mac : "--", client, row_y);
    row_y = draw_info_row(dc, state->label_font, muted, text, "Vendor",
                          neighbor->vendor[0] != '\0' ? neighbor->vendor : "--", client, row_y);

    const int divider_y = row_y + 10;
    HPEN divider_pen = CreatePen(PS_SOLID, 1, line);
    HPEN old_divider_pen = (HPEN)SelectObject(dc, divider_pen);
    MoveToEx(dc, 24, divider_y, NULL);
    LineTo(dc, client.right - 24, divider_y);
    SelectObject(dc, old_divider_pen);
    DeleteObject(divider_pen);

    RECT activity_title = {24, divider_y + 22, client.right - 24, divider_y + 52};
    draw_text(dc, state->title_font, text, "Activity", activity_title, DT_LEFT | DT_VCENTER);
    RECT activity_panel = {24, divider_y + 64, client.right - 24, divider_y + 190};
    HBRUSH panel_brush = CreateSolidBrush(panel);
    FillRect(dc, &activity_panel, panel_brush);
    DeleteObject(panel_brush);
    RECT activity_status_rect = {40, activity_panel.top + 18, activity_panel.right - 16,
                                 activity_panel.top + 44};
    const char *activity_status_text;
    if (state->capture_state == LP_DEVICE_CAPTURE_OFF) {
        activity_status_text = "Packet capture is off";
    } else if (state->capture_state == LP_DEVICE_CAPTURE_RUNNING) {
        activity_status_text = "Capturing...";
    } else if (state->capture.restricted_to_admins) {
        activity_status_text = "Npcap needs administrator rights";
    } else if (!state->capture.available) {
        activity_status_text = "Packet capture unavailable";
    } else if (state->capture.device_visible) {
        activity_status_text = "Visible device traffic";
    } else {
        activity_status_text = "No peer traffic visible";
    }
    draw_text(dc, state->label_font, text, activity_status_text, activity_status_rect,
              DT_LEFT | DT_VCENTER);
    RECT activity_detail = {40, activity_panel.top + 48, activity_panel.right - 24,
                            activity_panel.bottom - 12};
    HFONT old_font = (HFONT)SelectObject(dc, state->small_font);
    SetTextColor(dc, muted);
    SetBkMode(dc, TRANSPARENT);
    char capture_detail[256];
    if (state->capture_state == LP_DEVICE_CAPTURE_OFF) {
        snprintf(capture_detail, sizeof(capture_detail),
                 "Turn on \"Capture device traffic\" in the tray menu to sample this device "
                 "with Npcap. Only packet headers are read; payloads are never stored.");
    } else if (state->capture_state == LP_DEVICE_CAPTURE_RUNNING) {
        snprintf(capture_detail, sizeof(capture_detail),
                 "Sampling the selected device's traffic for a few seconds.");
    } else if (state->capture.restricted_to_admins) {
        snprintf(capture_detail, sizeof(capture_detail),
                 "Npcap is restricted to administrators. Run LinkPulse as administrator, or "
                 "reinstall Npcap without \"Restrict Npcap driver's access to Administrators "
                 "only\".");
    } else if (state->capture.device_visible) {
        snprintf(capture_detail, sizeof(capture_detail),
                 "Captured %u packets: %llu B received, "
                 "%llu B sent.",
                 state->capture.packet_count, (unsigned long long)state->capture.rx_bytes,
                 (unsigned long long)state->capture.tx_bytes);
    } else if (state->capture.detail[0] != '\0') {
        snprintf(capture_detail, sizeof(capture_detail),
                 "%s. This Wi-Fi adapter is not exposing "
                 "the selected device's traffic.",
                 state->capture.detail);
    } else {
        snprintf(capture_detail, sizeof(capture_detail),
                 "Waiting for the selected-device "
                 "capture sample.");
    }
    DrawTextA(dc, capture_detail, -1, &activity_detail, DT_LEFT | DT_WORDBREAK);
    SelectObject(dc, old_font);

    HBRUSH old_brush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN frame_pen = CreatePen(PS_SOLID, 1, line);
    HPEN old_pen = (HPEN)SelectObject(dc, frame_pen);
    RoundRect(dc, 0, 0, client.right, client.bottom, 24, 24);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(frame_pen);

    BitBlt(target_dc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

static LRESULT CALLBACK device_details_wndproc(HWND window, UINT message, WPARAM wparam,
                                               LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        lp_device_details_state_t *state = (lp_device_details_state_t *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
        if (state == NULL)
            return -1;
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
        paint_device_details(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE)
            ShowWindow(window, SW_HIDE);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
            go_back(window);
        return 0;
    case WM_LBUTTONUP: {
        const int x = (short)LOWORD(lparam);
        const int y = (short)HIWORD(lparam);
        RECT client;
        GetClientRect(window, &client);
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
        lp_device_details_state_t *state = state_of(window);
        if (state != NULL) {
            DeleteObject(state->background_brush);
            state->background_brush =
                CreateSolidBrush(is_taskbar_light_theme() ? RGB(250, 251, 252) : RGB(28, 30, 34));
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    }
    case WM_DESTROY: {
        lp_device_details_state_t *state = state_of(window);
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

HWND lp_device_details_create(HINSTANCE instance, HWND owner)
{
    WNDCLASSEXA window_class = {0};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = device_details_wndproc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.lpszClassName = "LinkPulseDeviceDetailsWindow";
    if (RegisterClassExA(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return NULL;
    }
    HWND window = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, window_class.lpszClassName,
                                  "LinkPulse device", WS_POPUP, 0, 0, LP_DEVICE_DETAILS_WIDTH,
                                  LP_DEVICE_DETAILS_HEIGHT, owner, NULL, instance, NULL);
    if (window != NULL) {
        HRGN region = CreateRoundRectRgn(0, 0, LP_DEVICE_DETAILS_WIDTH + 1,
                                         LP_DEVICE_DETAILS_HEIGHT + 1, 24, 24);
        if (region != NULL && SetWindowRgn(window, region, FALSE) == 0)
            DeleteObject(region);
    }
    return window;
}

/* Copies the device snapshot into the window state; returns false if the window is not ready. */
static bool store_snapshot(HWND window, const lp_neighbor_t *neighbor,
                           lp_device_capture_state_t capture_state,
                           const lp_capture_activity_t *capture)
{
    if (window == NULL || neighbor == NULL)
        return false;
    lp_device_details_state_t *state = state_of(window);
    if (state == NULL)
        return false;
    state->neighbor = *neighbor;
    state->capture_state = capture_state;
    if (capture_state == LP_DEVICE_CAPTURE_DONE && capture != NULL) {
        state->capture = *capture;
    } else {
        memset(&state->capture, 0, sizeof(state->capture));
    }
    return true;
}

void lp_device_details_update(HWND window, const lp_neighbor_t *neighbor,
                              lp_device_capture_state_t capture_state,
                              const lp_capture_activity_t *capture)
{
    if (store_snapshot(window, neighbor, capture_state, capture))
        InvalidateRect(window, NULL, FALSE);
}

void lp_device_details_show(HWND window, const lp_neighbor_t *neighbor,
                            lp_device_capture_state_t capture_state,
                            const lp_capture_activity_t *capture)
{
    if (!store_snapshot(window, neighbor, capture_state, capture))
        return;

    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = {0};
    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoA(monitor, &monitor_info);
    const int x = monitor_info.rcWork.right - LP_DEVICE_DETAILS_WIDTH - 12;
    const int y = monitor_info.rcWork.bottom - LP_DEVICE_DETAILS_HEIGHT - 8;
    SetWindowPos(window, HWND_TOPMOST, x, y, LP_DEVICE_DETAILS_WIDTH, LP_DEVICE_DETAILS_HEIGHT,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetForegroundWindow(window);
    InvalidateRect(window, NULL, FALSE);
}