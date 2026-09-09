#include "lp_test.h"

#include "linkpulse/discovery.h"

#include <stdio.h>
#include <string.h>

#define FAKE_STEP_COUNT 5

static lp_neighbor_list_t g_fake_snapshots[FAKE_STEP_COUNT];
static size_t g_fake_snapshot_index;

/* Resets the scripted neighbor snapshots used by the reconciliation test. */
static void reset_fakes(void)
{
    memset(g_fake_snapshots, 0, sizeof(g_fake_snapshots));
    g_fake_snapshot_index = 0;
}

/* Seeds the stable identity fields for one fake neighbor. */
static void set_neighbor(lp_neighbor_t *neighbor, const char *ip, const char *mac)
{
    snprintf(neighbor->ip, sizeof(neighbor->ip), "%s", ip);
    snprintf(neighbor->mac, sizeof(neighbor->mac), "%s", mac);
}

/* Supplies the next scripted neighbor snapshot to the core. */
static lp_status_t fake_snapshot_fn(lp_neighbor_list_t *out)
{
    if (g_fake_snapshot_index >= FAKE_STEP_COUNT) {
        return LP_ERR_IO;
    }
    *out = g_fake_snapshots[g_fake_snapshot_index++];
    return LP_OK;
}

/* Verifies baseline suppression, MAC identity changes, joins, and delayed leaves. */
static void test_baseline_and_neighbor_changes(void)
{
    reset_fakes();
    set_neighbor(&g_fake_snapshots[0].items[0], "192.168.1.2", "AA:BB:CC:DD:EE:01");
    set_neighbor(&g_fake_snapshots[0].items[1], "192.168.1.3", "AA:BB:CC:DD:EE:02");
    g_fake_snapshots[0].count = 2;

    /* The first neighbor keeps its identity while its IP address changes. */
    set_neighbor(&g_fake_snapshots[1].items[0], "192.168.1.20", "AA:BB:CC:DD:EE:01");
    snprintf(g_fake_snapshots[1].items[0].hostname,
             sizeof(g_fake_snapshots[1].items[0].hostname), "living-room-tv");
    g_fake_snapshots[1].items[0].connection_type = LP_CONNECTION_WIFI;
    snprintf(g_fake_snapshots[1].items[0].vendor,
             sizeof(g_fake_snapshots[1].items[0].vendor), "Epson");
    g_fake_snapshots[1].items[0].device_type = LP_DEVICE_PRINTER;
    g_fake_snapshots[1].items[0].device_confidence = 95;
    set_neighbor(&g_fake_snapshots[1].items[1], "192.168.1.4", "AA:BB:CC:DD:EE:03");
    g_fake_snapshots[1].count = 2;

    set_neighbor(&g_fake_snapshots[2].items[0], "192.168.1.4", "AA:BB:CC:DD:EE:03");
    g_fake_snapshots[2].count = 1;
    g_fake_snapshots[3] = g_fake_snapshots[2];
    g_fake_snapshots[4] = g_fake_snapshots[2];

    lp_discovery_t discovery;
    const lp_discovery_sources_t sources = {fake_snapshot_fn, NULL};
    lp_discovery_init(&discovery);
    lp_discovery_set_sources(&discovery, &sources);

    lp_discovery_event_t events[LP_DISCOVERY_MAX_EVENTS];
    size_t event_count = 0;
    LP_CHECK(lp_discovery_poll(&discovery, events, LP_DISCOVERY_MAX_EVENTS, &event_count) == LP_OK);
    LP_CHECK(event_count == 0);

    LP_CHECK(lp_discovery_poll(&discovery, events, LP_DISCOVERY_MAX_EVENTS, &event_count) == LP_OK);
    LP_CHECK(event_count == 1);
    LP_CHECK(events[0].type == LP_DISCOVERY_EVENT_JOINED);
    LP_CHECK_STR_EQ(events[0].neighbor.ip, "192.168.1.4");

    LP_CHECK(lp_discovery_poll(&discovery, events, LP_DISCOVERY_MAX_EVENTS, &event_count) == LP_OK);
    LP_CHECK(event_count == 0);

    LP_CHECK(lp_discovery_poll(&discovery, events, LP_DISCOVERY_MAX_EVENTS, &event_count) == LP_OK);
    LP_CHECK(event_count == 1);
    LP_CHECK(events[0].type == LP_DISCOVERY_EVENT_LEFT);
    LP_CHECK_STR_EQ(events[0].neighbor.mac, "AA:BB:CC:DD:EE:02");

    LP_CHECK(lp_discovery_poll(&discovery, events, LP_DISCOVERY_MAX_EVENTS, &event_count) == LP_OK);
    LP_CHECK(event_count == 1);
    LP_CHECK(events[0].type == LP_DISCOVERY_EVENT_LEFT);
    LP_CHECK_STR_EQ(events[0].neighbor.mac, "AA:BB:CC:DD:EE:01");
    LP_CHECK_STR_EQ(events[0].neighbor.ip, "192.168.1.20");
    LP_CHECK_STR_EQ(events[0].neighbor.hostname, "living-room-tv");
    LP_CHECK(events[0].neighbor.connection_type == LP_CONNECTION_WIFI);
    LP_CHECK_STR_EQ(events[0].neighbor.vendor, "Epson");
    LP_CHECK(events[0].neighbor.device_type == LP_DEVICE_PRINTER);
    LP_CHECK(events[0].neighbor.device_confidence == 95);
    LP_CHECK(discovery.known_count == 3);
    LP_CHECK(!discovery.known[0].active || !discovery.known[1].active || !discovery.known[2].active);
}

/* Verifies discovery reports an unconfigured provider instead of dereferencing it. */
static void test_poll_rejects_missing_source(void)
{
    lp_discovery_t discovery;
    lp_discovery_event_t events[1];
    size_t event_count = 0;
    lp_discovery_init(&discovery);

    LP_CHECK(lp_discovery_poll(&discovery, events, 1, &event_count) == LP_ERR_UNSUPPORTED);
    LP_CHECK(event_count == 0);
}

/* Runs discovery reconciliation and provider-validation scenarios. */
int main(void)
{
    test_baseline_and_neighbor_changes();
    test_poll_rejects_missing_source();
    LP_TEST_RETURN();
}