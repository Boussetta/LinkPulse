#include "lp_test.h"

#include "linkpulse/discovery.h"

#include <stdio.h>
#include <string.h>

#define FAKE_STEP_COUNT 3

static lp_neighbor_list_t g_fake_snapshots[FAKE_STEP_COUNT];
static size_t g_fake_snapshot_index;

static void reset_fakes(void)
{
    memset(g_fake_snapshots, 0, sizeof(g_fake_snapshots));
    g_fake_snapshot_index = 0;
}

static void set_neighbor(lp_neighbor_t *neighbor, const char *ip, const char *mac)
{
    snprintf(neighbor->ip, sizeof(neighbor->ip), "%s", ip);
    snprintf(neighbor->mac, sizeof(neighbor->mac), "%s", mac);
}

static lp_status_t fake_snapshot_fn(lp_neighbor_list_t *out)
{
    if (g_fake_snapshot_index >= FAKE_STEP_COUNT) {
        return LP_ERR_IO;
    }
    *out = g_fake_snapshots[g_fake_snapshot_index++];
    return LP_OK;
}

static void test_baseline_and_neighbor_changes(void)
{
    reset_fakes();
    set_neighbor(&g_fake_snapshots[0].items[0], "192.168.1.2", "AA:BB:CC:DD:EE:01");
    set_neighbor(&g_fake_snapshots[0].items[1], "192.168.1.3", "AA:BB:CC:DD:EE:02");
    g_fake_snapshots[0].count = 2;

    /* The first neighbor keeps its identity while its IP address changes. */
    set_neighbor(&g_fake_snapshots[1].items[0], "192.168.1.20", "AA:BB:CC:DD:EE:01");
    set_neighbor(&g_fake_snapshots[1].items[1], "192.168.1.4", "AA:BB:CC:DD:EE:03");
    g_fake_snapshots[1].count = 2;

    set_neighbor(&g_fake_snapshots[2].items[0], "192.168.1.4", "AA:BB:CC:DD:EE:03");
    g_fake_snapshots[2].count = 1;

    lp_discovery_t discovery;
    const lp_discovery_sources_t sources = {fake_snapshot_fn, NULL};
    lp_discovery_init(&discovery);
    lp_discovery_set_sources(&discovery, &sources);

    lp_discovery_event_t events[LP_DISCOVERY_MAX_EVENTS];
    size_t event_count = 0;
    LP_CHECK(lp_discovery_poll(&discovery, events, LP_DISCOVERY_MAX_EVENTS, &event_count) == LP_OK);
    LP_CHECK(event_count == 0);

    LP_CHECK(lp_discovery_poll(&discovery, events, LP_DISCOVERY_MAX_EVENTS, &event_count) == LP_OK);
    LP_CHECK(event_count == 2);
    LP_CHECK(events[0].type == LP_DISCOVERY_EVENT_JOINED);
    LP_CHECK_STR_EQ(events[0].neighbor.ip, "192.168.1.4");
    LP_CHECK(events[1].type == LP_DISCOVERY_EVENT_LEFT);
    LP_CHECK_STR_EQ(events[1].neighbor.ip, "192.168.1.3");

    LP_CHECK(lp_discovery_poll(&discovery, events, LP_DISCOVERY_MAX_EVENTS, &event_count) == LP_OK);
    LP_CHECK(event_count == 1);
    LP_CHECK(events[0].type == LP_DISCOVERY_EVENT_LEFT);
    LP_CHECK_STR_EQ(events[0].neighbor.mac, "AA:BB:CC:DD:EE:01");
    LP_CHECK_STR_EQ(events[0].neighbor.ip, "192.168.1.20");
}

static void test_poll_rejects_missing_source(void)
{
    lp_discovery_t discovery;
    lp_discovery_event_t events[1];
    size_t event_count = 0;
    lp_discovery_init(&discovery);

    LP_CHECK(lp_discovery_poll(&discovery, events, 1, &event_count) == LP_ERR_UNSUPPORTED);
    LP_CHECK(event_count == 0);
}

int main(void)
{
    test_baseline_and_neighbor_changes();
    test_poll_rejects_missing_source();
    LP_TEST_RETURN();
}