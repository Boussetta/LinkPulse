#ifndef LINKPULSE_DEVICE_DETAILS_WIN32_H
#define LINKPULSE_DEVICE_DETAILS_WIN32_H

#include "linkpulse/discovery.h"
#include "linkpulse/capture.h"

#include <windows.h>

#define LP_DEVICE_DETAILS_COPYDATA_BACK 30

HWND lp_device_details_create(HINSTANCE instance, HWND owner);
void lp_device_details_show(HWND window, const lp_neighbor_t *neighbor,
                            const lp_capture_activity_t *capture);

#endif /* LINKPULSE_DEVICE_DETAILS_WIN32_H */