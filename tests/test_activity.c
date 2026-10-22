#include "lp_test.h"

#include "linkpulse/activity.h"

#include <stdio.h>
#include <string.h>

static void add_flow(lp_flow_observation_t *flow, const char *source, const char *destination,
                     uint16_t port, uint64_t observed_at_ms)
{
    snprintf(flow->source_ip, sizeof(flow->source_ip), "%s", source);
    snprintf(flow->destination_ip, sizeof(flow->destination_ip), "%s", destination);
    flow->destination_port = port;
    flow->observed_at_ms = observed_at_ms;
}

static void test_repeated_flow_is_counted_once_for_risk(void)
{
    lp_flow_observation_t flows[3] = {0};
    add_flow(&flows[0], "192.168.1.20", "203.0.113.5", 443, 9000);
    add_flow(&flows[1], "192.168.1.20", "203.0.113.5", 443, 9500);
    add_flow(&flows[2], "192.168.1.21", "203.0.113.6", 443, 9500);
    lp_activity_detector_config_t config = {60000, 2, 2, 2};
    lp_device_risk_assessment_t result;

    LP_CHECK(lp_activity_assess_device(flows, 3, "192.168.1.20", 10000, &config, &result) == LP_OK);
    LP_CHECK(result.flow_count == 2);
    LP_CHECK(result.distinct_destination_count == 1);
    LP_CHECK(result.distinct_port_count == 1);
    LP_CHECK(!result.high_fanout);
    LP_CHECK(!result.horizontal_port_scan);
    LP_CHECK(!result.vertical_port_scan);
}

static void test_detects_fanout_and_horizontal_scan(void)
{
    lp_flow_observation_t flows[4] = {0};
    add_flow(&flows[0], "192.168.1.20", "203.0.113.1", 22, 9000);
    add_flow(&flows[1], "192.168.1.20", "203.0.113.2", 22, 9100);
    add_flow(&flows[2], "192.168.1.20", "203.0.113.3", 22, 9200);
    add_flow(&flows[3], "192.168.1.20", "203.0.113.4", 443, 9300);
    lp_activity_detector_config_t config = {60000, 4, 3, 3};
    lp_device_risk_assessment_t result;

    LP_CHECK(lp_activity_assess_device(flows, 4, "192.168.1.20", 10000, &config, &result) == LP_OK);
    LP_CHECK(result.high_fanout);
    LP_CHECK(result.horizontal_port_scan);
    LP_CHECK(!result.vertical_port_scan);
    LP_CHECK(result.max_destinations_on_one_port == 3);
    LP_CHECK(result.busiest_port == 22);
}

static void test_detects_vertical_scan_and_ignores_stale_flows(void)
{
    lp_flow_observation_t flows[4] = {0};
    add_flow(&flows[0], "192.168.1.20", "203.0.113.8", 21, 9000);
    add_flow(&flows[1], "192.168.1.20", "203.0.113.8", 22, 9100);
    add_flow(&flows[2], "192.168.1.20", "203.0.113.8", 23, 9200);
    add_flow(&flows[3], "192.168.1.20", "203.0.113.9", 80, 1000);
    lp_activity_detector_config_t config = {2000, 4, 4, 3};
    lp_device_risk_assessment_t result;

    LP_CHECK(lp_activity_assess_device(flows, 4, "192.168.1.20", 10000, &config, &result) == LP_OK);
    LP_CHECK(!result.high_fanout);
    LP_CHECK(!result.horizontal_port_scan);
    LP_CHECK(result.vertical_port_scan);
    LP_CHECK(result.max_ports_on_one_destination == 3);
    LP_CHECK_STR_EQ(result.busiest_destination_ip, "203.0.113.8");
}

int main(void)
{
    test_repeated_flow_is_counted_once_for_risk();
    test_detects_fanout_and_horizontal_scan();
    test_detects_vertical_scan_and_ignores_stale_flows();
    LP_TEST_RETURN();
}