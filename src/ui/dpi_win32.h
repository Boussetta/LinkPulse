#ifndef LINKPULSE_DPI_WIN32_H
#define LINKPULSE_DPI_WIN32_H

#include <windows.h>

/* The DPI every popup's layout constants are written for (100% scaling). */
#define LP_UI_DEFAULT_DPI 96u

/* Declares Per-Monitor V2 DPI awareness, falling back to older awareness APIs on systems that
   lack it. Must run before the first window is created; later calls are harmless no-ops. */
void lp_ui_enable_dpi_awareness(void);

/* Effective DPI of a window or monitor, or LP_UI_DEFAULT_DPI when it cannot be determined. */
UINT lp_ui_window_dpi(HWND window);
UINT lp_ui_monitor_dpi(HMONITOR monitor);

/* Converts between 96-DPI layout units and physical pixels at `dpi`. */
int lp_ui_scale(int value, UINT dpi);
int lp_ui_unscale(int value, UINT dpi);

/* Popups keep painting in 96-DPI layout units: these scale the DC with a world transform (a
   no-op at 96 DPI, so 100% stays pixel-identical) and report the client area in layout units.
   Call lp_ui_end_scaled_paint before blitting a scaled memory DC. */
void lp_ui_begin_scaled_paint(HDC dc, UINT dpi);
void lp_ui_end_scaled_paint(HDC dc, UINT dpi);
RECT lp_ui_layout_client_rect(HWND window, UINT dpi);
/* Converts a mouse message's client coordinates into layout units. */
POINT lp_ui_layout_point(LPARAM lparam, UINT dpi);

/* Computes the physical rectangle of a `layout_width` x `layout_height` popup docked at the
   bottom-right of `monitor`'s work area, and the DPI it was sized for. */
RECT lp_ui_popup_rect(HMONITOR monitor, int layout_width, int layout_height, UINT *out_dpi);
/* Clips the popup to its rounded frame at its current physical size. */
void lp_ui_apply_popup_region(HWND window, int width, int height, UINT dpi);
/* WM_DPICHANGED handler: resizes the popup for the new DPI, keeping its bottom-right corner
   docked, and repaints it. */
void lp_ui_handle_dpi_changed(HWND window, WPARAM wparam, int layout_width, int layout_height);

#endif /* LINKPULSE_DPI_WIN32_H */
