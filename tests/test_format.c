#include "lp_test.h"

#include "linkpulse/format.h"

/* Runs one formatting expectation with a fixed-size caller buffer. */
static void check_rate(uint64_t bytes_per_sec, bool use_bits, const char *expected)
{
    char buffer[64];
    lp_format_rate(bytes_per_sec, use_bits, buffer, sizeof(buffer));
    LP_CHECK_STR_EQ(buffer, expected);
}

/* Covers entity escaping, control stripping, UTF-8 passthrough, and truncation reporting. */
static void test_xml_escape(void)
{
    char buffer[64];
    LP_CHECK(lp_format_xml_escape("plain", buffer, sizeof(buffer)) == 5);
    LP_CHECK_STR_EQ(buffer, "plain");

    lp_format_xml_escape("a&b<c>d\"e'f", buffer, sizeof(buffer));
    LP_CHECK_STR_EQ(buffer, "a&amp;b&lt;c&gt;d&quot;e&apos;f");

    /* A hostile hostname cannot close the text node and inject toast markup. */
    lp_format_xml_escape("x</text><actions>", buffer, sizeof(buffer));
    LP_CHECK_STR_EQ(buffer, "x&lt;/text&gt;&lt;actions&gt;");

    lp_format_xml_escape("ta\tb\x01\x7f"
                         "c\n",
                         buffer, sizeof(buffer));
    LP_CHECK_STR_EQ(buffer, "tabc");

    lp_format_xml_escape("Jos\xc3\xa9", buffer, sizeof(buffer));
    LP_CHECK_STR_EQ(buffer, "Jos\xc3\xa9");

    /* Truncated output stays terminated and the full length is reported for resizing. */
    char small[4];
    LP_CHECK(lp_format_xml_escape("&&", small, sizeof(small)) == 10);
    LP_CHECK(strlen(small) == 3);
    LP_CHECK(lp_format_xml_escape("&&", NULL, 0) == 10);
    LP_CHECK(lp_format_xml_escape(NULL, buffer, sizeof(buffer)) == 0);
    LP_CHECK_STR_EQ(buffer, "");
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

    test_xml_escape();

    LP_TEST_RETURN();
}
