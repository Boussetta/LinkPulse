#include "network_map_win32.h"

#include <stdio.h>
#include <string.h>

#define LP_MAP_WIDTH 380
#define LP_MAP_HEIGHT 440
#define LP_MAP_MAX_VISIBLE_DEVICES 4

typedef struct {
    lp_neighbor_list_t neighbors;
    lp_local_network_list_t networks;
    HFONT title_font;
    HFONT label_font;
    HBRUSH background_brush;
} lp_network_map_state_t;

static bool is_taskbar_light_theme(void)
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    return RegGetValueA(
               HKEY_CURRENT_USER,
               "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               "SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &value, &size) != ERROR_SUCCESS ||
           value != 0;
}

static bool is_device_neighbor(const lp_network_map_state_t *state,
                               const lp_neighbor_t *neighbor)
{
    unsigned ip_first_octet = 0;
    if (neighbor->mac[0] == '\0' ||
        (sscanf(neighbor->ip, "%u.", &ip_first_octet) == 1 && ip_first_octet >= 224 &&
         ip_first_octet <= 239) ||
        strncmp(neighbor->ip, "ff", 2) == 0 || strncmp(neighbor->ip, "FF", 2) == 0) {
        return false;
    }
    for (size_t i = 0; i < state->networks.count; ++i) {
        if (strcmp(neighbor->ip, state->networks.items[i].gateway) == 0) {
            return false;
        }
    }
    return true;
}

static void draw_centered_text(HDC dc, HFONT font, COLORREF color, const char *text,
                               RECT rect)
{
    HFONT old_font = (HFONT)SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextA(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
}

static void draw_node(HDC dc, HFONT font, COLORREF fill, COLORREF border, COLORREF text_color,
                      const char *label, const char *detail, int center_x, int center_y,
                      int width)
{
    RECT node = {center_x - width / 2, center_y - 31, center_x + width / 2, center_y + 31};
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 2, border);
    HBRUSH old_brush = (HBRUSH)SelectObject(dc, brush);
    HPEN old_pen = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, node.left, node.top, node.right, node.bottom, 12, 12);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    RECT label_rect = {node.left + 8, node.top + 8, node.right - 8, node.top + 31};
    draw_centered_text(dc, font, text_color, label, label_rect);
    RECT detail_rect = {node.left + 8, node.top + 30, node.right - 8, node.bottom - 5};
    draw_centered_text(dc, font, text_color, detail, detail_rect);
}

static void paint_map(HWND window, HDC dc)
{
    lp_network_map_state_t *state =
        (lp_network_map_state_t *)GetWindowLongPtrA(window, GWLP_USERDATA);
    if (state == NULL) {
        return;
    }

    RECT client;
    GetClientRect(window, &client);
    FillRect(dc, &client, state->background_brush);

    const bool light = is_taskbar_light_theme();
    const COLORREF text = light ? RGB(30, 35, 42) : RGB(240, 243, 247);
    const COLORREF muted = light ? RGB(90, 99, 112) : RGB(166, 174, 185);
    const COLORREF line = light ? RGB(151, 161, 174) : RGB(85, 97, 112);
    const COLORREF internet_fill = light ? RGB(220, 240, 248) : RGB(23, 65, 78);
    const COLORREF gateway_fill = light ? RGB(221, 238, 224) : RGB(28, 70, 47);
    const COLORREF device_fill = light ? RGB(242, 244, 247) : RGB(44, 48, 55);

    RECT title_rect = {28, 20, client.right - 60, 54};
    HFONT old_font = (HFONT)SelectObject(dc, state->title_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    DrawTextA(dc, "Your network", -1, &title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font);

    RECT close_rect = {client.right - 52, 17, client.right - 16, 53};
    draw_centered_text(dc, state->title_font, muted, "x", close_rect);

    size_t device_count = 0;
    for (size_t i = 0; i < state->neighbors.count; ++i) {
        if (is_device_neighbor(state, &state->neighbors.items[i])) {
            ++device_count;
        }
    }
    const size_t visible_count =
        device_count < LP_MAP_MAX_VISIBLE_DEVICES ? device_count : LP_MAP_MAX_VISIBLE_DEVICES;
    char summary[64];
    if (device_count > visible_count) {
        snprintf(summary, sizeof(summary), "%llu of %llu devices shown",
                 (unsigned long long)visible_count, (unsigned long long)device_count);
    } else {
        snprintf(summary, sizeof(summary), "%llu device%s visible",
                 (unsigned long long)device_count, device_count == 1 ? "" : "s");
    }
    RECT summary_rect = {28, 51, client.right - 28, 78};
    draw_centered_text(dc, state->label_font, muted, summary, summary_rect);

    const int center_x = client.right / 2;
    const int internet_y = 115;
    const int gateway_y = 210;
    char gateway[LP_IP_STR_MAX] = "No gateway";
    for (size_t i = 0; i < state->networks.count; ++i) {
        if (state->networks.items[i].gateway[0] != '\0') {
            snprintf(gateway, sizeof(gateway), "%s", state->networks.items[i].gateway);
            break;
        }
    }

    HPEN line_pen = CreatePen(PS_SOLID, 2, line);
    HPEN old_pen = (HPEN)SelectObject(dc, line_pen);
    MoveToEx(dc, center_x, internet_y + 31, NULL);
    LineTo(dc, center_x, gateway_y - 31);

    size_t visible_index = 0;
    for (size_t i = 0; i < state->neighbors.count && visible_index < visible_count; ++i) {
        if (!is_device_neighbor(state, &state->neighbors.items[i])) {
            continue;
        }
        const int columns = visible_count > 1 ? 2 : 1;
        const int column = (int)visible_index % columns;
        const int row = (int)visible_index / columns;
        const int device_x = columns == 1 ? center_x : 100 + column * 180;
        const int device_y = 310 + row * 80;
        MoveToEx(dc, center_x, gateway_y + 31, NULL);
        LineTo(dc, device_x, device_y - 31);
        ++visible_index;
    }
    SelectObject(dc, old_pen);
    DeleteObject(line_pen);

    draw_node(dc, state->label_font, internet_fill, line, text, "Internet", "WAN", center_x,
              internet_y, 150);
    draw_node(dc, state->label_font, gateway_fill, line, text, "Gateway", gateway, center_x,
              gateway_y, 170);

    visible_index = 0;
    for (size_t i = 0; i < state->neighbors.count && visible_index < visible_count; ++i) {
        const lp_neighbor_t *neighbor = &state->neighbors.items[i];
        if (!is_device_neighbor(state, neighbor)) {
            continue;
        }
        const int columns = visible_count > 1 ? 2 : 1;
        const int column = (int)visible_index % columns;
        const int row = (int)visible_index / columns;
        const int device_x = columns == 1 ? center_x : 100 + column * 180;
        const int device_y = 310 + row * 80;
        char label[LP_HOSTNAME_MAX];
        if (neighbor->hostname[0] != '\0') {
            snprintf(label, sizeof(label), "%s", neighbor->hostname);
        } else {
            snprintf(label, sizeof(label), "Device %llu", (unsigned long long)visible_index + 1);
        }
        draw_node(dc, state->label_font, device_fill, line, text, label, neighbor->ip, device_x,
                device_y, 150);
        ++visible_index;
    }

    if (visible_count == 0) {
        RECT empty = {40, 320, client.right - 40, 380};
        draw_centered_text(dc, state->label_font, muted, "Waiting for nearby devices...", empty);
    }
}

static LRESULT CALLBACK network_map_wndproc(HWND window, UINT message, WPARAM wparam,
                                            LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        lp_network_map_state_t *state =
            (lp_network_map_state_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
        if (state == NULL) {
            return -1;
        }
        state->title_font = CreateFontA(24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        state->label_font = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        const COLORREF background =
            is_taskbar_light_theme() ? RGB(250, 251, 252) : RGB(28, 30, 34);
        state->background_brush = CreateSolidBrush(background);
        SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)state);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        paint_map(window, dc);
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
            ShowWindow(window, SW_HIDE);
        }
        return 0;
    case WM_LBUTTONUP: {
        const int x = (short)LOWORD(lparam);
        const int y = (short)HIWORD(lparam);
        RECT client;
        GetClientRect(window, &client);
        if (x >= client.right - 58 && y <= 58) {
            ShowWindow(window, SW_HIDE);
        }
        return 0;
    }
    case WM_SETTINGCHANGE: {
        lp_network_map_state_t *state =
            (lp_network_map_state_t *)GetWindowLongPtrA(window, GWLP_USERDATA);
        if (state != NULL) {
            DeleteObject(state->background_brush);
            state->background_brush = CreateSolidBrush(
                is_taskbar_light_theme() ? RGB(250, 251, 252) : RGB(28, 30, 34));
            InvalidateRect(window, NULL, TRUE);
        }
        return 0;
    }
    case WM_DESTROY: {
        lp_network_map_state_t *state =
            (lp_network_map_state_t *)GetWindowLongPtrA(window, GWLP_USERDATA);
        if (state != NULL) {
            DeleteObject(state->title_font);
            DeleteObject(state->label_font);
            DeleteObject(state->background_brush);
            HeapFree(GetProcessHeap(), 0, state);
        }
        return 0;
    }
    default:
        return DefWindowProcA(window, message, wparam, lparam);
    }
}

HWND lp_network_map_create(HINSTANCE instance, HWND owner)
{
    WNDCLASSEXA window_class;
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = network_map_wndproc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.lpszClassName = "LinkPulseNetworkMapWindow";
    if (RegisterClassExA(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return NULL;
    }

    return CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, window_class.lpszClassName,
                           "LinkPulse network", WS_POPUP | WS_BORDER, 0, 0, LP_MAP_WIDTH,
                           LP_MAP_HEIGHT, owner, NULL, instance, NULL);
}

void lp_network_map_show(HWND window, const lp_neighbor_list_t *neighbors,
                         const lp_local_network_list_t *networks)
{
    if (window == NULL || neighbors == NULL || networks == NULL) {
        return;
    }
    lp_network_map_state_t *state =
        (lp_network_map_state_t *)GetWindowLongPtrA(window, GWLP_USERDATA);
    if (state == NULL) {
        return;
    }
    state->neighbors = *neighbors;
    state->networks = *networks;

    if (IsWindowVisible(window)) {
        InvalidateRect(window, NULL, TRUE);
        return;
    }

    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info;
    memset(&monitor_info, 0, sizeof(monitor_info));
    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoA(monitor, &monitor_info);
    int x = cursor.x - LP_MAP_WIDTH - 72;
    int y = monitor_info.rcWork.bottom - LP_MAP_HEIGHT - 8;
    if (x < monitor_info.rcWork.left + 8) {
        x = monitor_info.rcWork.left + 8;
    }
    if (x + LP_MAP_WIDTH > monitor_info.rcWork.right - 8) {
        x = monitor_info.rcWork.right - LP_MAP_WIDTH - 8;
    }

    SetWindowPos(window, HWND_TOPMOST, x, y, LP_MAP_WIDTH, LP_MAP_HEIGHT,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(window, NULL, TRUE);
    SetForegroundWindow(window);
}