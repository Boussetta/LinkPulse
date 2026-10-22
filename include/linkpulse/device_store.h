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

/* Opens the per-user MAC-keyed XML device store. The returned handle is opaque.
   apply, observe, and flush are safe to call concurrently from any thread;
   close must not race with them. */
int lp_win32_device_store_open(void **store);

/* Applies persisted label, trust, type, and icon metadata for the given
   network's device to an observation. gateway_mac may be NULL/empty for an
   unresolved network. */
void lp_win32_device_store_apply(void *store, const char *gateway_mac, lp_neighbor_t *neighbor);

/* Records observed device and ISP metadata under the given network in memory.
   Call lp_win32_device_store_flush() once after a batch of observations. */
void lp_win32_device_store_observe(void *store, const lp_network_context_t *network_context,
                                    const lp_neighbor_t *neighbor);

/* Atomically persists pending observations, if any. A failed write keeps the
   previous file and retries on the next flush. */
void lp_win32_device_store_flush(void *store);

/* Flushes pending observations, then releases the store. */
void lp_win32_device_store_close(void *store);

#endif /* LINKPULSE_DEVICE_STORE_H */