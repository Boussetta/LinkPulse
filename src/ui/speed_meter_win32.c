#include "speed_meter_win32.h"

#include "linkpulse/discovery.h"
#include "linkpulse/format.h"
#include "linkpulse/isp.h"
#include "linkpulse/log.h"
#include "linkpulse/speedtest.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LP_SPEED_WIDTH 380
#define LP_SPEED_HEIGHT 740
#define LP_SPEED_GAUGE_CENTER_Y 206
#define LP_SPEED_GAUGE_RADIUS 92
#define LP_SPEED_GAUGE_THICKNESS 14
#define LP_SPEED_GAUGE_START_DEG 225.0f
#define LP_SPEED_GAUGE_SWEEP_DEG (-270.0f)
/* Log scale covering 0.1 Mb/s to 1 Gb/s, i.e. four decades of arc. */
#define LP_SPEED_GAUGE_MIN_MBPS 0.1
#define LP_SPEED_GAUGE_DECADES 4.0
#define LP_SPEED_ROW_HEIGHT 32
#define LP_SPEED_BUTTON_TOP 424
#define LP_SPEED_QUALITY_TOP 486
#define LP_SPEED_LABEL_MAX (LP_HOSTNAME_MAX + LP_VENDOR_MAX + 32)

#define WM_LP_SPEED_PROGRESS (WM_APP + 10)

typedef struct {
    CRITICAL_SECTION lock;
    lp_speedtest_progress_t progress;
    volatile long cancel;
    volatile LONG running;
    HANDLE thread;

    HFONT title_font;
    HFONT value_font;
    HFONT label_font;
    HFONT small_font;
    HBRUSH background_brush;

    char router_label[LP_SPEED_LABEL_MAX];
    char router_ip[LP_IP_STR_MAX];
    char isp[LP_ISP_NAME_MAX];
    bool use_bits;
} lp_speed_meter_state_t;

static lp_speed_meter_state_t *state_of(HWND window)
{
    return (lp_speed_meter_state_t *)GetWindowLongPtrA(window, GWLP_USERDATA);
}

static void stop_test(lp_speed_meter_state_t *state);

/* Asks the worker to abort; it may take until the current chunk to notice. */
static void cancel_test(lp_speed_meter_state_t *state)
{
    state->cancel = 1;
}

/* Reads the taskbar theme setting so the meter matches the map and tray icon. */
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

static void draw_text(HDC dc, HFONT font, COLORREF color, const char *text, RECT rect, UINT flags)
{
    HFONT old_font = (HFONT)SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextA(dc, text, -1, &rect, flags | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
}

static void draw_wrapped_text(HDC dc, HFONT font, COLORREF color, const char *text, RECT rect)
{
    HFONT old_font = (HFONT)SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextA(dc, text, -1, &rect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    SelectObject(dc, old_font);
}

/* Green through red, so the grade reads at a glance without the label. */
static COLORREF quality_color(lp_speedtest_quality_t quality, bool light)
{
    switch (quality) {
    case LP_SPEEDTEST_QUALITY_EXCELLENT:
        return light ? RGB(24, 140, 88) : RGB(76, 196, 140);
    case LP_SPEEDTEST_QUALITY_GOOD:
        return light ? RGB(84, 152, 60) : RGB(146, 200, 96);
    case LP_SPEEDTEST_QUALITY_FAIR:
        return light ? RGB(196, 132, 24) : RGB(232, 176, 72);
    case LP_SPEEDTEST_QUALITY_POOR:
        return light ? RGB(196, 60, 60) : RGB(232, 108, 108);
    case LP_SPEEDTEST_QUALITY_UNKNOWN:
    default:
        return light ? RGB(90, 99, 112) : RGB(166, 174, 185);
    }
}

/* Maps a byte rate onto the gauge's logarithmic 0..1 sweep fraction. */
static double gauge_fraction(uint64_t bytes_per_sec)
{
    const double mbps = (double)bytes_per_sec * 8.0 / 1000000.0;
    if (mbps <= LP_SPEED_GAUGE_MIN_MBPS) {
        return 0.0;
    }
    const double fraction = log10(mbps / LP_SPEED_GAUGE_MIN_MBPS) / LP_SPEED_GAUGE_DECADES;
    return fraction > 1.0 ? 1.0 : fraction;
}

static void draw_arc(HDC dc, int center_x, int center_y, int radius, int thickness,
                     COLORREF color, float start_deg, float sweep_deg)
{
    if (sweep_deg == 0.0f) {
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, thickness, color);
    HPEN old_pen = (HPEN)SelectObject(dc, pen);
    const double radians = (double)start_deg * 3.14159265358979323846 / 180.0;
    /* AngleArc draws a line from the current position to the arc's start. */
    MoveToEx(dc, center_x + (int)((double)radius * cos(radians)),
             center_y - (int)((double)radius * sin(radians)), NULL);
    AngleArc(dc, center_x, center_y, (DWORD)radius, start_deg, sweep_deg);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

/* Draws the decade tick marks that make the log scale readable. */
static void draw_gauge_ticks(HDC dc, int center_x, int center_y, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN old_pen = (HPEN)SelectObject(dc, pen);
    for (int decade = 0; decade <= (int)LP_SPEED_GAUGE_DECADES; ++decade) {
        const double fraction = (double)decade / LP_SPEED_GAUGE_DECADES;
        const double degrees =
            (double)LP_SPEED_GAUGE_START_DEG + (double)LP_SPEED_GAUGE_SWEEP_DEG * fraction;
        const double radians = degrees * 3.14159265358979323846 / 180.0;
        const int inner = LP_SPEED_GAUGE_RADIUS - LP_SPEED_GAUGE_THICKNESS / 2 - 4;
        const int outer = LP_SPEED_GAUGE_RADIUS - LP_SPEED_GAUGE_THICKNESS / 2 - 12;
        MoveToEx(dc, center_x + (int)((double)inner * cos(radians)),
                 center_y - (int)((double)inner * sin(radians)), NULL);
        LineTo(dc, center_x + (int)((double)outer * cos(radians)),
               center_y - (int)((double)outer * sin(radians)));
    }
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

static void format_millis(uint64_t microseconds, bool known, char *out, size_t out_cap)
{
    if (!known) {
        snprintf(out, out_cap, "--");
        return;
    }
    snprintf(out, out_cap, "%.1f ms", (double)microseconds / 1000.0);
}

static void format_rate_or_dash(uint64_t bytes_per_sec, bool known, bool use_bits, char *out,
                                size_t out_cap)
{
    if (!known) {
        snprintf(out, out_cap, "--");
        return;
    }
    lp_format_rate(bytes_per_sec, use_bits, out, out_cap);
}

/* Describes what the run is doing right now, under the gauge. */
static const char *phase_caption(const lp_speedtest_progress_t *progress, bool running)
{
    switch (progress->phase) {
    case LP_SPEEDTEST_PHASE_LATENCY:
        return "Measuring latency...";
    case LP_SPEEDTEST_PHASE_DOWNLOAD:
        return "Testing download";
    case LP_SPEEDTEST_PHASE_UPLOAD:
        return "Testing upload";
    case LP_SPEEDTEST_PHASE_DONE:
        return "Test complete";
    case LP_SPEEDTEST_PHASE_CANCELLED:
        return "Test cancelled";
    case LP_SPEEDTEST_PHASE_FAILED:
        return "Test failed \xB7 check your connection";
    case LP_SPEEDTEST_PHASE_IDLE:
    default:
        return running ? "Connecting..." : "Ready to test";
    }
}

static RECT button_rect(const RECT *client)
{
    const int center_x = client->right / 2;
    /* Anchored to the content above, not the frame: the panel matches the map's
       height and the space below is reserved for further detail. */
    RECT rect = {center_x - 80, LP_SPEED_BUTTON_TOP, center_x + 80, LP_SPEED_BUTTON_TOP + 36};
    return rect;
}

static RECT back_rect(void)
{
    RECT rect = {14, 10, 108, 44};
    return rect;
}

/* Hides the meter and asks the owner to bring the network map back up. */
static void go_back(HWND window, lp_speed_meter_state_t *state)
{
    cancel_test(state);
    HWND owner = GetWindow(window, GW_OWNER);
    if (owner == NULL) {
        ShowWindow(window, SW_HIDE);
        return;
    }
    /* The owner hides this window once the map has taken the foreground. */
    COPYDATASTRUCT copy_data;
    memset(&copy_data, 0, sizeof(copy_data));
    copy_data.dwData = LP_SPEED_METER_COPYDATA_BACK;
    copy_data.cbData = (DWORD)strlen(state->router_ip) + 1;
    copy_data.lpData = (PVOID)state->router_ip;
    (void)SendMessageA(owner, WM_COPYDATA, (WPARAM)window, (LPARAM)&copy_data);
}

static void paint_meter(HWND window, HDC target_dc)
{
    lp_speed_meter_state_t *state = state_of(window);
    if (state == NULL) {
        return;
    }

    RECT client;
    GetClientRect(window, &client);

    /* Double-buffered: the gauge repaints ~10x/s while a test runs. */
    HDC dc = CreateCompatibleDC(target_dc);
    HBITMAP bitmap = CreateCompatibleBitmap(target_dc, client.right, client.bottom);
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
    FillRect(dc, &client, state->background_brush);

    lp_speedtest_progress_t progress;
    EnterCriticalSection(&state->lock);
    progress = state->progress;
    LeaveCriticalSection(&state->lock);
    const bool running = InterlockedCompareExchange(&state->running, 0, 0) != 0;

    const bool light = is_taskbar_light_theme();
    const COLORREF text = light ? RGB(30, 35, 42) : RGB(240, 243, 247);
    const COLORREF muted = light ? RGB(90, 99, 112) : RGB(166, 174, 185);
    const COLORREF line = light ? RGB(151, 161, 174) : RGB(85, 97, 112);
    const COLORREF track = light ? RGB(224, 228, 234) : RGB(52, 57, 65);
    const COLORREF accent = progress.phase == LP_SPEEDTEST_PHASE_UPLOAD
                                ? RGB(232, 145, 60)
                                : (light ? RGB(24, 140, 210) : RGB(88, 180, 245));
    const COLORREF button_fill = light ? RGB(236, 240, 245) : RGB(44, 48, 55);

    RECT close_rect = {client.right - 42, 8, client.right - 8, 42};
    draw_text(dc, state->label_font, muted, "x", close_rect, DT_CENTER | DT_VCENTER);

    const RECT back = back_rect();
    draw_text(dc, state->label_font, muted, "\x3C  Network", back, DT_LEFT | DT_VCENTER);

    RECT title_rect = {24, 46, client.right - 48, 74};
    draw_text(dc, state->title_font, text, "Internet speed", title_rect, DT_LEFT | DT_VCENTER);

    char subtitle[LP_SPEED_LABEL_MAX + LP_IP_STR_MAX + LP_ISP_NAME_MAX + 8];
    if (state->isp[0] != '\0') {
        snprintf(subtitle, sizeof(subtitle), "%s \xB7 %s \xB7 %s", state->router_label,
                 state->router_ip, state->isp);
    } else {
        snprintf(subtitle, sizeof(subtitle), "%s \xB7 %s", state->router_label, state->router_ip);
    }
    RECT subtitle_rect = {24, 74, client.right - 24, 96};
    draw_text(dc, state->small_font, muted, subtitle, subtitle_rect, DT_LEFT | DT_VCENTER);

    const int center_x = client.right / 2;
    draw_arc(dc, center_x, LP_SPEED_GAUGE_CENTER_Y, LP_SPEED_GAUGE_RADIUS,
             LP_SPEED_GAUGE_THICKNESS, track, LP_SPEED_GAUGE_START_DEG, LP_SPEED_GAUGE_SWEEP_DEG);
    draw_gauge_ticks(dc, center_x, LP_SPEED_GAUGE_CENTER_Y, line);
    const double fraction = gauge_fraction(progress.live_bytes_per_sec);
    draw_arc(dc, center_x, LP_SPEED_GAUGE_CENTER_Y, LP_SPEED_GAUGE_RADIUS,
             LP_SPEED_GAUGE_THICKNESS, accent, LP_SPEED_GAUGE_START_DEG,
             (float)((double)LP_SPEED_GAUGE_SWEEP_DEG * fraction));

    char value[32];
    format_rate_or_dash(progress.live_bytes_per_sec, progress.live_bytes_per_sec > 0,
                        state->use_bits, value, sizeof(value));
    RECT value_rect = {center_x - 86, LP_SPEED_GAUGE_CENTER_Y - 30, center_x + 86,
                       LP_SPEED_GAUGE_CENTER_Y + 12};
    draw_text(dc, state->value_font, text, value, value_rect, DT_CENTER | DT_VCENTER);

    RECT phase_rect = {center_x - 110, LP_SPEED_GAUGE_CENTER_Y + 16, center_x + 110,
                       LP_SPEED_GAUGE_CENTER_Y + 40};
    draw_text(dc, state->small_font, muted, phase_caption(&progress, running), phase_rect,
              DT_CENTER | DT_VCENTER);

    const int bar_top = LP_SPEED_GAUGE_CENTER_Y + 74;
    RECT bar = {40, bar_top, client.right - 40, bar_top + 6};
    HBRUSH track_brush = CreateSolidBrush(track);
    FillRect(dc, &bar, track_brush);
    DeleteObject(track_brush);
    if (running) {
        const unsigned percent = progress.phase_percent > 100 ? 100 : progress.phase_percent;
        RECT filled = bar;
        filled.right = bar.left + (bar.right - bar.left) * (int)percent / 100;
        HBRUSH fill_brush = CreateSolidBrush(accent);
        FillRect(dc, &filled, fill_brush);
        DeleteObject(fill_brush);
    }

    char download[32];
    char upload[32];
    char ping[32];
    char jitter[32];
    format_rate_or_dash(progress.download_bytes_per_sec, progress.has_download, state->use_bits,
                        download, sizeof(download));
    format_rate_or_dash(progress.upload_bytes_per_sec, progress.has_upload, state->use_bits,
                        upload, sizeof(upload));
    format_millis(progress.latency_us, progress.has_latency, ping, sizeof(ping));
    format_millis(progress.jitter_us, progress.has_latency, jitter, sizeof(jitter));

    const char *row_labels[3] = {"Download", "Upload", "Ping \xB7 jitter"};
    char ping_row[72];
    snprintf(ping_row, sizeof(ping_row), "%s \xB7 %s", ping, jitter);
    const char *row_values[3] = {download, upload, ping_row};
    const int rows_top = bar_top + 22;
    for (int i = 0; i < 3; ++i) {
        const int row_y = rows_top + i * LP_SPEED_ROW_HEIGHT;
        RECT label_rect = {32, row_y, center_x, row_y + LP_SPEED_ROW_HEIGHT};
        RECT value_row_rect = {center_x, row_y, client.right - 32, row_y + LP_SPEED_ROW_HEIGHT};
        draw_text(dc, state->label_font, muted, row_labels[i], label_rect, DT_LEFT | DT_VCENTER);
        draw_text(dc, state->label_font, text, row_values[i], value_row_rect,
                  DT_RIGHT | DT_VCENTER);
    }

    const RECT button = button_rect(&client);
    HBRUSH button_brush = CreateSolidBrush(button_fill);
    HPEN button_pen = CreatePen(PS_SOLID, 1, line);
    HBRUSH old_brush = (HBRUSH)SelectObject(dc, button_brush);
    HPEN old_pen = (HPEN)SelectObject(dc, button_pen);
    RoundRect(dc, button.left, button.top, button.right, button.bottom, 10, 10);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(button_pen);
    DeleteObject(button_brush);
    const char *button_text = running ? "Cancel" : "Run test";
    draw_text(dc, state->label_font, text, button_text, button, DT_CENTER | DT_VCENTER);

    if (progress.phase == LP_SPEEDTEST_PHASE_DONE) {
        const lp_speedtest_quality_t quality = lp_speedtest_quality(&progress);
        const COLORREF quality_tone = quality_color(quality, light);

        HPEN divider_pen = CreatePen(PS_SOLID, 1, track);
        HPEN old_divider_pen = (HPEN)SelectObject(dc, divider_pen);
        MoveToEx(dc, 32, LP_SPEED_QUALITY_TOP, NULL);
        LineTo(dc, client.right - 32, LP_SPEED_QUALITY_TOP);
        SelectObject(dc, old_divider_pen);
        DeleteObject(divider_pen);

        RECT quality_label = {32, LP_SPEED_QUALITY_TOP + 14, center_x,
                              LP_SPEED_QUALITY_TOP + 42};
        RECT quality_value = {center_x, LP_SPEED_QUALITY_TOP + 14, client.right - 32,
                              LP_SPEED_QUALITY_TOP + 42};
        draw_text(dc, state->label_font, muted, "Connection quality", quality_label,
                  DT_LEFT | DT_VCENTER);
        draw_text(dc, state->title_font, quality_tone, lp_speedtest_quality_str(quality),
                  quality_value, DT_RIGHT | DT_VCENTER);

        const int segment_top = LP_SPEED_QUALITY_TOP + 52;
        const int segment_span = (client.right - 64) / 4;
        for (int i = 0; i < 4; ++i) {
            RECT segment = {32 + i * segment_span, segment_top,
                            32 + (i + 1) * segment_span - 8, segment_top + 10};
            HBRUSH segment_brush = CreateSolidBrush((int)quality > i ? quality_tone : track);
            FillRect(dc, &segment, segment_brush);
            DeleteObject(segment_brush);
        }

        RECT hint = {32, segment_top + 22, client.right - 32, segment_top + 76};
        draw_wrapped_text(dc, state->small_font, muted, lp_speedtest_quality_hint(quality), hint);
    }

    HBRUSH old_frame_brush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN frame_pen = CreatePen(PS_SOLID, 1, line);
    HPEN old_frame_pen = (HPEN)SelectObject(dc, frame_pen);
    RoundRect(dc, 0, 0, client.right, client.bottom, 24, 24);
    SelectObject(dc, old_frame_pen);
    SelectObject(dc, old_frame_brush);
    DeleteObject(frame_pen);

    BitBlt(target_dc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

/* Worker-thread callback: publishes a snapshot and wakes the UI thread. */
static void on_speedtest_progress(const lp_speedtest_progress_t *progress, void *user_data)
{
    HWND window = (HWND)user_data;
    lp_speed_meter_state_t *state = state_of(window);
    if (state == NULL) {
        return;
    }
    EnterCriticalSection(&state->lock);
    state->progress = *progress;
    LeaveCriticalSection(&state->lock);
    PostMessageA(window, WM_LP_SPEED_PROGRESS, 0, 0);
}

static DWORD WINAPI speedtest_thread_proc(LPVOID param)
{
    HWND window = (HWND)param;
    lp_speed_meter_state_t *state = state_of(window);
    if (state == NULL) {
        return 0;
    }
    lp_speedtest_request_t request;
    memset(&request, 0, sizeof(request));
    request.on_progress = on_speedtest_progress;
    request.user_data = window;
    request.cancel_flag = &state->cancel;

    lp_speedtest_progress_t result;
    (void)lp_speedtest_run(&request, &result);

    EnterCriticalSection(&state->lock);
    state->progress = result;
    LeaveCriticalSection(&state->lock);
    InterlockedExchange(&state->running, 0);
    PostMessageA(window, WM_LP_SPEED_PROGRESS, 0, 0);
    return 0;
}

static void start_test(HWND window, lp_speed_meter_state_t *state)
{
    if (InterlockedCompareExchange(&state->running, 1, 0) != 0) {
        return;
    }
    if (state->thread != NULL) {
        WaitForSingleObject(state->thread, INFINITE);
        CloseHandle(state->thread);
        state->thread = NULL;
    }
    state->cancel = 0;
    EnterCriticalSection(&state->lock);
    memset(&state->progress, 0, sizeof(state->progress));
    LeaveCriticalSection(&state->lock);
    InvalidateRect(window, NULL, FALSE);

    state->thread = CreateThread(NULL, 0, speedtest_thread_proc, window, 0, NULL);
    if (state->thread == NULL) {
        InterlockedExchange(&state->running, 0);
    }
}

/* Signals the worker to abort and blocks until it has actually stopped. */
static void stop_test(lp_speed_meter_state_t *state)
{
    state->cancel = 1;
    if (state->thread != NULL) {
        WaitForSingleObject(state->thread, INFINITE);
        CloseHandle(state->thread);
        state->thread = NULL;
    }
    InterlockedExchange(&state->running, 0);
}

static LRESULT CALLBACK speed_meter_wndproc(HWND window, UINT message, WPARAM wparam,
                                            LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        lp_speed_meter_state_t *state =
            (lp_speed_meter_state_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
        if (state == NULL) {
            return -1;
        }
        InitializeCriticalSection(&state->lock);
        state->title_font = CreateFontA(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        state->value_font = CreateFontA(38, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        state->label_font = CreateFontA(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        state->small_font = CreateFontA(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        state->background_brush =
            CreateSolidBrush(is_taskbar_light_theme() ? RGB(250, 251, 252) : RGB(28, 30, 34));
        snprintf(state->router_label, sizeof(state->router_label), "Gateway");
        SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)state);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        paint_meter(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LP_SPEED_PROGRESS:
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            lp_speed_meter_state_t *state = state_of(window);
            if (state != NULL) {
                go_back(window, state);
            }
        }
        return 0;
    case WM_LBUTTONUP: {
        lp_speed_meter_state_t *state = state_of(window);
        if (state == NULL) {
            return 0;
        }
        const int x = (short)LOWORD(lparam);
        const int y = (short)HIWORD(lparam);
        RECT client;
        GetClientRect(window, &client);
        if (x >= client.right - 58 && y <= 58) {
            cancel_test(state);
            ShowWindow(window, SW_HIDE);
            return 0;
        }
        const RECT back = back_rect();
        if (x >= back.left && x <= back.right && y >= back.top && y <= back.bottom) {
            go_back(window, state);
            return 0;
        }
        const RECT button = button_rect(&client);
        if (x >= button.left && x <= button.right && y >= button.top && y <= button.bottom) {
            if (InterlockedCompareExchange(&state->running, 0, 0) != 0) {
                cancel_test(state);
            } else {
                start_test(window, state);
            }
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    }
    case WM_SETTINGCHANGE: {
        lp_speed_meter_state_t *state = state_of(window);
        if (state != NULL) {
            DeleteObject(state->background_brush);
            state->background_brush =
                CreateSolidBrush(is_taskbar_light_theme() ? RGB(250, 251, 252) : RGB(28, 30, 34));
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    }
    case WM_DESTROY: {
        lp_speed_meter_state_t *state = state_of(window);
        if (state != NULL) {
            stop_test(state);
            DeleteObject(state->title_font);
            DeleteObject(state->value_font);
            DeleteObject(state->label_font);
            DeleteObject(state->small_font);
            DeleteObject(state->background_brush);
            DeleteCriticalSection(&state->lock);
            HeapFree(GetProcessHeap(), 0, state);
            SetWindowLongPtrA(window, GWLP_USERDATA, 0);
        }
        return 0;
    }
    default:
        return DefWindowProcA(window, message, wparam, lparam);
    }
}

HWND lp_speed_meter_create(HINSTANCE instance, HWND owner)
{
    WNDCLASSEXA window_class;
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = speed_meter_wndproc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.lpszClassName = "LinkPulseSpeedMeterWindow";
    if (RegisterClassExA(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return NULL;
    }

    HWND window = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, window_class.lpszClassName,
                                  "LinkPulse speed", WS_POPUP, 0, 0, LP_SPEED_WIDTH,
                                  LP_SPEED_HEIGHT, owner, NULL, instance, NULL);
    if (window != NULL) {
        HRGN region = CreateRoundRectRgn(0, 0, LP_SPEED_WIDTH + 1, LP_SPEED_HEIGHT + 1, 24, 24);
        if (region != NULL && SetWindowRgn(window, region, FALSE) == 0) {
            DeleteObject(region);
        }
    }
    return window;
}

void lp_speed_meter_show(HWND window, const char *router_label, const char *router_ip,
                         const char *isp, bool use_bits)
{
    if (window == NULL) {
        return;
    }
    lp_speed_meter_state_t *state = state_of(window);
    if (state == NULL) {
        return;
    }
    snprintf(state->router_label, sizeof(state->router_label), "%s",
             router_label != NULL && router_label[0] != '\0' ? router_label : "Gateway");
    snprintf(state->router_ip, sizeof(state->router_ip), "%s", router_ip != NULL ? router_ip : "");
    snprintf(state->isp, sizeof(state->isp), "%s", isp != NULL ? isp : "");
    state->use_bits = use_bits;

    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info;
    memset(&monitor_info, 0, sizeof(monitor_info));
    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoA(monitor, &monitor_info);
    const int x = monitor_info.rcWork.right - LP_SPEED_WIDTH - 12;
    const int y = monitor_info.rcWork.bottom - LP_SPEED_HEIGHT - 8;
    SetWindowPos(window, HWND_TOPMOST, x, y, LP_SPEED_WIDTH, LP_SPEED_HEIGHT,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(window, NULL, FALSE);
    SetForegroundWindow(window);
    LP_INFO("speed meter opened: router=%s ip=%s", state->router_label, state->router_ip);
}
