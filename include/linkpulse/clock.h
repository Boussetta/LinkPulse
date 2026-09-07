#ifndef LINKPULSE_CLOCK_H
#define LINKPULSE_CLOCK_H

#include <stdint.h>

/* Steady clock, unaffected by wall-clock adjustments. Rate math must use this
   and never time-of-day, or an NTP step produces absurd bitrates. */
uint64_t lp_clock_monotonic_ns(void);

#endif /* LINKPULSE_CLOCK_H */
