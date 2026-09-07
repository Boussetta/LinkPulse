#ifndef LINKPULSE_FORMAT_H
#define LINKPULSE_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Renders a byte rate as a human-readable string, e.g. "12.3 Mb/s" or
   "1.5 MB/s". Scales through b/s, Kb/s, Mb/s, Gb/s (or the byte equivalents). */
void lp_format_rate(uint64_t bytes_per_sec, bool use_bits, char *out, size_t out_cap);

#endif /* LINKPULSE_FORMAT_H */
