#include "linkpulse/clock.h"

#include <windows.h>

uint64_t lp_clock_monotonic_ns(void)
{
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) {
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
            return 0;
        }
    }

    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now)) {
        return 0;
    }

    const uint64_t ticks = (uint64_t)now.QuadPart;
    const uint64_t f = (uint64_t)freq.QuadPart;

    /* Split to keep the intermediate product from overflowing after ~9 hours. */
    return (ticks / f) * 1000000000ULL + ((ticks % f) * 1000000000ULL) / f;
}
