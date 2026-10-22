#ifndef LINKPULSE_ACTIVITY_H
#define LINKPULSE_ACTIVITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linkpulse/discovery.h"
#include "linkpulse/status.h"

#define LP_ACTIVITY_MAX_OBSERVATIONS 512

typedef struct {
    char source_ip[LP_IP_STR_MAX];
    char destination_ip[LP_IP_STR_MAX];
    uint16_t destination_port;
    uint64_t observed_at_ms;
} lp_flow_observation_t;

typedef struct {
    uint64_t window_ms;
    size_t fanout_destination_threshold;
    size_t horizontal_scan_destination_threshold;
    size_t vertical_scan_port_threshold;
} lp_activity_detector_config_t;

typedef struct {
    size_t flow_count;
    size_t distinct_destination_count;
    size_t distinct_port_count;
    size_t max_destinations_on_one_port;
    uint16_t busiest_port;
    size_t max_ports_on_one_destination;
    char busiest_destination_ip[LP_IP_STR_MAX];
    bool high_fanout;
    bool horizontal_port_scan;
    bool vertical_port_scan;
} lp_device_risk_assessment_t;

/* Returns conservative defaults for a rolling one-minute observation window. */
lp_activity_detector_config_t lp_activity_detector_default_config(void);

/* Assesses outbound observations for one device. The caller supplies flow data
   from a router or gateway sensor; local neighbor discovery is not flow data. */
lp_status_t lp_activity_assess_device(const lp_flow_observation_t *observations,
                                      size_t observation_count, const char *device_ip,
                                      uint64_t now_ms, const lp_activity_detector_config_t *config,
                                      lp_device_risk_assessment_t *out);

#endif /* LINKPULSE_ACTIVITY_H */