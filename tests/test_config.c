#include "lp_test.h"

#include "linkpulse/config.h"

/* Verifies the persisted-settings baseline used on first run. */
static void test_defaults(void)
{
    lp_config_t config;
    lp_config_defaults(&config);
    LP_CHECK(config.mode == LP_IFACE_SELECT_AUTO);
    LP_CHECK_STR_EQ(config.iface_name, "");
    LP_CHECK(!config.include_virtual);
    LP_CHECK(!config.use_bits);
    LP_CHECK(config.interval_ms == 1000);
}

/* Verifies whitespace, comments, and all supported key overrides. */
static void test_parse_overrides_fields(void)
{
    lp_config_t config;
    lp_config_defaults(&config);

    lp_config_parse(&config, "# a comment\n"
                             "; another comment\n"
                             "\n"
                             "mode=manual\n"
                             "iface = Wi-Fi \n"
                             "include_virtual=1\n"
                             "use_bits=1\n"
                             "interval_ms=500\n");

    LP_CHECK(config.mode == LP_IFACE_SELECT_MANUAL);
    LP_CHECK_STR_EQ(config.iface_name, "Wi-Fi");
    LP_CHECK(config.include_virtual);
    LP_CHECK(config.use_bits);
    LP_CHECK(config.interval_ms == 500);
}

/* Verifies malformed input cannot overwrite a valid existing configuration. */
static void test_parse_ignores_unknown_and_malformed(void)
{
    lp_config_t config;
    lp_config_defaults(&config);
    config.include_virtual = true;
    config.use_bits = true;

    lp_config_parse(&config, "not_a_key_value_line\n"
                             "unknown_key=123\n"
                             "mode=bogus\n"
                             "include_virtual=bogus\n"
                             "use_bits=bogus\n"
                             "interval_ms=not_a_number\n"
                             "interval_ms=0\n");

    /* All malformed/unknown: known keys with malformed values must leave existing fields unchanged. */
    LP_CHECK(config.mode == LP_IFACE_SELECT_AUTO);
    LP_CHECK(config.include_virtual);
    LP_CHECK(config.use_bits);
    LP_CHECK(config.interval_ms == 1000);
}

/* Verifies canonical serialization can reconstruct every configuration field. */
static void test_serialize_round_trips_through_parse(void)
{
    lp_config_t original;
    lp_config_defaults(&original);
    original.mode = LP_IFACE_SELECT_ALL;
    original.include_virtual = true;
    original.use_bits = true;
    original.interval_ms = 2000;
    snprintf(original.iface_name, sizeof(original.iface_name), "Ethernet");

    char text[512];
    const size_t len = lp_config_serialize(&original, text, sizeof(text));
    LP_CHECK(len > 0);
    LP_CHECK(len == strlen(text));

    lp_config_t restored;
    lp_config_defaults(&restored);
    lp_config_parse(&restored, text);

    LP_CHECK(restored.mode == original.mode);
    LP_CHECK_STR_EQ(restored.iface_name, original.iface_name);
    LP_CHECK(restored.include_virtual == original.include_virtual);
    LP_CHECK(restored.use_bits == original.use_bits);
    LP_CHECK(restored.interval_ms == original.interval_ms);
}

/* Verifies serialization handles unusable output buffers without writing. */
static void test_serialize_rejects_null_and_zero_cap(void)
{
    lp_config_t config;
    lp_config_defaults(&config);

    char buffer[8];
    LP_CHECK(lp_config_serialize(&config, buffer, 0) == 0);
    LP_CHECK(lp_config_serialize(&config, NULL, sizeof(buffer)) == 0);
}

/* Runs the configuration contract scenarios. */
int main(void)
{
    test_defaults();
    test_parse_overrides_fields();
    test_parse_ignores_unknown_and_malformed();
    test_serialize_round_trips_through_parse();
    test_serialize_rejects_null_and_zero_cap();
    LP_TEST_RETURN();
}
