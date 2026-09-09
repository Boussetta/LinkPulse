#ifndef LINKPULSE_DEVICE_STORE_H
#define LINKPULSE_DEVICE_STORE_H

#include "linkpulse/discovery.h"

/* Opens the per-user MAC-keyed XML device store. The returned handle is opaque. */
int lp_win32_device_store_open(void **store);

/* Applies persisted label, trust, type, and icon metadata to an observation. */
void lp_win32_device_store_apply(void *store, lp_neighbor_t *neighbor);

/* Records observed metadata and persists the store atomically. */
void lp_win32_device_store_observe(void *store, const lp_neighbor_t *neighbor);

/* Releases the store and any XML resources owned by it. */
void lp_win32_device_store_close(void *store);

#endif /* LINKPULSE_DEVICE_STORE_H */