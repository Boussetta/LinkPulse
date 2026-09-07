#include "linkpulse/sampler.h"

#include <stdio.h>
#include <string.h>

void lp_sampler_init(lp_sampler_t *sampler, const lp_sampler_config_t *config)
{
    if (sampler == NULL) {
        return;
    }
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
    if (sampler == NULL || sources == NULL) {
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

static lp_sampler_target_t *find_target(lp_sampler_t *sampler, const char *name)
{
    for (size_t i = 0; i < sampler->target_count; ++i) {
        if (strcmp(sampler->targets[i].name, name) == 0) {
            return &sampler->targets[i];
        }
    }
    return NULL;
}

/* Accumulates this interface's byte delta since its own last poll into
   *delta_rx / *delta_tx (added, not overwritten, so callers can sum several
   interfaces). Tracking each interface independently -- rather than diffing
   one aggregate sum -- means one interface's counter reset, or its
   appearance/disappearance, can never be masked by another interface's
   traffic (which a single combined baseline cannot distinguish). A brand-new
   interface, or one whose counters just decreased (reset/replaced adapter),
   contributes 0 for this poll only. */
static void accumulate_target_delta(lp_sampler_t *sampler, const char *name, uint64_t current_rx,
                                    uint64_t current_tx, uint64_t *delta_rx, uint64_t *delta_tx,
                                    bool *counter_went_backwards)
{
    lp_sampler_target_t *entry = find_target(sampler, name);
    if (entry == NULL) {
        if (sampler->target_count >= LP_SAMPLER_MAX_TRACKED_IFACES) {
            return; /* table full: can't track this one's rate this poll */
        }
        entry = &sampler->targets[sampler->target_count++];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->name, sizeof(entry->name), "%s", name);
    }

    entry->seen_this_poll = true;
    if (entry->initialized) {
        if (current_rx < entry->rx_bytes || current_tx < entry->tx_bytes) {
            *counter_went_backwards = true;
        }
        if (current_rx >= entry->rx_bytes) *delta_rx += current_rx - entry->rx_bytes;
        if (current_tx >= entry->tx_bytes) *delta_tx += current_tx - entry->tx_bytes;
    }
    entry->rx_bytes = current_rx;
    entry->tx_bytes = current_tx;
    entry->initialized = true;
}

/* Drops tracked interfaces not seen this poll (vanished, or -- for AUTO/MANUAL,
   whose single target is renewed as a fresh entry -- superseded by a new one),
   and clears the scratch flag for survivors ahead of the next poll. */
static void evict_untracked_targets(lp_sampler_t *sampler)
{
    size_t write = 0;
    for (size_t i = 0; i < sampler->target_count; ++i) {
        if (!sampler->targets[i].seen_this_poll) {
            continue;
        }
        if (write != i) {
            sampler->targets[write] = sampler->targets[i];
        }
        sampler->targets[write].seen_this_poll = false;
        ++write;
    }
    sampler->target_count = write;
}

/* Resolves the target interface(s) for this poll and accumulates their byte
   deltas since each one's own last poll. Returns LP_ERR_NOT_FOUND (or whatever
   status the AUTO default-route lookup returned) if a required single
   interface could not be located this round; LP_OK for LP_IFACE_SELECT_ALL
   regardless of how many interfaces matched, including zero. */
static lp_status_t resolve_deltas(lp_sampler_t *sampler, const lp_iface_list_t *list,
                                  uint64_t *delta_rx, uint64_t *delta_tx)
{
    *delta_rx = 0;
    *delta_tx = 0;

    if (sampler->config.mode == LP_IFACE_SELECT_ALL) {
        bool any_counter_went_backwards = false;
        for (size_t i = 0; i < list->count; ++i) {
            const lp_iface_t *iface = &list->items[i];
            if (iface->is_loopback) {
                continue;
            }
            if (iface->is_virtual && !sampler->config.include_virtual) {
                continue;
            }
            accumulate_target_delta(sampler, iface->name, iface->rx_bytes, iface->tx_bytes,
                                    delta_rx, delta_tx, &any_counter_went_backwards);
        }
        if (any_counter_went_backwards) {
            *delta_rx = 0;
            *delta_tx = 0;
        }
        evict_untracked_targets(sampler);
        return LP_OK;
    }

    char target[LP_IFNAME_MAX];
    if (sampler->config.mode == LP_IFACE_SELECT_MANUAL) {
        strncpy(target, sampler->config.iface_name, sizeof(target));
        target[sizeof(target) - 1] = '\0';
    } else {
        const lp_status_t st = sampler->sources.default_iface_fn(target, sizeof(target));
        if (st != LP_OK) {
            return st;
        }
        target[sizeof(target) - 1] = '\0';
    }

    const lp_iface_t *found = lp_iface_list_find(list, target);
    if (found == NULL) {
        return LP_ERR_NOT_FOUND;
    }

    bool counter_went_backwards = false;
    accumulate_target_delta(sampler, target, found->rx_bytes, found->tx_bytes, delta_rx, delta_tx,
                            &counter_went_backwards);
    evict_untracked_targets(sampler);
    return LP_OK;
}

lp_status_t lp_sampler_poll(lp_sampler_t *sampler, lp_rate_sample_t *out)
{
    if (sampler == NULL || out == NULL || sampler->sources.snapshot_fn == NULL ||
        sampler->sources.clock_fn == NULL ||
        (sampler->config.mode == LP_IFACE_SELECT_AUTO &&
         sampler->sources.default_iface_fn == NULL)) {
        return LP_ERR_INVALID_ARG;
    }

    lp_iface_list_t list;
    const lp_status_t snapshot_status = sampler->sources.snapshot_fn(&list);
    if (snapshot_status != LP_OK) {
        return snapshot_status;
    }

    uint64_t delta_rx = 0;
    uint64_t delta_tx = 0;
    const lp_status_t resolve_status = resolve_deltas(sampler, &list, &delta_rx, &delta_tx);
    lp_iface_list_free(&list);

    if (resolve_status != LP_OK) {
        /* Not found this round: every previously tracked target is stale, so drop
           them all rather than let a future comeback diff against ancient counters. */
        sampler->target_count = 0;
        return resolve_status;
    }

    const uint64_t now = sampler->sources.clock_fn();
    const uint64_t elapsed_ns = now - sampler->last_poll_timestamp_ns;

    const lp_rate_sample_t sample = {bytes_per_sec(delta_rx, elapsed_ns),
                                     bytes_per_sec(delta_tx, elapsed_ns), now};
    sampler->last_poll_timestamp_ns = now;

    push_history(sampler, &sample);
    *out = sample;
    return LP_OK;
}

size_t lp_sampler_history(const lp_sampler_t *sampler, lp_rate_sample_t *out, size_t cap)
{
    if (sampler == NULL || out == NULL || cap == 0) {
        return 0;
    }

    const size_t count = (sampler->history_count < cap) ? sampler->history_count : cap;
    const size_t start =
        (sampler->history_head + (sampler->history_count - count)) % LP_SAMPLER_HISTORY_CAP;

    for (size_t i = 0; i < count; ++i) {
        const size_t slot = (start + i) % LP_SAMPLER_HISTORY_CAP;
        out[i] = sampler->history[slot];
    }
    return count;
}
