#ifndef LINKPULSE_DEVICE_STORE_H
#define LINKPULSE_DEVICE_STORE_H

#include "linkpulse/discovery.h"
#include "linkpulse/isp.h"

/* Identifies which physical network (by gateway MAC) an observation belongs
   to, so devices and ISP identity stay scoped per-router instead of pooled
   into one global list across every network ever connected to. */
typedef struct {
    const char *gateway_mac;
    const char *gateway_hostname;
    const char *gateway_vendor;
    const lp_isp_info_t *isp; /* NULL when not yet known this poll */
} lp_network_context_t;

/* Opens the per-user MAC-keyed XML device store. The returned handle is opaque. */
int lp_win32_device_store_open(void **store);

/* Applies persisted label, trust, type, and icon metadata for the given
   network's device to an observation. gateway_mac may be NULL/empty for an
   unresolved network. */
void lp_win32_device_store_apply(void *store, const char *gateway_mac, lp_neighbor_t *neighbor);

/* Records observed device and ISP metadata under the given network and
   persists the store atomically. */
void lp_win32_device_store_observe(void *store, const lp_network_context_t *network_context,
                                    const lp_neighbor_t *neighbor);

/* Releases the store and any XML resources owned by it. */
void lp_win32_device_store_close(void *store);

#endif /* LINKPULSE_DEVICE_STORE_H */