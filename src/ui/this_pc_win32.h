#ifndef LINKPULSE_THIS_PC_WIN32_H
#define LINKPULSE_THIS_PC_WIN32_H

#include "linkpulse/discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <windows.h>

/* Sent to the owner window when the user asks to return to the network map. */
#define LP_THIS_PC_COPYDATA_BACK 20

/* Points of rx/tx history rendered as a Task-Manager-style throughput graph. */
#define LP_THIS_PC_GRAPH_POINTS 60

/* Creates the hidden-until-shown "This PC" details popup. */
HWND lp_this_pc_create(HINSTANCE instance, HWND owner);

/* Updates the graph/adapter snapshot and shows or refreshes the popup. */
void lp_this_pc_show(HWND window, const char *hostname, const lp_local_network_list_t *networks,
                     const uint64_t *rx_history, const uint64_t *tx_history, size_t history_count,
                     bool use_bits);

#endif /* LINKPULSE_THIS_PC_WIN32_H */
