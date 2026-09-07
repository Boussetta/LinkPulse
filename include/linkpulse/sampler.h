#ifndef LINKPULSE_SAMPLER_H
#define LINKPULSE_SAMPLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linkpulse/net.h"
#include "linkpulse/status.h"

/* Capacity of the rolling sample history (feeds a future sparkline). */
#define LP_SAMPLER_HISTORY_CAP 300

typedef struct {
    uint64_t rx_bytes_per_sec;
    uint64_t tx_bytes_per_sec;
    uint64_t timestamp_ns;
} lp_rate_sample_t;

typedef enum {
    LP_IFACE_SELECT_AUTO = 0, /* interface owning the default route, re-resolved every poll */
    LP_IFACE_SELECT_MANUAL,   /* a fixed interface name */
    LP_IFACE_SELECT_ALL       /* sum of all matching interfaces */
} lp_iface_select_mode_t;

typedef struct {
    lp_iface_select_mode_t mode;
    char iface_name[LP_IFNAME_MAX]; /* used when mode == LP_IFACE_SELECT_MANUAL */
    bool include_virtual;           /* used when mode == LP_IFACE_SELECT_ALL */
} lp_sampler_config_t;

/* Injectable data sources, so the sampler is unit-testable without a real
   network. lp_sampler_init() leaves these unset (NULL); the caller must supply
   them via lp_sampler_set_sources() before the first poll -- typically with
   lp_net_snapshot / lp_net_default_iface / lp_clock_monotonic_ns for real use,
   or fakes in tests. */
typedef lp_status_t (*lp_net_snapshot_fn)(lp_iface_list_t *out);
typedef lp_status_t (*lp_net_default_iface_fn)(char *name_out, size_t name_cap);
typedef uint64_t (*lp_clock_fn)(void);

typedef struct {
    lp_net_snapshot_fn snapshot_fn;
    lp_net_default_iface_fn default_iface_fn;
    lp_clock_fn clock_fn;
} lp_sampler_sources_t;

typedef struct {
    lp_sampler_config_t config;
    lp_sampler_sources_t sources;

    bool has_baseline;
    uint64_t baseline_rx_bytes;
    uint64_t baseline_tx_bytes;
    uint64_t baseline_timestamp_ns;
    char active_iface[LP_IFNAME_MAX]; /* resolved target for AUTO/MANUAL; unused for ALL */

    lp_rate_sample_t history[LP_SAMPLER_HISTORY_CAP];
    size_t history_count;
    size_t history_head; /* index of the oldest sample */
} lp_sampler_t;

void lp_sampler_init(lp_sampler_t *sampler, const lp_sampler_config_t *config);

/* Overrides the data sources used when polling; a NULL field leaves that
   particular source unchanged (initially NULL after lp_sampler_init). */
void lp_sampler_set_sources(lp_sampler_t *sampler, const lp_sampler_sources_t *sources);

/* Takes one sample and appends it to the history.
   - LP_ERR_NOT_FOUND: the target interface (or default route) could not be resolved
     this round, e.g. the adapter is temporarily offline; keep polling.
   - The first successful poll (or the one right after an interface change or a
     counter reset) reports a rate of 0: a rate needs two samples. */
lp_status_t lp_sampler_poll(lp_sampler_t *sampler, lp_rate_sample_t *out);

/* Copies up to `cap` of the most recent samples (oldest first) into `out`. Returns
   the number of samples copied. */
size_t lp_sampler_history(const lp_sampler_t *sampler, lp_rate_sample_t *out, size_t cap);

#endif /* LINKPULSE_SAMPLER_H */
