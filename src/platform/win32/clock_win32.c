#include "linkpulse/clock.h"

#include <windows.h>

uint64_t lp_clock_monotonic_ns(void)
{
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    const uint64_t ticks = (uint64_t)now.QuadPart;
    const uint64_t f = (uint64_t)freq.QuadPart;

    /* Split to keep the intermediate product from overflowing after ~9 hours. */
    return (ticks / f) * 1000000000ULL + ((ticks % f) * 1000000000ULL) / f;
}
