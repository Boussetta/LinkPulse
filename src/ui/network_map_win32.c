#include "network_map_win32.h"

#include <stdio.h>
#include <string.h>

#define LP_MAP_WIDTH 380
#define LP_MAP_HEIGHT 740
#define LP_MAP_MAX_VISIBLE_DEVICES 6
#define LP_MAP_ANIMATION_TIMER 2
#define LP_MAP_ANIMATION_STEP_MS 10
#define LP_MAP_ANIMATION_DURATION_MS 180
#define LP_ISP_DETAIL_LINE_MAX 300
#define LP_ISP_DETAIL_LINES_MAX 3
#define LP_ISP_DETAIL_LINE_HEIGHT 20
#define LP_DEVICE_DETAIL_LINES_MAX 3

typedef struct {
    lp_neighbor_list_t neighbors;
    lp_local_network_list_t networks;
    HFONT cloud_font;
    HFONT icon_font;
    HFONT label_font;
    HBRUSH background_brush;
    int target_x;
    int target_y;
    DWORD animation_started_at;
    bool show_isp_details;
    bool has_selected_device;
    char selected_device_ip[LP_IP_STR_MAX];
} lp_network_map_state_t;

/* Reads the taskbar theme setting used to keep the map consistent with the tray. */
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

/* Removes gateways and non-device multicast entries from the visible map. */
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

/* Draws one clipped, centered ANSI label while restoring the selected font. */
static void draw_centered_text(HDC dc, HFONT font, COLORREF color, const char *text,
                               RECT rect)
{
    HFONT old_font = (HFONT)SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextA(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
}

/* Shortens local DNS names for the fixed-width device node labels. */
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

/* Selects a stable Segoe MDL2 glyph from persisted or inferred device metadata. */
static const wchar_t *device_icon_glyph(const lp_neighbor_t *neighbor)
{
    if (strcmp(neighbor->icon, "phone") == 0 || strcmp(neighbor->icon, "mobile") == 0) {
        return L"\xE8EA";
    }
    if (strcmp(neighbor->icon, "watch") == 0 || strcmp(neighbor->icon, "smartwatch") == 0) {
        return L"\xE91B";
    }
    if (strcmp(neighbor->icon, "printer") == 0) {
        return L"\xE749";
    }
    if (strcmp(neighbor->icon, "tv") == 0 || strcmp(neighbor->icon, "television") == 0) {
        return L"\xE7F4";
    }
    switch (neighbor->device_type) {
    case LP_DEVICE_LAPTOP:
    case LP_DEVICE_DESKTOP:
        return L"\xE770";
    case LP_DEVICE_MOBILE:
        return L"\xE8EA";
    case LP_DEVICE_SMARTWATCH:
        return L"\xE91B";
    case LP_DEVICE_PRINTER:
        return L"\xE749";
    case LP_DEVICE_TELEVISION:
        return L"\xE7F4";
    case LP_DEVICE_ROUTER:
        return L"\xE968";
    default:
        return L"\xE897"; /* "Help" glyph: reads as an unknown-device marker rather than an eye. */
    }
}

/* Draws a gateway-style rounded node with a label and detail line. */
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

/* Draws a device node with IP and paired device/connection icons. */
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

    HFONT old_icon_font = (HFONT)SelectObject(dc, icon_font);
    SetTextColor(dc, muted);
    SetBkMode(dc, TRANSPARENT);
    RECT device_icon_rect = {node.left + 8, node.bottom - 28, node.left + 34, node.bottom - 4};
    DrawTextW(dc, device_icon_glyph(neighbor), 1, &device_icon_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_icon_font);

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
        RECT icon_rect = {node.right - 34, node.bottom - 28, node.right - 8, node.bottom - 4};
        DrawTextW(dc, icon, 1, &icon_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old_font);
    }
}

/* Draws the top-level Internet node, with the ISP name when it is known. */
static void draw_internet_cloud(HDC dc, HFONT cloud_font, HFONT label_font, COLORREF fill,
                                COLORREF text_color, COLORREF muted, int center_x,
                                const char *isp)
{
    HFONT old_font = (HFONT)SelectObject(dc, cloud_font);
    SetTextColor(dc, fill);
    SetBkMode(dc, TRANSPARENT);
    RECT cloud_rect = {center_x - 100, -4, center_x + 100, 116};
    DrawTextW(dc, L"\x2601", 1, &cloud_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font);

    RECT label_rect = {center_x - 90, 40, center_x + 90, 68};
    draw_centered_text(dc, label_font, text_color, "Internet", label_rect);
    if (isp != NULL && isp[0] != '\0') {
        RECT isp_rect = {center_x - 90, 66, center_x + 90, 90};
        draw_centered_text(dc, label_font, muted, isp, isp_rect);
    }
}

/* Builds up to LP_ISP_DETAIL_LINES_MAX concise lines from all fetched ISP fields. */
static size_t format_isp_detail_lines(
    const lp_isp_info_t *isp, char lines[LP_ISP_DETAIL_LINES_MAX][LP_ISP_DETAIL_LINE_MAX])
{
    size_t count = 0;
    if (isp->asn[0] != '\0' && isp->public_ip[0] != '\0') {
        snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "%s \xB7 %s", isp->asn, isp->public_ip);
    } else if (isp->public_ip[0] != '\0') {
        snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "%s", isp->public_ip);
    } else if (isp->asn[0] != '\0') {
        snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "%s", isp->asn);
    }

    if (count < LP_ISP_DETAIL_LINES_MAX) {
        char place[LP_ISP_DETAIL_LINE_MAX] = "";
        if (isp->city[0] != '\0' && isp->region[0] != '\0' && isp->country[0] != '\0') {
            snprintf(place, sizeof(place), "%s, %s, %s", isp->city, isp->region, isp->country);
        } else if (isp->city[0] != '\0' && isp->country[0] != '\0') {
            snprintf(place, sizeof(place), "%s, %s", isp->city, isp->country);
        } else if (isp->city[0] != '\0') {
            snprintf(place, sizeof(place), "%s", isp->city);
        } else if (isp->country[0] != '\0') {
            snprintf(place, sizeof(place), "%s", isp->country);
        }
        if (place[0] != '\0') {
            snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "%s", place);
        }
    }

    if (count < LP_ISP_DETAIL_LINES_MAX) {
        if (isp->postal[0] != '\0' && isp->timezone[0] != '\0') {
            snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "%s \xB7 %s", isp->postal,
                     isp->timezone);
        } else if (isp->timezone[0] != '\0') {
            snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "%s", isp->timezone);
        } else if (isp->postal[0] != '\0') {
            snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "%s", isp->postal);
        }
    }

    if (count == 0) {
        snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "Fetching ISP information...");
    }
    return count;
}

/* Orders "This PC" (when known) ahead of discovered neighbors, capped for the grid. */
static size_t collect_visible_devices(const lp_network_map_state_t *state,
                                      lp_neighbor_t *out_items, bool *out_is_local,
                                      size_t max_count)
{
    size_t count = 0;
    if (count < max_count &&
        (state->networks.local_hostname[0] != '\0' || state->networks.local_ip[0] != '\0')) {
        lp_neighbor_t local_device = {0};
        snprintf(local_device.hostname, sizeof(local_device.hostname), "%s",
                state->networks.local_hostname);
        snprintf(local_device.ip, sizeof(local_device.ip), "%s", state->networks.local_ip);
        local_device.device_type = LP_DEVICE_DESKTOP;
        local_device.device_confidence = 100;
        local_device.connection_type = state->networks.local_connection_type;
        out_items[count] = local_device;
        out_is_local[count] = true;
        ++count;
    }
    for (size_t i = 0; i < state->neighbors.count && count < max_count; ++i) {
        if (is_device_neighbor(state, &state->neighbors.items[i])) {
            out_items[count] = state->neighbors.items[i];
            out_is_local[count] = false;
            ++count;
        }
    }
    return count;
}

/* Computes the same grid position paint_map and click hit-testing must agree on. */
static void device_node_center(size_t visible_index, size_t visible_count, int center_x,
                               int isp_extra_height, int *out_x, int *out_y)
{
    const int columns = visible_count > 1 ? 2 : 1;
    const int column = (int)visible_index % columns;
    const int row = (int)visible_index / columns;
    *out_x = columns == 1 ? center_x : 100 + column * 180;
    *out_y = 285 + row * 112 + isp_extra_height;
}

/* Builds up to LP_DEVICE_DETAIL_LINES_MAX lines describing everything known about a device. */
static size_t format_device_detail_lines(
    const lp_neighbor_t *neighbor, bool is_local,
    char lines[LP_DEVICE_DETAIL_LINES_MAX][LP_ISP_DETAIL_LINE_MAX])
{
    size_t count = 0;
    if (!is_local && neighbor->vendor[0] != '\0') {
        snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "%s", neighbor->vendor);
    }
    if (!is_local && count < LP_DEVICE_DETAIL_LINES_MAX) {
        if (neighbor->mac[0] != '\0') {
            snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "%s", neighbor->mac);
        } else {
            /* Public/isolated Wi-Fi often proxy-ARPs, so the real MAC is never visible. */
            snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX,
                    "MAC hidden by this network");
        }
    }
    if (count < LP_DEVICE_DETAIL_LINES_MAX) {
        const char *connection = neighbor->connection_type == LP_CONNECTION_WIFI ? "Wi-Fi"
                                 : neighbor->connection_type == LP_CONNECTION_ETHERNET
                                       ? "Ethernet"
                                       : "Unknown connection";
        if (neighbor->ip[0] != '\0') {
            snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "%s \xB7 %s", neighbor->ip,
                    connection);
        } else {
            snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "%s", connection);
        }
    }
    if (count == 0) {
        snprintf(lines[count++], LP_ISP_DETAIL_LINE_MAX, "No additional details yet");
    }
    return count;
}

/* Paints a snapshot of gateway and device state using the current theme. */
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

    lp_neighbor_t visible_items[LP_MAP_MAX_VISIBLE_DEVICES];
    bool visible_is_local[LP_MAP_MAX_VISIBLE_DEVICES] = {0};
    size_t device_count = state->networks.local_hostname[0] != '\0' ||
                                          state->networks.local_ip[0] != '\0'
                                      ? 1
                                      : 0;
    for (size_t i = 0; i < state->neighbors.count; ++i) {
        if (is_device_neighbor(state, &state->neighbors.items[i])) {
            ++device_count;
        }
    }
    const size_t visible_count =
        device_count < LP_MAP_MAX_VISIBLE_DEVICES ? device_count : LP_MAP_MAX_VISIBLE_DEVICES;
    const size_t visible_actual =
        collect_visible_devices(state, visible_items, visible_is_local, visible_count);
    const int center_x = client.right / 2;
    char isp_detail_lines[LP_ISP_DETAIL_LINES_MAX][LP_ISP_DETAIL_LINE_MAX];
    size_t isp_detail_line_count = 0;
    int isp_extra_height = 0;
    if (state->show_isp_details) {
        isp_detail_line_count = format_isp_detail_lines(&state->networks.isp_info, isp_detail_lines);
        isp_extra_height = (int)isp_detail_line_count * LP_ISP_DETAIL_LINE_HEIGHT + 8;
    }
    const int cloud_bottom_y = 104;
    const int gateway_y = 165 + isp_extra_height;
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

    draw_internet_cloud(dc, state->cloud_font, state->label_font, internet_fill, text, muted,
                        center_x, state->networks.isp_info.isp);
    for (size_t i = 0; i < isp_detail_line_count; ++i) {
        RECT detail_rect = {center_x - 120, 92 + (int)i * LP_ISP_DETAIL_LINE_HEIGHT,
                            center_x + 120, 92 + (int)(i + 1) * LP_ISP_DETAIL_LINE_HEIGHT};
        draw_centered_text(dc, state->label_font, muted, isp_detail_lines[i], detail_rect);
    }
    draw_node(dc, state->label_font, gateway_fill, line, text, gateway_label, gateway, center_x,
              gateway_y, 170);

    size_t visible_index = 0;
    lp_neighbor_t selected_device = {0};
    bool selected_is_local = false;
    bool has_selected_match = false;
    for (; visible_index < visible_actual; ++visible_index) {
        int device_x, device_y;
        device_node_center(visible_index, visible_actual, center_x, isp_extra_height, &device_x,
                           &device_y);
        const lp_neighbor_t *neighbor = &visible_items[visible_index];
        char label[LP_HOSTNAME_MAX];
        if (visible_is_local[visible_index]) {
            snprintf(label, sizeof(label), "This PC");
        } else if (neighbor->label[0] != '\0') {
            snprintf(label, sizeof(label), "%s", neighbor->label);
        } else if (neighbor->hostname[0] != '\0') {
            display_hostname(neighbor->hostname, label, sizeof(label));
            if (label[0] == '\0') {
                snprintf(label, sizeof(label), "Device %llu", (unsigned long long)visible_index + 1);
            }
        } else if (neighbor->vendor[0] != '\0') {
            snprintf(label, sizeof(label), "%s", neighbor->vendor);
        } else {
            snprintf(label, sizeof(label), "Device %llu", (unsigned long long)visible_index + 1);
        }
        draw_device_node(dc, state->label_font, state->icon_font, device_fill, line, text, muted,
                         label, neighbor, device_x, device_y);
        if (state->has_selected_device && neighbor->ip[0] != '\0' &&
            strcmp(neighbor->ip, state->selected_device_ip) == 0) {
            selected_device = *neighbor;
            selected_is_local = visible_is_local[visible_index];
            has_selected_match = true;
        }
    }

    if (visible_actual == 0) {
        RECT empty = {40, 285 + isp_extra_height, client.right - 40, 345 + isp_extra_height};
        draw_centered_text(dc, state->label_font, muted, "Waiting for nearby devices...", empty);
    }

    if (has_selected_match) {
        char detail_lines[LP_DEVICE_DETAIL_LINES_MAX][LP_ISP_DETAIL_LINE_MAX];
        const size_t detail_line_count =
            format_device_detail_lines(&selected_device, selected_is_local, detail_lines);
        const int panel_top = client.bottom - 130;
        RECT divider = {30, panel_top, client.right - 30, panel_top};
        HPEN divider_pen = CreatePen(PS_SOLID, 1, line);
        HPEN old_divider_pen = (HPEN)SelectObject(dc, divider_pen);
        MoveToEx(dc, divider.left, divider.top, NULL);
        LineTo(dc, divider.right, divider.top);
        SelectObject(dc, old_divider_pen);
        DeleteObject(divider_pen);
        for (size_t i = 0; i < detail_line_count; ++i) {
            RECT detail_rect = {30, panel_top + 12 + (int)i * LP_ISP_DETAIL_LINE_HEIGHT,
                                client.right - 30,
                                panel_top + 12 + (int)(i + 1) * LP_ISP_DETAIL_LINE_HEIGHT};
            draw_centered_text(dc, state->label_font, muted, detail_lines[i], detail_rect);
        }
    }

    HBRUSH old_brush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN frame_pen = CreatePen(PS_SOLID, 1, line);
    old_pen = (HPEN)SelectObject(dc, frame_pen);
    RoundRect(dc, 0, 0, client.right, client.bottom, 24, 24);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(frame_pen);
}

/* Owns map window resources, animation, repaint, theme, and close handling. */
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
            KillTimer(window, LP_MAP_ANIMATION_TIMER);
            ShowWindow(window, SW_HIDE);
        }
        return 0;
    case WM_TIMER:
        if (wparam == LP_MAP_ANIMATION_TIMER) {
            lp_network_map_state_t *state =
                (lp_network_map_state_t *)GetWindowLongPtrA(window, GWLP_USERDATA);
            if (state == NULL) {
                KillTimer(window, LP_MAP_ANIMATION_TIMER);
                return 0;
            }
            const DWORD elapsed = GetTickCount() - state->animation_started_at;
            if (elapsed >= LP_MAP_ANIMATION_DURATION_MS) {
                SetWindowPos(window, HWND_TOPMOST, state->target_x, state->target_y, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE);
                KillTimer(window, LP_MAP_ANIMATION_TIMER);
                return 0;
            }
            const int distance = LP_MAP_WIDTH + 12;
            const int remaining = (int)(LP_MAP_ANIMATION_DURATION_MS - elapsed);
            const int x = state->target_x + distance * remaining / LP_MAP_ANIMATION_DURATION_MS;
            SetWindowPos(window, HWND_TOPMOST, x, state->target_y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
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
            return 0;
        }
        const int center_x = client.right / 2;
        if (x >= center_x - 100 && x <= center_x + 100 && y >= 0 && y <= 100) {
            lp_network_map_state_t *state =
                (lp_network_map_state_t *)GetWindowLongPtrA(window, GWLP_USERDATA);
            if (state != NULL) {
                state->show_isp_details = !state->show_isp_details;
                InvalidateRect(window, NULL, TRUE);
            }
            return 0;
        }
        {
            lp_network_map_state_t *state =
                (lp_network_map_state_t *)GetWindowLongPtrA(window, GWLP_USERDATA);
            if (state == NULL) {
                return 0;
            }
            int isp_extra_height = 0;
            if (state->show_isp_details) {
                char isp_detail_lines[LP_ISP_DETAIL_LINES_MAX][LP_ISP_DETAIL_LINE_MAX];
                const size_t isp_detail_line_count =
                    format_isp_detail_lines(&state->networks.isp_info, isp_detail_lines);
                isp_extra_height = (int)isp_detail_line_count * LP_ISP_DETAIL_LINE_HEIGHT + 8;
            }
            size_t device_count = state->networks.local_hostname[0] != '\0' ||
                                          state->networks.local_ip[0] != '\0'
                                      ? 1
                                      : 0;
            for (size_t i = 0; i < state->neighbors.count; ++i) {
                if (is_device_neighbor(state, &state->neighbors.items[i])) {
                    ++device_count;
                }
            }
            const size_t visible_count =
                device_count < LP_MAP_MAX_VISIBLE_DEVICES ? device_count : LP_MAP_MAX_VISIBLE_DEVICES;
            lp_neighbor_t visible_items[LP_MAP_MAX_VISIBLE_DEVICES];
            bool visible_is_local[LP_MAP_MAX_VISIBLE_DEVICES] = {0};
            const size_t visible_actual =
                collect_visible_devices(state, visible_items, visible_is_local, visible_count);
            for (size_t i = 0; i < visible_actual; ++i) {
                int device_x, device_y;
                device_node_center(i, visible_actual, center_x, isp_extra_height, &device_x,
                                   &device_y);
                if (x >= device_x - 75 && x <= device_x + 75 && y >= device_y - 45 &&
                    y <= device_y + 45) {
                    if (state->has_selected_device &&
                        strcmp(state->selected_device_ip, visible_items[i].ip) == 0) {
                        state->has_selected_device = false;
                    } else {
                        snprintf(state->selected_device_ip, sizeof(state->selected_device_ip), "%s",
                                visible_items[i].ip);
                        state->has_selected_device = true;
                    }
                    InvalidateRect(window, NULL, TRUE);
                    break;
                }
            }
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

/* Registers and creates the popup map window owned by the hidden tray window. */
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

/* Copies the latest discovery snapshot and animates the map beside the cursor monitor. */
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

    state->target_x = x;
    state->target_y = y;
    state->animation_started_at = GetTickCount();
    SetWindowPos(window, HWND_TOPMOST, x + LP_MAP_WIDTH + 12, y, LP_MAP_WIDTH, LP_MAP_HEIGHT,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(window, NULL, TRUE);
    SetForegroundWindow(window);
    SetTimer(window, LP_MAP_ANIMATION_TIMER, LP_MAP_ANIMATION_STEP_MS, NULL);
}