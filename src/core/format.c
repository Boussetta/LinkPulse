#include "linkpulse/format.h"

#include <stdio.h>

void lp_format_rate(uint64_t bytes_per_sec, bool use_bits, char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0) {
        return;
    }

    double value = (double)bytes_per_sec;
    const char *unit_base = "B";
    if (use_bits) {
        value *= 8.0;
        unit_base = "b";
    }

    static const char *const prefixes[] = {"", "K", "M", "G", "T"};
    size_t prefix_index = 0;
    /* 999.95 rather than 1000.0: anything that would round to "1000.0" at one
       decimal must roll over to the next prefix instead (e.g. "1.0 MB/s", not
       "1000.0 KB/s"). The base unit always holds a whole number, so this never
       misfires there. */
    while (value >= 999.95 && prefix_index + 1 < (sizeof(prefixes) / sizeof(prefixes[0]))) {
        value /= 1000.0;
        ++prefix_index;
    }

    /* No fractional digits for the base unit (e.g. "42 B/s"), one decimal beyond it. */
    if (prefix_index == 0) {
        snprintf(out, out_cap, "%.0f %s%s/s", value, prefixes[prefix_index], unit_base);
    } else {
        snprintf(out, out_cap, "%.1f %s%s/s", value, prefixes[prefix_index], unit_base);
    }
}
