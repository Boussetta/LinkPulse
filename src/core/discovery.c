#include "linkpulse/discovery.h"

#include <string.h>

void lp_discovery_init(lp_discovery_t *discovery)
{
    if (discovery == NULL) {
        return;
    }
    memset(discovery, 0, sizeof(*discovery));
}

void lp_discovery_set_sources(lp_discovery_t *discovery, const lp_discovery_sources_t *sources)
{
    if (discovery == NULL || sources == NULL) {
        return;
    }
    if (sources->snapshot_fn != NULL) {
        discovery->sources.snapshot_fn = sources->snapshot_fn;
    }
    if (sources->local_networks_fn != NULL) {
        discovery->sources.local_networks_fn = sources->local_networks_fn;
    }
}

/* Identifies a neighbour by MAC when both sides have one, falling back to IP
   otherwise (some IPv6 neighbour states can be reported without a MAC). */
static bool neighbors_match(const lp_neighbor_t *a, const lp_neighbor_t *b)
{
    if (a->mac[0] != '\0' && b->mac[0] != '\0') {
        return strcmp(a->mac, b->mac) == 0;
    }
    return strcmp(a->ip, b->ip) == 0;
}

static long find_known_index(const lp_neighbor_t *known, size_t known_count,
                             const lp_neighbor_t *item)
{
    for (size_t i = 0; i < known_count; ++i) {
        if (neighbors_match(&known[i], item)) {
            return (long)i;
        }
    }
    return -1;
}

lp_status_t lp_discovery_poll(lp_discovery_t *discovery, lp_discovery_event_t *events,
                              size_t max_events, size_t *event_count)
{
    if (discovery == NULL || events == NULL || event_count == NULL) {
        return LP_ERR_INVALID_ARG;
    }
    *event_count = 0;
    if (discovery->sources.snapshot_fn == NULL) {
        return LP_ERR_UNSUPPORTED;
    }

    lp_neighbor_list_t current;
    const lp_status_t status = discovery->sources.snapshot_fn(&current);
    if (status != LP_OK) {
        return status;
    }

    const size_t old_known_count = discovery->known_count;
    bool seen[LP_DISCOVERY_MAX_NEIGHBORS] = {0};
    size_t emitted = 0;

    for (size_t i = 0; i < current.count; ++i) {
        const long idx = find_known_index(discovery->known, old_known_count, &current.items[i]);
        if (idx >= 0) {
            seen[idx] = true;
            discovery->known[idx] = current.items[i];
            continue;
        }
        if (discovery->has_baseline && emitted < max_events) {
            events[emitted].type = LP_DISCOVERY_EVENT_JOINED;
            events[emitted].neighbor = current.items[i];
            ++emitted;
        }
        if (discovery->known_count < LP_DISCOVERY_MAX_NEIGHBORS) {
            discovery->known[discovery->known_count++] = current.items[i];
        }
    }

    /* Compact the pre-existing region in place (write index never exceeds the
       read index), then slide the newly appended entries down to follow it. */
    size_t write = 0;
    for (size_t i = 0; i < old_known_count; ++i) {
        if (seen[i]) {
            discovery->known[write++] = discovery->known[i];
        } else if (discovery->has_baseline && emitted < max_events) {
            events[emitted].type = LP_DISCOVERY_EVENT_LEFT;
            events[emitted].neighbor = discovery->known[i];
            ++emitted;
        }
    }
    for (size_t i = old_known_count; i < discovery->known_count; ++i) {
        discovery->known[write++] = discovery->known[i];
    }
    discovery->known_count = write;
    discovery->has_baseline = true;
    *event_count = emitted;
    return LP_OK;
}
