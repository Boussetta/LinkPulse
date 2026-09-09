#include "network_map_win32.h"

#include <stdio.h>
#include <string.h>

#define LP_MAP_WIDTH 380
#define LP_MAP_HEIGHT 650
#define LP_MAP_MAX_VISIBLE_DEVICES 6

typedef struct {
    lp_neighbor_list_t neighbors;
    lp_local_network_list_t networks;
    HFONT cloud_font;
    HFONT icon_font;
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
        if (state->networks.items[i].gateway_mac[0] != '\0' &&
            strcmp(neighbor->mac, state->networks.items[i].gateway_mac) == 0) {
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

static void display_hostname(const char *hostname, char *out, size_t out_cap)
{
    const char *suffix = ".fritz.box";
    const size_t hostname_length = strlen(hostname);
    const size_t suffix_length = strlen(suffix);
    size_t display_length = hostname_length;
    if (hostname_length > suffix_length &&
        _stricmp(hostname + hostname_length - suffix_length, suffix) == 0) {
        display_length -= suffix_length;
    }
    if (display_length >= out_cap) {
        display_length = out_cap - 1;
    }
    memcpy(out, hostname, display_length);
    out[display_length] = '\0';
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

static void draw_device_node(HDC dc, HFONT label_font, HFONT icon_font, COLORREF fill,
                             COLORREF border, COLORREF text_color, COLORREF muted,
                             const char *label, const lp_neighbor_t *neighbor, int center_x,
                             int center_y)
{
    RECT node = {center_x - 75, center_y - 45, center_x + 75, center_y + 45};
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 2, border);
    HBRUSH old_brush = (HBRUSH)SelectObject(dc, brush);
    HPEN old_pen = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, node.left, node.top, node.right, node.bottom, 12, 12);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    RECT label_rect = {node.left + 8, node.top + 4, node.right - 8, node.top + 23};
    draw_centered_text(dc, label_font, text_color, label, label_rect);
    RECT detail_rect = {node.left + 8, node.top + 22, node.right - 8, node.top + 41};
    draw_centered_text(dc, label_font, text_color, neighbor->ip, detail_rect);

    char identity[LP_VENDOR_MAX + 32];
    const char *type = "Unknown device";
    switch (neighbor->device_type) {
    case LP_DEVICE_LAPTOP:
        type = "Laptop";
        break;
    case LP_DEVICE_MOBILE:
        type = "Mobile phone";
        break;
    case LP_DEVICE_SMARTWATCH:
        type = "Smartwatch";
        break;
    case LP_DEVICE_PRINTER:
        type = "Printer";
        break;
    case LP_DEVICE_TELEVISION:
        type = "Television";
        break;
    case LP_DEVICE_ROUTER:
        type = "Router";
        break;
    case LP_DEVICE_DESKTOP:
        type = "Desktop";
        break;
    default:
        break;
    }
    if (neighbor->vendor[0] != '\0') {
        if (neighbor->device_confidence > 0) {
            snprintf(identity, sizeof(identity), "%s - %s - %u%%", type, neighbor->vendor,
                     (unsigned)neighbor->device_confidence);
        } else {
            snprintf(identity, sizeof(identity), "%s - %s", type, neighbor->vendor);
        }
    } else {
        if (neighbor->device_confidence > 0) {
            snprintf(identity, sizeof(identity), "%s - %u%%", type,
                     (unsigned)neighbor->device_confidence);
        } else {
            snprintf(identity, sizeof(identity), "%s", type);
        }
    }
    RECT identity_rect = {node.left + 8, node.top + 40, node.right - 8, node.top + 59};
    draw_centered_text(dc, label_font, muted, identity, identity_rect);

    const wchar_t *icon = NULL;
    if (neighbor->connection_type == LP_CONNECTION_WIFI) {
        icon = L"\xE701";
    } else if (neighbor->connection_type == LP_CONNECTION_ETHERNET) {
        icon = L"\xE839";
    }
    if (icon != NULL) {
        HFONT old_font = (HFONT)SelectObject(dc, icon_font);
        SetTextColor(dc, muted);
        SetBkMode(dc, TRANSPARENT);
        RECT icon_rect = {center_x - 16, node.top + 59, center_x + 16, node.bottom - 3};
        DrawTextW(dc, icon, 1, &icon_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old_font);
    }
}

static void draw_internet_cloud(HDC dc, HFONT cloud_font, HFONT label_font, COLORREF fill,
                                COLORREF text_color, int center_x)
{
    HFONT old_font = (HFONT)SelectObject(dc, cloud_font);
    SetTextColor(dc, fill);
    SetBkMode(dc, TRANSPARENT);
    RECT cloud_rect = {center_x - 100, -4, center_x + 100, 116};
    DrawTextW(dc, L"\x2601", 1, &cloud_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font);

    RECT label_rect = {center_x - 60, 46, center_x + 60, 78};
    draw_centered_text(dc, label_font, text_color, "Internet", label_rect);
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

    RECT close_rect = {client.right - 42, 8, client.right - 8, 42};
    draw_centered_text(dc, state->label_font, muted, "x", close_rect);

    lp_neighbor_t local_device = {0};
    bool has_local_device = false;
    if (state->networks.local_hostname[0] != '\0' || state->networks.local_ip[0] != '\0') {
        snprintf(local_device.hostname, sizeof(local_device.hostname), "%s",
                 state->networks.local_hostname);
        snprintf(local_device.ip, sizeof(local_device.ip), "%s", state->networks.local_ip);
            local_device.device_type = LP_DEVICE_DESKTOP;
            local_device.device_confidence = 100;
            local_device.connection_type = state->networks.local_connection_type;
            has_local_device = true;
    }
    size_t device_count = has_local_device ? 1 : 0;
    for (size_t i = 0; i < state->neighbors.count; ++i) {
        if (is_device_neighbor(state, &state->neighbors.items[i])) {
            ++device_count;
        }
    }
    const size_t visible_count =
        device_count < LP_MAP_MAX_VISIBLE_DEVICES ? device_count : LP_MAP_MAX_VISIBLE_DEVICES;
    const int center_x = client.right / 2;
    const int cloud_bottom_y = 104;
    const int gateway_y = 165;
    char gateway[LP_IP_STR_MAX] = "No gateway";
    char gateway_label[LP_HOSTNAME_MAX + LP_VENDOR_MAX + 32];
    gateway_label[0] = '\0';
    for (size_t i = 0; i < state->networks.count; ++i) {
        if (state->networks.items[i].gateway[0] != '\0') {
            snprintf(gateway, sizeof(gateway), "%s", state->networks.items[i].gateway);
            if (state->networks.items[i].gateway_hostname[0] != '\0') {
                snprintf(gateway_label, sizeof(gateway_label), "%s",
                         state->networks.items[i].gateway_hostname);
            } else if (state->networks.items[i].gateway_vendor[0] != '\0') {
                snprintf(gateway_label, sizeof(gateway_label), "%s",
                         state->networks.items[i].gateway_vendor);
            }
            break;
        }
    }
    if (gateway_label[0] == '\0') {
        snprintf(gateway_label, sizeof(gateway_label), "Gateway");
    }

    HPEN line_pen = CreatePen(PS_SOLID, 2, line);
    HPEN old_pen = (HPEN)SelectObject(dc, line_pen);
    MoveToEx(dc, center_x, cloud_bottom_y, NULL);
    LineTo(dc, center_x, gateway_y - 31);

    SelectObject(dc, old_pen);
    DeleteObject(line_pen);

    draw_internet_cloud(dc, state->cloud_font, state->label_font, internet_fill, text, center_x);
    draw_node(dc, state->label_font, gateway_fill, line, text, gateway_label, gateway, center_x,
              gateway_y, 170);

    size_t visible_index = 0;
    if (has_local_device && visible_index < visible_count) {
        const int columns = visible_count > 1 ? 2 : 1;
        const int column = (int)visible_index % columns;
        const int row = (int)visible_index / columns;
        const int device_x = columns == 1 ? center_x : 100 + column * 180;
        const int device_y = 285 + row * 112;
        draw_device_node(dc, state->label_font, state->icon_font, device_fill, line, text, muted,
                         "This PC", &local_device, device_x, device_y);
        ++visible_index;
    }
    for (size_t i = 0; i < state->neighbors.count && visible_index < visible_count; ++i) {
        const lp_neighbor_t *neighbor = &state->neighbors.items[i];
        if (!is_device_neighbor(state, neighbor)) {
            continue;
        }
        const int columns = visible_count > 1 ? 2 : 1;
        const int column = (int)visible_index % columns;
        const int row = (int)visible_index / columns;
        const int device_x = columns == 1 ? center_x : 100 + column * 180;
        const int device_y = 285 + row * 112;
        char label[LP_HOSTNAME_MAX];
        if (neighbor->hostname[0] != '\0') {
            display_hostname(neighbor->hostname, label, sizeof(label));
            if (label[0] == '\0') {
                snprintf(label, sizeof(label), "Device %llu",
                         (unsigned long long)visible_index + 1);
            }
        } else {
            snprintf(label, sizeof(label), "Device %llu", (unsigned long long)visible_index + 1);
        }
        draw_device_node(dc, state->label_font, state->icon_font, device_fill, line, text, muted,
                 label, neighbor, device_x, device_y);
        ++visible_index;
    }

    if (visible_count == 0) {
        RECT empty = {40, 285, client.right - 40, 345};
        draw_centered_text(dc, state->label_font, muted, "Waiting for nearby devices...", empty);
    }

    HBRUSH old_brush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN frame_pen = CreatePen(PS_SOLID, 1, line);
    old_pen = (HPEN)SelectObject(dc, frame_pen);
    RoundRect(dc, 0, 0, client.right, client.bottom, 24, 24);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(frame_pen);
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
        state->cloud_font = CreateFontA(112, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI Symbol");
        state->icon_font = CreateFontA(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe MDL2 Assets");
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
            DeleteObject(state->cloud_font);
            DeleteObject(state->icon_font);
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

    HWND window = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, window_class.lpszClassName,
                                  "LinkPulse network", WS_POPUP, 0, 0, LP_MAP_WIDTH,
                                  LP_MAP_HEIGHT, owner, NULL, instance, NULL);
    if (window != NULL) {
        HRGN region = CreateRoundRectRgn(0, 0, LP_MAP_WIDTH + 1, LP_MAP_HEIGHT + 1, 24, 24);
        if (region != NULL && SetWindowRgn(window, region, FALSE) == 0) {
            DeleteObject(region);
        }
    }
    return window;
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
    const int x = monitor_info.rcWork.right - LP_MAP_WIDTH - 12;
    const int y = monitor_info.rcWork.bottom - LP_MAP_HEIGHT - 8;

    SetWindowPos(window, HWND_TOPMOST, x, y, LP_MAP_WIDTH, LP_MAP_HEIGHT,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(window, NULL, TRUE);
    SetForegroundWindow(window);
}