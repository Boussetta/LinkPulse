#include "lp_test.h"

#include "linkpulse/sampler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* ---- table-driven fake data sources, shared by the scripted scenarios below ---- */

#define MAX_FAKE_STEPS 8
#define MAX_FAKE_IFACES 4

typedef struct {
    const char *name;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    bool is_loopback;
    bool is_virtual;
} fake_iface_t;

typedef struct {
    fake_iface_t ifaces[MAX_FAKE_IFACES];
    size_t iface_count;
} fake_snapshot_t;

static fake_snapshot_t g_fake_snapshots[MAX_FAKE_STEPS];
static size_t g_fake_snapshot_index;

static uint64_t g_fake_clock_values[MAX_FAKE_STEPS];
static size_t g_fake_clock_index;

static const char *g_fake_default_iface_values[MAX_FAKE_STEPS];
static lp_status_t g_fake_default_iface_status[MAX_FAKE_STEPS];
static size_t g_fake_default_iface_index;

static void reset_fakes(void)
{
    memset(g_fake_snapshots, 0, sizeof(g_fake_snapshots));
    g_fake_snapshot_index = 0;
    memset(g_fake_clock_values, 0, sizeof(g_fake_clock_values));
    g_fake_clock_index = 0;
    for (size_t i = 0; i < MAX_FAKE_STEPS; ++i) {
        g_fake_default_iface_values[i] = NULL;
        g_fake_default_iface_status[i] = LP_OK;
    }
    g_fake_default_iface_index = 0;
}

static lp_status_t fake_snapshot_fn(lp_iface_list_t *out)
{
    LP_CHECK(g_fake_snapshot_index < MAX_FAKE_STEPS);
    if (g_fake_snapshot_index >= MAX_FAKE_STEPS) {
        out->items = NULL;
        out->count = 0;
        return LP_ERR_IO;
    }
    const fake_snapshot_t *snap = &g_fake_snapshots[g_fake_snapshot_index];
    ++g_fake_snapshot_index;

    lp_iface_t *items = NULL;
    if (snap->iface_count > 0) {
        items = calloc(snap->iface_count, sizeof(*items));
        LP_CHECK(items != NULL);
        if (items == NULL) {
            return LP_ERR_NO_MEMORY;
        }
    }
    for (size_t i = 0; i < snap->iface_count; ++i) {
        snprintf(items[i].name, sizeof(items[i].name), "%s", snap->ifaces[i].name);
        items[i].rx_bytes = snap->ifaces[i].rx_bytes;
        items[i].tx_bytes = snap->ifaces[i].tx_bytes;
        items[i].is_loopback = snap->ifaces[i].is_loopback;
        items[i].is_virtual = snap->ifaces[i].is_virtual;
        items[i].is_up = true;
    }
    out->items = items;
    out->count = snap->iface_count;
    return LP_OK;
}

static uint64_t fake_clock_fn(void)
{
    LP_CHECK(g_fake_clock_index < MAX_FAKE_STEPS);
    if (g_fake_clock_index >= MAX_FAKE_STEPS) {
        return 0;
    }
    const uint64_t value = g_fake_clock_values[g_fake_clock_index];
    ++g_fake_clock_index;
    return value;
}

static lp_status_t fake_default_iface_fn(char *name_out, size_t name_cap)
{
    LP_CHECK(g_fake_default_iface_index < MAX_FAKE_STEPS);
    if (g_fake_default_iface_index >= MAX_FAKE_STEPS) {
        return LP_ERR_IO;
    }
    const lp_status_t status = g_fake_default_iface_status[g_fake_default_iface_index];
    const char *name = g_fake_default_iface_values[g_fake_default_iface_index];
    ++g_fake_default_iface_index;
    if (status != LP_OK) {
        return status;
    }
    LP_CHECK(name != NULL);
    if (name == NULL) {
        return LP_ERR_IO;
    }
    snprintf(name_out, name_cap, "%s", name);
    return LP_OK;
}

static void install_fakes(lp_sampler_t *sampler)
{
    const lp_sampler_sources_t sources = {fake_snapshot_fn, fake_default_iface_fn, fake_clock_fn};
    lp_sampler_set_sources(sampler, &sources);
}

/* ---- scenarios ---- */

static void test_manual_basic_rate(void)
{
    reset_fakes();
    g_fake_snapshots[0] = (fake_snapshot_t){{{"eth0", 1000, 500, false, false}}, 1};
    g_fake_clock_values[0] = 0;
    g_fake_snapshots[1] = (fake_snapshot_t){{{"eth0", 1001000, 500500, false, false}}, 1};
    g_fake_clock_values[1] = 1000000000ULL; /* 1 second later */

    lp_sampler_t sampler;
    lp_sampler_config_t config = {LP_IFACE_SELECT_MANUAL, "eth0", false};
    lp_sampler_init(&sampler, &config);
    install_fakes(&sampler);

    lp_rate_sample_t sample;
    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    LP_CHECK(sample.rx_bytes_per_sec == 0); /* no rate on the first sample */
    LP_CHECK(sample.tx_bytes_per_sec == 0);

    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    LP_CHECK(sample.rx_bytes_per_sec == 1000000);
    LP_CHECK(sample.tx_bytes_per_sec == 500000);
}

static void test_counter_reset_yields_zero_then_resumes(void)
{
    reset_fakes();
    g_fake_snapshots[0] = (fake_snapshot_t){{{"eth0", 5000, 1000, false, false}}, 1};
    g_fake_clock_values[0] = 0;
    /* Adapter replaced/reset: counters drop below the baseline. */
    g_fake_snapshots[1] = (fake_snapshot_t){{{"eth0", 100, 50, false, false}}, 1};
    g_fake_clock_values[1] = 1000000000ULL;
    g_fake_snapshots[2] = (fake_snapshot_t){{{"eth0", 2100, 1050, false, false}}, 1};
    g_fake_clock_values[2] = 2000000000ULL;

    lp_sampler_t sampler;
    lp_sampler_config_t config = {LP_IFACE_SELECT_MANUAL, "eth0", false};
    lp_sampler_init(&sampler, &config);
    install_fakes(&sampler);

    lp_rate_sample_t sample;
    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);

    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    LP_CHECK(sample.rx_bytes_per_sec ==
             0); /* reset absorbed, not a huge negative-turned-huge rate */
    LP_CHECK(sample.tx_bytes_per_sec == 0);

    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    LP_CHECK(sample.rx_bytes_per_sec == 2000);
    LP_CHECK(sample.tx_bytes_per_sec == 1000);
}

static void test_all_mode_excludes_loopback_and_virtual(void)
{
    reset_fakes();
    g_fake_snapshots[0] = (fake_snapshot_t){{
                                                {"lo", 10, 10, true, false},
                                                {"eth0", 100, 50, false, false},
                                                {"vEthernet", 999, 999, false, true},
                                            },
                                            3};
    g_fake_clock_values[0] = 0;
    g_fake_snapshots[1] = (fake_snapshot_t){{
                                                {"lo", 10, 10, true, false},
                                                {"eth0", 2100, 1050, false, false},
                                                {"vEthernet", 999, 999, false, true},
                                            },
                                            3};
    g_fake_clock_values[1] = 1000000000ULL;

    lp_sampler_t sampler;
    lp_sampler_config_t config = {LP_IFACE_SELECT_ALL, "", false};
    lp_sampler_init(&sampler, &config);
    install_fakes(&sampler);

    lp_rate_sample_t sample;
    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    LP_CHECK(sample.rx_bytes_per_sec == 2000);
    LP_CHECK(sample.tx_bytes_per_sec == 1000);
}

static void test_all_mode_new_iface_does_not_mask_existing_traffic(void)
{
    reset_fakes();
    /* Only eth0 present initially. */
    g_fake_snapshots[0] = (fake_snapshot_t){{{"eth0", 100, 50, false, false}}, 1};
    g_fake_clock_values[0] = 0;
    /* A second adapter appears mid-run, bringing its own pre-existing counters.
       eth0's real traffic must still be counted; only eth1 (brand new, no
       baseline yet) should contribute 0 this round. */
    g_fake_snapshots[1] = (fake_snapshot_t){{
                                                {"eth0", 150, 75, false, false},
                                                {"eth1", 500000, 250000, false, false},
                                            },
                                            2};
    g_fake_clock_values[1] = 1000000000ULL;
    g_fake_snapshots[2] = (fake_snapshot_t){{
                                                {"eth0", 250, 175, false, false},
                                                {"eth1", 501000, 250500, false, false},
                                            },
                                            2};
    g_fake_clock_values[2] = 2000000000ULL;

    lp_sampler_t sampler;
    lp_sampler_config_t config = {LP_IFACE_SELECT_ALL, "", false};
    lp_sampler_init(&sampler, &config);
    install_fakes(&sampler);

    lp_rate_sample_t sample;
    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);

    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    LP_CHECK(sample.rx_bytes_per_sec == 50); /* eth0's real delta; eth1 is new, contributes 0 */
    LP_CHECK(sample.tx_bytes_per_sec == 25);

    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    LP_CHECK(sample.rx_bytes_per_sec == 1100); /* (100+1000) bytes over 1s, both now tracked */
    LP_CHECK(sample.tx_bytes_per_sec == 600);  /* (100+500) bytes over 1s */
}

static void test_all_mode_one_iface_reset_resets_aggregate_for_poll(void)
{
    reset_fakes();
    g_fake_snapshots[0] = (fake_snapshot_t){{
                                                {"eth0", 5000, 1000, false, false},
                                                {"eth1", 100, 100, false, false},
                                            },
                                            2};
    g_fake_clock_values[0] = 0;
    /* eth0's counters reset (adapter replaced); eth1 keeps growing normally.
       A single combined-sum baseline could let eth1's increase mask eth0's
       reset (or vice versa); each must be judged on its own counters. */
    g_fake_snapshots[1] = (fake_snapshot_t){{
                                                {"eth0", 50, 20, false, false},
                                                {"eth1", 1100, 600, false, false},
                                            },
                                            2};
    g_fake_clock_values[1] = 1000000000ULL;
    g_fake_snapshots[2] = (fake_snapshot_t){{
                                                {"eth0", 250, 120, false, false},
                                                {"eth1", 2100, 1100, false, false},
                                            },
                                            2};
    g_fake_clock_values[2] = 2000000000ULL;

    lp_sampler_t sampler;
    lp_sampler_config_t config = {LP_IFACE_SELECT_ALL, "", false};
    lp_sampler_init(&sampler, &config);
    install_fakes(&sampler);

    lp_rate_sample_t sample;
    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);

    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    LP_CHECK(sample.rx_bytes_per_sec == 0);
    LP_CHECK(sample.tx_bytes_per_sec == 0);

    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    LP_CHECK(sample.rx_bytes_per_sec == 1200);
    LP_CHECK(sample.tx_bytes_per_sec == 600);
}

static void test_auto_mode_resets_baseline_on_iface_change(void)
{
    reset_fakes();
    g_fake_default_iface_values[0] = "wifi";
    g_fake_snapshots[0] = (fake_snapshot_t){{{"wifi", 1000, 1000, false, false}}, 1};
    g_fake_clock_values[0] = 0;

    g_fake_default_iface_values[1] = "eth0"; /* default route switched adapters */
    g_fake_snapshots[1] = (fake_snapshot_t){{{"eth0", 50, 50, false, false}}, 1};
    g_fake_clock_values[1] = 1000000000ULL;

    g_fake_default_iface_values[2] = "eth0";
    g_fake_snapshots[2] = (fake_snapshot_t){{{"eth0", 3050, 1550, false, false}}, 1};
    g_fake_clock_values[2] = 2000000000ULL;

    lp_sampler_t sampler;
    lp_sampler_config_t config = {LP_IFACE_SELECT_AUTO, "", false};
    lp_sampler_init(&sampler, &config);
    install_fakes(&sampler);

    lp_rate_sample_t sample;
    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);

    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    LP_CHECK(sample.rx_bytes_per_sec == 0); /* interface changed: fresh baseline, no rate yet */
    LP_CHECK(sample.tx_bytes_per_sec == 0);

    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    LP_CHECK(sample.rx_bytes_per_sec == 3000);
    LP_CHECK(sample.tx_bytes_per_sec == 1500);
}

static void test_auto_mode_propagates_default_route_failure(void)
{
    reset_fakes();
    g_fake_default_iface_status[0] = LP_ERR_NOT_FOUND;
    g_fake_snapshots[0] = (fake_snapshot_t){{{0}}, 0}; /* not consulted before the failure */
    g_fake_clock_values[0] = 0;

    lp_sampler_t sampler;
    lp_sampler_config_t config = {LP_IFACE_SELECT_AUTO, "", false};
    lp_sampler_init(&sampler, &config);
    install_fakes(&sampler);

    lp_rate_sample_t sample;
    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_ERR_NOT_FOUND);
}

static void test_manual_mode_missing_interface_is_not_found(void)
{
    reset_fakes();
    g_fake_snapshots[0] = (fake_snapshot_t){{{"eth0", 100, 100, false, false}}, 1};
    g_fake_clock_values[0] = 0;

    lp_sampler_t sampler;
    lp_sampler_config_t config = {LP_IFACE_SELECT_MANUAL, "wifi", false}; /* not present */
    lp_sampler_init(&sampler, &config);
    install_fakes(&sampler);

    lp_rate_sample_t sample;
    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_ERR_NOT_FOUND);
}

static void test_poll_rejects_missing_sources(void)
{
    reset_fakes();

    lp_sampler_t sampler;
    lp_sampler_config_t config = {LP_IFACE_SELECT_MANUAL, "eth0", false};
    lp_sampler_init(&sampler, &config); /* sources left unset */

    lp_rate_sample_t sample;
    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_ERR_INVALID_ARG);
}

static void test_history_rejects_null_and_zero_cap(void)
{
    reset_fakes();
    g_fake_snapshots[0] = (fake_snapshot_t){{{"eth0", 100, 100, false, false}}, 1};
    g_fake_clock_values[0] = 0;

    lp_sampler_t sampler;
    lp_sampler_config_t config = {LP_IFACE_SELECT_MANUAL, "eth0", false};
    lp_sampler_init(&sampler, &config);
    install_fakes(&sampler);

    lp_rate_sample_t sample;
    LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);

    lp_rate_sample_t buffer[1];
    LP_CHECK(lp_sampler_history(&sampler, buffer, 0) == 0);
    LP_CHECK(lp_sampler_history(&sampler, NULL, 1) == 0);
    LP_CHECK(lp_sampler_history(NULL, buffer, 1) == 0);
}

/* ---- history ring buffer: procedural fakes, independent of the table above ---- */

static uint64_t g_hist_clock_ns;
static uint64_t g_hist_rx_bytes;

static lp_status_t hist_snapshot_fn(lp_iface_list_t *out)
{
    lp_iface_t *items = calloc(1, sizeof(*items));
    LP_CHECK(items != NULL);
    if (items == NULL) {
        return LP_ERR_NO_MEMORY;
    }
    snprintf(items[0].name, sizeof(items[0].name), "eth0");
    items[0].rx_bytes = g_hist_rx_bytes;
    items[0].tx_bytes = g_hist_rx_bytes;
    items[0].is_up = true;
    g_hist_rx_bytes += 100;

    out->items = items;
    out->count = 1;
    return LP_OK;
}

static uint64_t hist_clock_fn(void)
{
    const uint64_t value = g_hist_clock_ns;
    g_hist_clock_ns += 1000000000ULL;
    return value;
}

static void test_history_wraps_at_capacity(void)
{
    g_hist_clock_ns = 0;
    g_hist_rx_bytes = 0;

    lp_sampler_t sampler;
    lp_sampler_config_t config = {LP_IFACE_SELECT_MANUAL, "eth0", false};
    lp_sampler_init(&sampler, &config);
    const lp_sampler_sources_t sources = {hist_snapshot_fn, NULL, hist_clock_fn};
    lp_sampler_set_sources(&sampler, &sources);

    const size_t total_polls = LP_SAMPLER_HISTORY_CAP + 5;
    lp_rate_sample_t sample;
    for (size_t i = 0; i < total_polls; ++i) {
        LP_CHECK(lp_sampler_poll(&sampler, &sample) == LP_OK);
    }

    lp_rate_sample_t history[LP_SAMPLER_HISTORY_CAP];
    const size_t count = lp_sampler_history(&sampler, history, LP_SAMPLER_HISTORY_CAP);
    LP_CHECK(count == LP_SAMPLER_HISTORY_CAP);
    LP_CHECK(history[count - 1].timestamp_ns == (total_polls - 1) * 1000000000ULL);
    for (size_t i = 1; i < count; ++i) {
        LP_CHECK(history[i].timestamp_ns > history[i - 1].timestamp_ns);
    }

    lp_rate_sample_t partial[3];
    LP_CHECK(lp_sampler_history(&sampler, partial, 3) == 3);
    /* A partial request must return the most recent samples, not the oldest. */
    LP_CHECK(partial[2].timestamp_ns == history[count - 1].timestamp_ns);
    LP_CHECK(partial[1].timestamp_ns == history[count - 2].timestamp_ns);
    LP_CHECK(partial[0].timestamp_ns == history[count - 3].timestamp_ns);
}

int main(void)
{
    test_manual_basic_rate();
    test_counter_reset_yields_zero_then_resumes();
    test_all_mode_excludes_loopback_and_virtual();
    test_all_mode_new_iface_does_not_mask_existing_traffic();
    test_all_mode_one_iface_reset_resets_aggregate_for_poll();
    test_auto_mode_resets_baseline_on_iface_change();
    test_auto_mode_propagates_default_route_failure();
    test_manual_mode_missing_interface_is_not_found();
    test_poll_rejects_missing_sources();
    test_history_rejects_null_and_zero_cap();
    test_history_wraps_at_capacity();
    LP_TEST_RETURN();
}
