#include "linkpulse/sampler.h"

#include <string.h>

void lp_sampler_init(lp_sampler_t *sampler, const lp_sampler_config_t *config)
{
    memset(sampler, 0, sizeof(*sampler));
    if (config != NULL) {
        sampler->config = *config;
    }
    /* Sources are left unset: core must never reference platform-implemented
       functions directly, or lp_core would gain a link-time dependency on
       lp_platform (and GNU ld's single-pass archive resolution would then fail,
       since lp_platform is linked before lp_core). The caller wires the real
       functions via lp_sampler_set_sources(). */
}

void lp_sampler_set_sources(lp_sampler_t *sampler, const lp_sampler_sources_t *sources)
{
    if (sources == NULL) {
        return;
    }
    if (sources->snapshot_fn != NULL) {
        sampler->sources.snapshot_fn = sources->snapshot_fn;
    }
    if (sources->default_iface_fn != NULL) {
        sampler->sources.default_iface_fn = sources->default_iface_fn;
    }
    if (sources->clock_fn != NULL) {
        sampler->sources.clock_fn = sources->clock_fn;
    }
}

static void reset_baseline(lp_sampler_t *sampler)
{
    sampler->has_baseline = false;
    sampler->baseline_rx_bytes = 0;
    sampler->baseline_tx_bytes = 0;
    sampler->baseline_timestamp_ns = 0;
}

static void push_history(lp_sampler_t *sampler, const lp_rate_sample_t *sample)
{
    const size_t next_slot =
        (sampler->history_head + sampler->history_count) % LP_SAMPLER_HISTORY_CAP;
    sampler->history[next_slot] = *sample;
    if (sampler->history_count < LP_SAMPLER_HISTORY_CAP) {
        ++sampler->history_count;
    } else {
        /* Full: the slot we just overwrote was the oldest, so it moves forward. */
        sampler->history_head = (sampler->history_head + 1) % LP_SAMPLER_HISTORY_CAP;
    }
}

static uint64_t bytes_per_sec(uint64_t delta_bytes, uint64_t elapsed_ns)
{
    if (elapsed_ns == 0) {
        return 0;
    }
    const double rate = (double)delta_bytes * 1e9 / (double)elapsed_ns;
    return (uint64_t)(rate + 0.5);
}

/* Resolves the target interface(s) for this poll and sums their counters.
   Returns LP_ERR_NOT_FOUND if a required single interface could not be located. */
static lp_status_t resolve_totals(lp_sampler_t *sampler, const lp_iface_list_t *list,
                                  uint64_t *total_rx, uint64_t *total_tx, bool *iface_changed)
{
    *total_rx = 0;
    *total_tx = 0;
    *iface_changed = false;

    if (sampler->config.mode == LP_IFACE_SELECT_ALL) {
        for (size_t i = 0; i < list->count; ++i) {
            const lp_iface_t *iface = &list->items[i];
            if (iface->is_loopback) {
                continue;
            }
            if (iface->is_virtual && !sampler->config.include_virtual) {
                continue;
            }
            *total_rx += iface->rx_bytes;
            *total_tx += iface->tx_bytes;
        }
        return LP_OK;
    }

    char target[LP_IFNAME_MAX];
    if (sampler->config.mode == LP_IFACE_SELECT_MANUAL) {
        memcpy(target, sampler->config.iface_name, sizeof(target));
    } else {
        if (sampler->sources.default_iface_fn(target, sizeof(target)) != LP_OK) {
            return LP_ERR_NOT_FOUND;
        }
    }

    if (strcmp(target, sampler->active_iface) != 0) {
        *iface_changed = true;
        memcpy(sampler->active_iface, target, sizeof(sampler->active_iface));
    }

    const lp_iface_t *found = lp_iface_list_find(list, target);
    if (found == NULL) {
        return LP_ERR_NOT_FOUND;
    }

    *total_rx = found->rx_bytes;
    *total_tx = found->tx_bytes;
    return LP_OK;
}

lp_status_t lp_sampler_poll(lp_sampler_t *sampler, lp_rate_sample_t *out)
{
    lp_iface_list_t list;
    const lp_status_t snapshot_status = sampler->sources.snapshot_fn(&list);
    if (snapshot_status != LP_OK) {
        return snapshot_status;
    }

    uint64_t total_rx = 0;
    uint64_t total_tx = 0;
    bool iface_changed = false;
    const lp_status_t resolve_status =
        resolve_totals(sampler, &list, &total_rx, &total_tx, &iface_changed);
    lp_iface_list_free(&list);

    if (resolve_status != LP_OK) {
        reset_baseline(sampler);
        return resolve_status;
    }

    const uint64_t now = sampler->sources.clock_fn();

    /* A fresh start: first poll ever, the active interface just changed, or the
       counters went backwards (adapter reset/replaced). No rate yet this round. */
    const bool counters_went_backwards =
        sampler->has_baseline &&
        (total_rx < sampler->baseline_rx_bytes || total_tx < sampler->baseline_tx_bytes);

    lp_rate_sample_t sample = {0, 0, now};
    if (!sampler->has_baseline || iface_changed || counters_went_backwards) {
        sampler->has_baseline = true;
    } else {
        const uint64_t elapsed_ns = now - sampler->baseline_timestamp_ns;
        sample.rx_bytes_per_sec = bytes_per_sec(total_rx - sampler->baseline_rx_bytes, elapsed_ns);
        sample.tx_bytes_per_sec = bytes_per_sec(total_tx - sampler->baseline_tx_bytes, elapsed_ns);
    }

    sampler->baseline_rx_bytes = total_rx;
    sampler->baseline_tx_bytes = total_tx;
    sampler->baseline_timestamp_ns = now;

    push_history(sampler, &sample);
    *out = sample;
    return LP_OK;
}

size_t lp_sampler_history(const lp_sampler_t *sampler, lp_rate_sample_t *out, size_t cap)
{
    const size_t count = (sampler->history_count < cap) ? sampler->history_count : cap;
    for (size_t i = 0; i < count; ++i) {
        const size_t slot = (sampler->history_head + i) % LP_SAMPLER_HISTORY_CAP;
        out[i] = sampler->history[slot];
    }
    return count;
}
