#ifndef LINKPULSE_DEVICE_DETAILS_WIN32_H
#define LINKPULSE_DEVICE_DETAILS_WIN32_H

#include "linkpulse/discovery.h"
#include "linkpulse/capture.h"

#include <windows.h>

#define LP_DEVICE_DETAILS_COPYDATA_BACK 30

typedef enum {
    LP_DEVICE_CAPTURE_OFF = 0, /* the user has not opted in to packet capture */
    LP_DEVICE_CAPTURE_RUNNING, /* a sample for this device is in progress */
    LP_DEVICE_CAPTURE_DONE,    /* `capture` holds this device's finished sample */
} lp_device_capture_state_t;

HWND lp_device_details_create(HINSTANCE instance, HWND owner);
/* Stores the snapshot, positions the popup near the taskbar, and activates it. Use for explicit
   user opens only. `capture` is only read when `capture_state` is LP_DEVICE_CAPTURE_DONE. */
void lp_device_details_show(HWND window, const lp_neighbor_t *neighbor,
                            lp_device_capture_state_t capture_state,
                            const lp_capture_activity_t *capture);
/* Stores the snapshot and repaints without moving, showing, or activating the popup. */
void lp_device_details_update(HWND window, const lp_neighbor_t *neighbor,
                              lp_device_capture_state_t capture_state,
                              const lp_capture_activity_t *capture);

#endif /* LINKPULSE_DEVICE_DETAILS_WIN32_H */