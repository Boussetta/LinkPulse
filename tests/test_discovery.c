#include "lp_test.h"

#include "linkpulse/discovery.h"

#include <stdio.h>
#include <string.h>

#define FAKE_STEP_COUNT 6

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

    static lp_discovery_t discovery; /* too large for the stack */
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
    static lp_discovery_t discovery;
    lp_discovery_event_t events[1];
    size_t event_count = 0;
    lp_discovery_init(&discovery);

    LP_CHECK(lp_discovery_poll(&discovery, events, 1, &event_count) == LP_ERR_UNSUPPORTED);
    LP_CHECK(event_count == 0);
}

static lp_discovery_t g_discovery;
static lp_discovery_event_t g_events[2 * LP_DISCOVERY_MAX_NEIGHBORS];

/* Starts a fresh discovery instance backed by the scripted snapshots. */
static void start_discovery(void)
{
    const lp_discovery_sources_t sources = {fake_snapshot_fn, NULL};
    lp_discovery_init(&g_discovery);
    lp_discovery_set_sources(&g_discovery, &sources);
}

/* Polls once with a buffer large enough for every possible event. */
static size_t poll_events(void)
{
    size_t event_count = 0;
    LP_CHECK(lp_discovery_poll(&g_discovery, g_events, sizeof(g_events) / sizeof(g_events[0]),
                               &event_count) == LP_OK);
    return event_count;
}

/* Counts the events of one type from the last poll. */
static size_t count_events(size_t event_count, lp_discovery_event_type_t type)
{
    size_t count = 0;
    for (size_t i = 0; i < event_count; ++i) {
        if (g_events[i].type == type) {
            ++count;
        }
    }
    return count;
}

/* Fills one snapshot with a full table of distinct neighbors from one MAC block. */
static void fill_snapshot(lp_neighbor_list_t *snapshot, unsigned block)
{
    for (unsigned i = 0; i < LP_DISCOVERY_MAX_NEIGHBORS; ++i) {
        char ip[LP_IP_STR_MAX];
        char mac[LP_MAC_STR_MAX];
        snprintf(ip, sizeof(ip), "10.%u.%u.%u", block, i / 250u, 1u + i % 250u);
        snprintf(mac, sizeof(mac), "02:00:00:%02X:%02X:%02X", block, i / 256u, i % 256u);
        set_neighbor(&snapshot->items[i], ip, mac);
    }
    snapshot->count = LP_DISCOVERY_MAX_NEIGHBORS;
}

/* Verifies a full table neither repeats joins nor keeps departed neighbors forever. */
static void test_full_table_churn_evicts_departed_neighbors(void)
{
    reset_fakes();
    fill_snapshot(&g_fake_snapshots[0], 1);
    for (size_t step = 1; step < FAKE_STEP_COUNT; ++step) {
        fill_snapshot(&g_fake_snapshots[step], 2);
    }
    start_discovery();

    LP_CHECK(poll_events() == 0);
    /* The first block is still within its grace period, so the second block
       cannot be stored yet and must not be reported as joined. */
    LP_CHECK(poll_events() == 0);
    LP_CHECK(poll_events() == 0);

    const size_t event_count = poll_events();
    LP_CHECK(count_events(event_count, LP_DISCOVERY_EVENT_LEFT) == LP_DISCOVERY_MAX_NEIGHBORS);
    LP_CHECK(count_events(event_count, LP_DISCOVERY_EVENT_JOINED) == LP_DISCOVERY_MAX_NEIGHBORS);
    LP_CHECK(g_discovery.known_count == LP_DISCOVERY_MAX_NEIGHBORS);

    LP_CHECK(poll_events() == 0);
    bool all_second_block = true;
    for (size_t i = 0; i < g_discovery.known_count; ++i) {
        all_second_block = all_second_block && g_discovery.known[i].active &&
                           strncmp(g_discovery.known[i].mac, "02:00:00:02:", 12) == 0;
    }
    LP_CHECK(all_second_block);
}

/* Verifies a neighbor that left is reported as joined exactly once when it returns. */
static void test_rejoin_after_left_joins_once(void)
{
    reset_fakes();
    set_neighbor(&g_fake_snapshots[0].items[0], "192.168.1.9", "AA:BB:CC:DD:EE:09");
    g_fake_snapshots[0].count = 1;
    g_fake_snapshots[4] = g_fake_snapshots[0];
    g_fake_snapshots[5] = g_fake_snapshots[0];
    start_discovery();

    LP_CHECK(poll_events() == 0);
    LP_CHECK(poll_events() == 0);
    LP_CHECK(poll_events() == 0);
    size_t event_count = poll_events();
    LP_CHECK(event_count == 1);
    LP_CHECK(count_events(event_count, LP_DISCOVERY_EVENT_LEFT) == 1);

    event_count = poll_events();
    LP_CHECK(event_count == 1);
    LP_CHECK(count_events(event_count, LP_DISCOVERY_EVENT_JOINED) == 1);
    LP_CHECK_STR_EQ(g_events[0].neighbor.mac, "AA:BB:CC:DD:EE:09");
    LP_CHECK(poll_events() == 0);
    LP_CHECK(g_discovery.known_count == 1);
}

/* Verifies neighbors reported without a MAC are tracked by IP address. */
static void test_mac_less_neighbor_uses_ip_identity(void)
{
    reset_fakes();
    set_neighbor(&g_fake_snapshots[0].items[0], "fe80::5", "");
    g_fake_snapshots[0].count = 1;
    g_fake_snapshots[1] = g_fake_snapshots[0];
    g_fake_snapshots[2] = g_fake_snapshots[0];
    set_neighbor(&g_fake_snapshots[2].items[1], "fe80::6", "");
    g_fake_snapshots[2].count = 2;
    start_discovery();

    LP_CHECK(poll_events() == 0);
    LP_CHECK(poll_events() == 0);
    const size_t event_count = poll_events();
    LP_CHECK(event_count == 1);
    LP_CHECK(count_events(event_count, LP_DISCOVERY_EVENT_JOINED) == 1);
    LP_CHECK_STR_EQ(g_events[0].neighbor.ip, "fe80::6");
    LP_CHECK(g_discovery.known_count == 2);
}

/* Verifies one identity listed twice in a snapshot is stored and reported once. */
static void test_duplicate_mac_in_snapshot_joins_once(void)
{
    reset_fakes();
    set_neighbor(&g_fake_snapshots[1].items[0], "192.168.1.7", "AA:BB:CC:DD:EE:07");
    set_neighbor(&g_fake_snapshots[1].items[1], "192.168.1.8", "AA:BB:CC:DD:EE:07");
    g_fake_snapshots[1].count = 2;
    g_fake_snapshots[2] = g_fake_snapshots[1];
    start_discovery();

    LP_CHECK(poll_events() == 0);
    const size_t event_count = poll_events();
    LP_CHECK(event_count == 1);
    LP_CHECK(count_events(event_count, LP_DISCOVERY_EVENT_JOINED) == 1);
    LP_CHECK(g_discovery.known_count == 1);
    LP_CHECK(poll_events() == 0);
}

/* Verifies events beyond max_events are counted, and the count resets on the next poll. */
static void test_event_overflow_is_counted(void)
{
    reset_fakes();
    for (unsigned i = 0; i < 5; ++i) {
        char ip[LP_IP_STR_MAX];
        char mac[LP_MAC_STR_MAX];
        snprintf(ip, sizeof(ip), "192.168.2.%u", 10u + i);
        snprintf(mac, sizeof(mac), "AA:BB:CC:00:00:%02X", i);
        set_neighbor(&g_fake_snapshots[1].items[i], ip, mac);
    }
    g_fake_snapshots[1].count = 5;
    g_fake_snapshots[2] = g_fake_snapshots[1];
    start_discovery();

    lp_discovery_event_t events[2];
    size_t event_count = 0;
    LP_CHECK(lp_discovery_poll(&g_discovery, events, 2, &event_count) == LP_OK);
    LP_CHECK(event_count == 0);
    LP_CHECK(g_discovery.dropped_events == 0);

    LP_CHECK(lp_discovery_poll(&g_discovery, events, 2, &event_count) == LP_OK);
    LP_CHECK(event_count == 2);
    LP_CHECK(g_discovery.dropped_events == 3);
    /* The dropped neighbors are still stored, so they are not reported again. */
    LP_CHECK(g_discovery.known_count == 5);

    LP_CHECK(lp_discovery_poll(&g_discovery, events, 2, &event_count) == LP_OK);
    LP_CHECK(event_count == 0);
    LP_CHECK(g_discovery.dropped_events == 0);
}

/* Runs discovery reconciliation and provider-validation scenarios. */
int main(void)
{
    test_baseline_and_neighbor_changes();
    test_poll_rejects_missing_source();
    test_full_table_churn_evicts_departed_neighbors();
    test_rejoin_after_left_joins_once();
    test_mac_less_neighbor_uses_ip_identity();
    test_duplicate_mac_in_snapshot_joins_once();
    test_event_overflow_is_counted();
    LP_TEST_RETURN();
}