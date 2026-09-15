#ifndef LINKPULSE_SPEED_METER_WIN32_H
#define LINKPULSE_SPEED_METER_WIN32_H

#include <stdbool.h>

#include <windows.h>

/* Sent to the owner window when the user asks to return to the network map. */
#define LP_SPEED_METER_COPYDATA_BACK 10

/* Creates the hidden-until-shown speed meter popup. */
HWND lp_speed_meter_create(HINSTANCE instance, HWND owner);

/* Shows the meter for one router and starts a fresh measurement run. */
void lp_speed_meter_show(HWND window, const char *router_label, const char *router_ip,
                         const char *isp, bool use_bits);

#endif /* LINKPULSE_SPEED_METER_WIN32_H */
