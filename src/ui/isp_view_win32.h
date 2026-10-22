#ifndef LINKPULSE_ISP_VIEW_WIN32_H
#define LINKPULSE_ISP_VIEW_WIN32_H

#include "linkpulse/isp.h"

#include <stdbool.h>

#include <windows.h>

/* Sent to the owner window when the user asks to return to the network map. */
#define LP_ISP_VIEW_COPYDATA_BACK 40

HWND lp_isp_view_create(HINSTANCE instance, HWND owner);
/* Stores the ISP snapshot, positions the popup near the taskbar, and activates it. Use for
   explicit user opens only. `info` is NULL until a lookup has succeeded; `lookup_failed` tells a
   failed lookup apart from one that is still in progress. */
void lp_isp_view_show(HWND window, const lp_isp_info_t *info, bool lookup_failed);
/* Stores the snapshot and repaints without moving, showing, or activating the popup. */
void lp_isp_view_update(HWND window, const lp_isp_info_t *info, bool lookup_failed);

#endif /* LINKPULSE_ISP_VIEW_WIN32_H */
