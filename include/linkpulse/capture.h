#ifndef LINKPULSE_CAPTURE_H
#define LINKPULSE_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linkpulse/discovery.h"
#include "linkpulse/status.h"

/* libpcap link-layer type for Ethernet (DLT_EN10MB). */
#define LP_CAPTURE_LINKTYPE_ETHERNET 1

typedef struct {
    bool available;
    bool device_visible;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint32_t packet_count; /* IP packets to or from the device, not distinct flows */
    char detail[128];
} lp_capture_activity_t;

/* An IPv4 or IPv6 address in network byte order. */
typedef struct {
    bool is_ipv6;
    uint8_t bytes[16]; /* IPv4 uses the first 4 bytes */
} lp_capture_address_t;

typedef enum {
    LP_CAPTURE_FRAME_IGNORED = 0, /* malformed, truncated, non-IP, or not the device's traffic */
    LP_CAPTURE_FRAME_FROM_DEVICE,
    LP_CAPTURE_FRAME_TO_DEVICE,
} lp_capture_direction_t;

/* Classifies one captured frame relative to `device`. Accepts Ethernet frames with at most one
   802.1Q tag carrying IPv4 or IPv6, and never reads past `caplen`. Pure; safe on untrusted data. */
lp_capture_direction_t lp_capture_classify_frame(int link_type, const uint8_t *frame,
                                                 size_t caplen,
                                                 const lp_capture_address_t *device);

/* Captures visible metadata for one device for a bounded proof-of-concept run.
   Npcap is loaded dynamically from the system directory; no SDK or link-time dependency is
   required. Only packet headers are captured, in non-promiscuous mode. Callers must only invoke
   this after the user has opted in (lp_config_t.packet_capture). */
lp_status_t lp_win32_capture_device_activity(const char *device_ip, unsigned duration_ms,
                                             lp_capture_activity_t *out);

#endif /* LINKPULSE_CAPTURE_H */
