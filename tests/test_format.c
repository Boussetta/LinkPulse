#include "lp_test.h"

#include "linkpulse/format.h"

/* Runs one formatting expectation with a fixed-size caller buffer. */
static void check_rate(uint64_t bytes_per_sec, bool use_bits, const char *expected)
{
    char buffer[64];
    lp_format_rate(bytes_per_sec, use_bits, buffer, sizeof(buffer));
    LP_CHECK_STR_EQ(buffer, expected);
}

/* Covers base units, decimal scaling, bit conversion, and rounding boundaries. */
int main(void)
{
    check_rate(0, false, "0 B/s");
    check_rate(512, false, "512 B/s");
    check_rate(1500, false, "1.5 KB/s");
    check_rate(1500000, false, "1.5 MB/s");
    check_rate(1500000000, false, "1.5 GB/s");

    check_rate(0, true, "0 b/s");
    check_rate(125, true, "1.0 Kb/s"); /* 125 B/s * 8 = 1000 b/s, which crosses into Kb/s */
    check_rate(125000, true, "1.0 Mb/s");

    /* Rounding-boundary regression: 999951 B/s must roll over to "1.0 MB/s", not
       round to the misleading "1000.0 KB/s". */
    check_rate(999951, false, "1.0 MB/s");

    LP_TEST_RETURN();
}
