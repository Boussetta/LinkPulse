#include "lp_test.h"

#include "linkpulse/clock.h"

#include <windows.h>

/* Verifies the Windows monotonic clock advances at a plausible rate. */
int main(void)
{
    const uint64_t first = lp_clock_monotonic_ns();
    Sleep(20);
    const uint64_t second = lp_clock_monotonic_ns();

    LP_CHECK(second > first);
    /* Sleep(20) must have advanced the clock by at least 10 ms, allowing for
       timer granularity, and not by an implausible amount. */
    LP_CHECK(second - first > 10ULL * 1000000ULL);
    LP_CHECK(second - first < 5ULL * 1000000000ULL);

    LP_TEST_RETURN();
}
