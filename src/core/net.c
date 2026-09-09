#include "linkpulse/net.h"

#include <stdlib.h>
#include <string.h>

/* Releases the heap-owned adapter array and makes repeated cleanup harmless. */
void lp_iface_list_free(lp_iface_list_t *list)
{
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

/* Performs the small bounded linear lookup used by sampler target selection. */
const lp_iface_t *lp_iface_list_find(const lp_iface_list_t *list, const char *name)
{
    if (list == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i].name, name) == 0) {
            return &list->items[i];
        }
    }
    return NULL;
}
