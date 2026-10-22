#include "linkpulse/discovery.h"

#include <string.h>

/* Clears retained neighbor identities so the next successful poll establishes a baseline. */
void lp_discovery_init(lp_discovery_t *discovery)
{
    if (discovery == NULL) {
        return;
    }
    memset(discovery, 0, sizeof(*discovery));
}

/* Wires platform or test providers without discarding retained discovery state. */
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
/* Matches stable MAC identities first, falling back to IP for incomplete entries. */
static bool neighbors_match(const lp_neighbor_t *a, const lp_neighbor_t *b)
{
    if (a->mac[0] != '\0' && b->mac[0] != '\0') {
        return strcmp(a->mac, b->mac) == 0;
    }
    return strcmp(a->ip, b->ip) == 0;
}

/* Finds an existing neighbor in the fixed-capacity retained baseline. */
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

/* Removes one retained entry, keeping the remaining entries in insertion order. */
static void remove_known_at(lp_discovery_t *discovery, size_t index)
{
    const size_t tail = discovery->known_count - index - 1;
    memmove(&discovery->known[index], &discovery->known[index + 1],
            tail * sizeof(discovery->known[0]));
    memmove(&discovery->missing_polls[index], &discovery->missing_polls[index + 1],
            tail * sizeof(discovery->missing_polls[0]));
    --discovery->known_count;
}

/* Makes room for one new neighbor by evicting the longest-retained departed entry. */
static bool reserve_known_slot(lp_discovery_t *discovery)
{
    if (discovery->known_count < LP_DISCOVERY_MAX_NEIGHBORS) {
        return true;
    }
    for (size_t i = 0; i < discovery->known_count; ++i) {
        if (!discovery->known[i].active) {
            remove_known_at(discovery, i);
            return true;
        }
    }
    return false;
}

/* Appends one event when the caller-provided buffer still has room, else counts it as dropped. */
static void emit_event(lp_discovery_t *discovery, lp_discovery_event_t *events, size_t max_events,
                       size_t *emitted, lp_discovery_event_type_t type,
                       const lp_neighbor_t *neighbor)
{
    if (*emitted >= max_events) {
        ++discovery->dropped_events;
        return;
    }
    events[*emitted].type = type;
    events[*emitted].neighbor = *neighbor;
    ++*emitted;
}

/* Reconciles one platform snapshot with the baseline and emits bounded events. */
lp_status_t lp_discovery_poll(lp_discovery_t *discovery, lp_discovery_event_t *events,
                              size_t max_events, size_t *event_count)
{
    if (discovery == NULL || events == NULL || event_count == NULL) {
        return LP_ERR_INVALID_ARG;
    }
    *event_count = 0;
    discovery->dropped_events = 0;
    if (discovery->sources.snapshot_fn == NULL) {
        return LP_ERR_UNSUPPORTED;
    }

    /* Kept in the discovery state: a full snapshot is ~180 KB, too large for a thread stack. */
    lp_neighbor_list_t *current = &discovery->scratch;
    const lp_status_t status = discovery->sources.snapshot_fn(current);
    if (status != LP_OK) {
        return status;
    }

    const size_t old_known_count = discovery->known_count;
    bool seen[LP_DISCOVERY_MAX_NEIGHBORS] = {0};
    size_t new_items[LP_DISCOVERY_MAX_NEIGHBORS];
    size_t new_count = 0;
    size_t emitted = 0;

    /* Match the snapshot against the retained baseline. Unmatched neighbors are
       collected once each, so an identity duplicated in one snapshot joins once. */
    for (size_t i = 0; i < current->count; ++i) {
        current->items[i].active = true;
        const long idx = find_known_index(discovery->known, old_known_count, &current->items[i]);
        if (idx >= 0) {
            seen[idx] = true;
            if (discovery->has_baseline && !discovery->known[idx].active) {
                emit_event(discovery, events, max_events, &emitted, LP_DISCOVERY_EVENT_JOINED,
                           &current->items[i]);
            }
            discovery->known[idx] = current->items[i];
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < new_count && !duplicate; ++j) {
            duplicate = neighbors_match(&current->items[new_items[j]], &current->items[i]);
        }
        if (!duplicate) {
            new_items[new_count++] = i;
        }
    }

    /* Compact the retained region in place (write index never exceeds the read
       index). Missing neighbors leave after a grace period and stay retained as
       inactive entries, so a later return is reported as a join. */
    size_t write = 0;
    for (size_t i = 0; i < old_known_count; ++i) {
        uint8_t missing = 0;
        if (!seen[i]) {
            missing = discovery->missing_polls[i];
            if (missing + 1 < LP_DISCOVERY_MISSING_POLLS_BEFORE_LEFT) {
                ++missing;
            } else {
                if (discovery->known[i].active && discovery->has_baseline) {
                    emit_event(discovery, events, max_events, &emitted, LP_DISCOVERY_EVENT_LEFT,
                               &discovery->known[i]);
                }
                discovery->known[i].active = false;
            }
        }
        discovery->known[write] = discovery->known[i];
        discovery->missing_polls[write] = missing;
        ++write;
    }
    discovery->known_count = write;

    /* Store new neighbors, evicting departed entries when the table is full. A
       neighbor that cannot be stored is not reported either; otherwise it would
       be reported as joined again on every poll. */
    for (size_t i = 0; i < new_count; ++i) {
        if (!reserve_known_slot(discovery)) {
            break;
        }
        const lp_neighbor_t *neighbor = &current->items[new_items[i]];
        discovery->known[discovery->known_count] = *neighbor;
        discovery->missing_polls[discovery->known_count] = 0;
        ++discovery->known_count;
        if (discovery->has_baseline) {
            emit_event(discovery, events, max_events, &emitted, LP_DISCOVERY_EVENT_JOINED,
                       neighbor);
        }
    }
    discovery->has_baseline = true;
    *event_count = emitted;
    return LP_OK;
}
