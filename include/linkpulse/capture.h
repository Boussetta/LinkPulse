#ifndef LINKPULSE_CAPTURE_H
#define LINKPULSE_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "linkpulse/discovery.h"
#include "linkpulse/status.h"

typedef struct {
    bool available;
    bool device_visible;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint32_t flow_count;
    char detail[128];
} lp_capture_activity_t;

/* Captures visible metadata for one device for a bounded proof-of-concept run.
   Npcap is loaded dynamically; no SDK or link-time dependency is required. */
lp_status_t lp_win32_capture_device_activity(const char *device_ip, unsigned duration_ms,
                                             lp_capture_activity_t *out);

#endif /* LINKPULSE_CAPTURE_H */