#ifndef LINKPULSE_NET_H
#define LINKPULSE_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linkpulse/status.h"

#define LP_IFNAME_MAX 128
#define LP_IFDESC_MAX 256

/* One network interface and its cumulative byte counters at a point in time. */
typedef struct {
    char name[LP_IFNAME_MAX];     /* OS identifier: "eth0", or the adapter GUID on Windows */
    char description[LP_IFDESC_MAX]; /* human-readable name; equals `name` on Linux */
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    bool is_up;
    bool is_loopback;
    bool is_virtual; /* WSL vEthernet, Docker, VPN, tunnels */
} lp_iface_t;

typedef struct {
    lp_iface_t *items;
    size_t count;
} lp_iface_list_t;

/* Implemented per platform. On success the caller owns `out` and must free it
   with lp_iface_list_free(). */
lp_status_t lp_net_snapshot(lp_iface_list_t *out);

/* Name of the interface carrying the default route, i.e. the internet-facing one. */
lp_status_t lp_net_default_iface(char *name_out, size_t name_cap);

void lp_iface_list_free(lp_iface_list_t *list);
const lp_iface_t *lp_iface_list_find(const lp_iface_list_t *list, const char *name);

#endif /* LINKPULSE_NET_H */
