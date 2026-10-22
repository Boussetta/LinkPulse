#include "linkpulse/format.h"

#include <stdio.h>

/* Chooses a decimal unit and formats a rate without exposing floating-point details to callers. */
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

/* Escapes one UTF-8 string for XML, counting the full length even when the buffer is short. */
size_t lp_format_xml_escape(const char *in, char *out, size_t out_cap)
{
    if (out != NULL && out_cap > 0) {
        out[0] = '\0';
    }
    if (in == NULL) {
        return 0;
    }

    size_t length = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; ++p) {
        const char *entity = NULL;
        switch (*p) {
        case '&':
            entity = "&amp;";
            break;
        case '<':
            entity = "&lt;";
            break;
        case '>':
            entity = "&gt;";
            break;
        case '"':
            entity = "&quot;";
            break;
        case '\'':
            entity = "&apos;";
            break;
        default:
            break;
        }
        /* Control characters are invalid in XML 1.0 and never meaningful in a toast. */
        if (entity == NULL && (*p < 0x20 || *p == 0x7f)) {
            continue;
        }

        const char single[2] = {(char)*p, '\0'};
        for (const char *piece = (entity != NULL) ? entity : single; *piece != '\0';
             ++piece, ++length) {
            if (out != NULL && length + 1 < out_cap) {
                out[length] = *piece;
                out[length + 1] = '\0';
            }
        }
    }
    return length;
}
