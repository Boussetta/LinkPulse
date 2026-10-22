#include "linkpulse/activity.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char destination_ip[LP_IP_STR_MAX];
    uint16_t destination_port;
} lp_endpoint_t;

lp_activity_detector_config_t lp_activity_detector_default_config(void)
{
    const lp_activity_detector_config_t config = {60000, 40, 16, 16};
    return config;
}

static bool observation_is_in_window(const lp_flow_observation_t *observation,
                                     const char *device_ip, uint64_t now_ms, uint64_t window_ms)
{
    return strcmp(observation->source_ip, device_ip) == 0 &&
           observation->destination_ip[0] != '\0' && observation->destination_port != 0 &&
           observation->observed_at_ms <= now_ms &&
           now_ms - observation->observed_at_ms <= window_ms;
}

static bool endpoint_exists(const lp_endpoint_t *endpoints, size_t endpoint_count,
                            const lp_flow_observation_t *observation)
{
    for (size_t i = 0; i < endpoint_count; ++i) {
        if (endpoints[i].destination_port == observation->destination_port &&
            strcmp(endpoints[i].destination_ip, observation->destination_ip) == 0) {
            return true;
        }
    }
    return false;
}

lp_status_t lp_activity_assess_device(const lp_flow_observation_t *observations,
                                      size_t observation_count, const char *device_ip,
                                      uint64_t now_ms, const lp_activity_detector_config_t *config,
                                      lp_device_risk_assessment_t *out)
{
    if (observations == NULL || device_ip == NULL || device_ip[0] == '\0' || config == NULL ||
        out == NULL || observation_count > LP_ACTIVITY_MAX_OBSERVATIONS || config->window_ms == 0 ||
        config->fanout_destination_threshold == 0 ||
        config->horizontal_scan_destination_threshold == 0 ||
        config->vertical_scan_port_threshold == 0) {
        return LP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    lp_endpoint_t endpoints[LP_ACTIVITY_MAX_OBSERVATIONS];
    size_t endpoint_count = 0;
    for (size_t i = 0; i < observation_count; ++i) {
        if (!observation_is_in_window(&observations[i], device_ip, now_ms, config->window_ms)) {
            continue;
        }
        ++out->flow_count;
        if (!endpoint_exists(endpoints, endpoint_count, &observations[i])) {
            snprintf(endpoints[endpoint_count].destination_ip,
                     sizeof(endpoints[endpoint_count].destination_ip), "%s",
                     observations[i].destination_ip);
            endpoints[endpoint_count].destination_port = observations[i].destination_port;
            ++endpoint_count;
        }
    }

    for (size_t i = 0; i < endpoint_count; ++i) {
        bool first_destination = true;
        bool first_port = true;
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(endpoints[j].destination_ip, endpoints[i].destination_ip) == 0) {
                first_destination = false;
            }
            if (endpoints[j].destination_port == endpoints[i].destination_port) {
                first_port = false;
            }
        }
        if (first_destination)
            ++out->distinct_destination_count;
        if (first_port)
            ++out->distinct_port_count;

        size_t destinations_on_port = 0;
        size_t ports_on_destination = 0;
        for (size_t j = 0; j < endpoint_count; ++j) {
            if (endpoints[j].destination_port == endpoints[i].destination_port) {
                ++destinations_on_port;
            }
            if (strcmp(endpoints[j].destination_ip, endpoints[i].destination_ip) == 0) {
                ++ports_on_destination;
            }
        }
        if (destinations_on_port > out->max_destinations_on_one_port) {
            out->max_destinations_on_one_port = destinations_on_port;
            out->busiest_port = endpoints[i].destination_port;
        }
        if (ports_on_destination > out->max_ports_on_one_destination) {
            out->max_ports_on_one_destination = ports_on_destination;
            snprintf(out->busiest_destination_ip, sizeof(out->busiest_destination_ip), "%s",
                     endpoints[i].destination_ip);
        }
    }

    out->high_fanout = out->distinct_destination_count >= config->fanout_destination_threshold;
    out->horizontal_port_scan =
        out->max_destinations_on_one_port >= config->horizontal_scan_destination_threshold;
    out->vertical_port_scan =
        out->max_ports_on_one_destination >= config->vertical_scan_port_threshold;
    return LP_OK;
}