#include "linkpulse/clock.h"

#include <windows.h>

/* Computes value * multiplier / divisor without overflowing intermediate products. */
static uint64_t muldiv_fraction_u64(uint64_t value, uint32_t multiplier, uint64_t divisor)
{
    uint64_t quotient = 0;
    uint64_t remainder = 0;

    for (int bit = 31; bit >= 0; --bit) {
        quotient *= 2;
        const uint64_t doubled = remainder + remainder;
        if (doubled < remainder || doubled >= divisor) {
            ++quotient;
            remainder = doubled - divisor;
        } else {
            remainder = doubled;
        }

        if ((multiplier & (1u << bit)) != 0u) {
            const uint64_t sum = remainder + value;
            if (sum < remainder || sum >= divisor) {
                ++quotient;
                remainder = sum - divisor;
            } else {
                remainder = sum;
            }
        }
    }

    return quotient;
}

/* Converts QueryPerformanceCounter ticks to a monotonic nanosecond timestamp. */
uint64_t lp_clock_monotonic_ns(void)
{
    /* Queried every call rather than cached in a static: QueryPerformanceFrequency
       is cheap and fixed for the life of the process, and caching it in a plain
       static would be an unsynchronized data race across threads. */
    LARGE_INTEGER freq;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
        return 0;
    }

    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now)) {
        return 0;
    }

    const uint64_t ticks = (uint64_t)now.QuadPart;
    const uint64_t f = (uint64_t)freq.QuadPart;
    const uint64_t whole_seconds = ticks / f;
    const uint64_t fractional_ticks = ticks % f;

    /* Keep the nanosecond conversion exact without overflowing uint64_t
       intermediates, even for unusually large counter frequencies. */
    return whole_seconds * 1000000000ULL + muldiv_fraction_u64(fractional_ticks, 1000000000u, f);
}
