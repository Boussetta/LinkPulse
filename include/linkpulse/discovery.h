#ifndef LINKPULSE_DISCOVERY_H
#define LINKPULSE_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linkpulse/status.h"

#define LP_MAC_STR_MAX 18
#define LP_IP_STR_MAX 46
#define LP_HOSTNAME_MAX 256
#define LP_VENDOR_MAX 64
#define LP_DISCOVERY_MAX_NEIGHBORS 256
#define LP_DISCOVERY_MAX_EVENTS 32
#define LP_DISCOVERY_MAX_NETWORKS 32
#define LP_DISCOVERY_MISSING_POLLS_BEFORE_LEFT 3

typedef enum {
    LP_CONNECTION_UNKNOWN = 0,
    LP_CONNECTION_WIFI,
    LP_CONNECTION_ETHERNET
} lp_connection_type_t;

typedef enum {
    LP_DEVICE_UNKNOWN = 0,
    LP_DEVICE_LAPTOP,
    LP_DEVICE_MOBILE,
    LP_DEVICE_SMARTWATCH,
    LP_DEVICE_PRINTER,
    LP_DEVICE_TELEVISION,
    LP_DEVICE_ROUTER,
    LP_DEVICE_DESKTOP
} lp_device_type_t;

typedef struct {
    char ip[LP_IP_STR_MAX];
    char mac[LP_MAC_STR_MAX];
    char hostname[LP_HOSTNAME_MAX];
    char vendor[LP_VENDOR_MAX];
    lp_connection_type_t connection_type;
    lp_device_type_t device_type;
    uint8_t device_confidence;
    bool active;
} lp_neighbor_t;

typedef struct {
    lp_neighbor_t items[LP_DISCOVERY_MAX_NEIGHBORS];
    size_t count;
} lp_neighbor_list_t;

typedef struct {
    char address[LP_IP_STR_MAX];
    uint8_t prefix_length;
    char gateway[LP_IP_STR_MAX];
    char gateway_mac[LP_MAC_STR_MAX];
    char gateway_hostname[LP_HOSTNAME_MAX];
    char gateway_vendor[LP_VENDOR_MAX];
    lp_device_type_t gateway_device_type;
    uint8_t gateway_confidence;
} lp_local_network_t;

typedef struct {
    lp_local_network_t items[LP_DISCOVERY_MAX_NETWORKS];
    size_t count;
} lp_local_network_list_t;

typedef lp_status_t (*lp_net_neighbor_snapshot_fn)(lp_neighbor_list_t *out);
typedef lp_status_t (*lp_net_local_networks_fn)(lp_local_network_list_t *out);

typedef struct {
    lp_net_neighbor_snapshot_fn snapshot_fn;
    lp_net_local_networks_fn local_networks_fn;
} lp_discovery_sources_t;

typedef enum {
    LP_DISCOVERY_EVENT_JOINED = 0,
    LP_DISCOVERY_EVENT_LEFT
} lp_discovery_event_type_t;

typedef struct {
    lp_discovery_event_type_t type;
    lp_neighbor_t neighbor;
} lp_discovery_event_t;

typedef struct {
    lp_discovery_sources_t sources;
    lp_neighbor_t known[LP_DISCOVERY_MAX_NEIGHBORS];
    uint8_t missing_polls[LP_DISCOVERY_MAX_NEIGHBORS];
    size_t known_count;
    bool has_baseline;
} lp_discovery_t;

void lp_discovery_init(lp_discovery_t *discovery);

void lp_discovery_set_sources(lp_discovery_t *discovery,
                              const lp_discovery_sources_t *sources);

lp_status_t lp_discovery_poll(lp_discovery_t *discovery, lp_discovery_event_t *events,
                              size_t max_events, size_t *event_count);

lp_status_t lp_net_neighbor_snapshot(lp_neighbor_list_t *out);
lp_status_t lp_net_local_networks(lp_local_network_list_t *out);

#endif /* LINKPULSE_DISCOVERY_H */