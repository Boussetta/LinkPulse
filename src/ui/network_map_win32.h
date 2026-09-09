#ifndef LINKPULSE_NETWORK_MAP_WIN32_H
#define LINKPULSE_NETWORK_MAP_WIN32_H

#include "linkpulse/discovery.h"

#include <windows.h>

/* Creates the hidden-until-shown network map popup. */
HWND lp_network_map_create(HINSTANCE instance, HWND owner);

/* Updates map data and shows or refreshes the popup. */
void lp_network_map_show(HWND window, const lp_neighbor_list_t *neighbors,
                         const lp_local_network_list_t *networks);

#endif /* LINKPULSE_NETWORK_MAP_WIN32_H */