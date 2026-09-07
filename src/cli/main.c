#include "linkpulse/clock.h"
#include "linkpulse/format.h"
#include "linkpulse/log.h"
#include "linkpulse/net.h"
#include "linkpulse/sampler.h"
#include "linkpulse/tray.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#ifndef LP_VERSION
#define LP_VERSION "0.0.0-unknown" /* overridden by the CMake project version */
#endif

/* Set from the console-control handler, which runs on its own thread; checked
   once per loop iteration in watch_rate(). */
static volatile LONG g_stop_requested;

static BOOL WINAPI handle_console_event(DWORD event)
{
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        InterlockedExchange(&g_stop_requested, 1);
        return TRUE;
    default:
        return FALSE;
    }
}

static void print_usage(void)
{
    printf("LinkPulse " LP_VERSION " - local network activity monitor\n\n"
           "Usage: linkpulse [options]\n\n"
           "  --list                 List network interfaces and their current byte counters\n"
           "  --watch                Print live download/upload rates once per second\n"
           "  --tray                 Run as a system tray icon (right-click for options)\n"
           "  --iface <name>         Watch a specific interface instead of the default route\n"
           "  --all                  Watch the sum of all interfaces\n"
           "  --include-virtual      With --all, include virtual/pseudo adapters\n"
           "  --bits                 Show bit rates (Mb/s) instead of byte rates (MB/s)\n"
           "  --interval <ms>        Poll interval for --watch/--tray, default 1000\n"
           "  --debug                Enable debug logging\n"
           "  --version              Print version and exit\n"
           "  --help                 Show this help\n");
}

static int list_interfaces(void)
{
    lp_iface_list_t list;
    const lp_status_t status = lp_net_snapshot(&list);
    if (status != LP_OK) {
        LP_ERROR("failed to read interface table: %s", lp_status_str(status));
        return 1;
    }

    char active[LP_IFNAME_MAX];
    const bool have_active = (lp_net_default_iface(active, sizeof(active)) == LP_OK);
    if (have_active) {
        printf("Default route interface: %s\n\n", active);
    }

    printf("%-32s %16s %16s  %s\n", "INTERFACE", "RX BYTES", "TX BYTES", "FLAGS");
    for (size_t i = 0; i < list.count; ++i) {
        const lp_iface_t *iface = &list.items[i];
        char flags[64];
        snprintf(flags, sizeof(flags), "%s%s%s%s", iface->is_up ? "up " : "down ",
                 iface->is_loopback ? "loopback " : "", iface->is_virtual ? "virtual " : "",
                 (have_active && strcmp(iface->name, active) == 0) ? "*default*" : "");
        printf("%-32s %16llu %16llu  %s\n", iface->name, (unsigned long long)iface->rx_bytes,
               (unsigned long long)iface->tx_bytes, flags);
    }

    lp_iface_list_free(&list);
    return 0;
}

static int watch_rate(const lp_sampler_config_t *config, bool use_bits, unsigned interval_ms)
{
    lp_sampler_t sampler;
    lp_sampler_init(&sampler, config);
    const lp_sampler_sources_t sources = {lp_net_snapshot, lp_net_default_iface,
                                          lp_clock_monotonic_ns};
    lp_sampler_set_sources(&sampler, &sources);

    if (!SetConsoleCtrlHandler(handle_console_event, TRUE)) {
        LP_WARN("failed to install console control handler; Ctrl+C may not exit cleanly");
    }

    while (!InterlockedCompareExchange(&g_stop_requested, 0, 0)) {
        lp_rate_sample_t sample;
        const lp_status_t status = lp_sampler_poll(&sampler, &sample);
        if (status == LP_ERR_NOT_FOUND) {
            printf("\rwaiting for interface...                                   ");
            fflush(stdout);
        } else if (status != LP_OK) {
            fprintf(stderr, "\nfailed to sample interface: %s\n", lp_status_str(status));
            return 1;
        } else {
            char rx_str[32];
            char tx_str[32];
            lp_format_rate(sample.rx_bytes_per_sec, use_bits, rx_str, sizeof(rx_str));
            lp_format_rate(sample.tx_bytes_per_sec, use_bits, tx_str, sizeof(tx_str));
            printf("\rdown: %-12s up: %-12s", rx_str, tx_str);
            fflush(stdout);
        }
        Sleep(interval_ms);
    }

    printf("\n");
    return 0;
}

int main(int argc, char **argv)
{
    bool want_list = false;
    bool want_watch = false;
    bool want_tray = false;
    bool use_bits = false;
    unsigned interval_ms = 1000;
    lp_sampler_config_t sampler_config = {LP_IFACE_SELECT_AUTO, "", false};

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf(LP_VERSION "\n");
            return 0;
        }
        if (strcmp(argv[i], "--debug") == 0) {
            lp_log_set_level(LP_LOG_DEBUG);
        } else if (strcmp(argv[i], "--list") == 0) {
            want_list = true;
        } else if (strcmp(argv[i], "--watch") == 0) {
            want_watch = true;
        } else if (strcmp(argv[i], "--tray") == 0) {
            want_tray = true;
        } else if (strcmp(argv[i], "--all") == 0) {
            sampler_config.mode = LP_IFACE_SELECT_ALL;
        } else if (strcmp(argv[i], "--include-virtual") == 0) {
            sampler_config.include_virtual = true;
        } else if (strcmp(argv[i], "--bits") == 0) {
            use_bits = true;
        } else if (strcmp(argv[i], "--iface") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--iface requires a value\n\n");
                print_usage();
                return 2;
            }
            sampler_config.mode = LP_IFACE_SELECT_MANUAL;
            snprintf(sampler_config.iface_name, sizeof(sampler_config.iface_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--interval") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--interval requires a value\n\n");
                print_usage();
                return 2;
            }
            const char *value = argv[++i];
            char *end = NULL;
            const unsigned long parsed = strtoul(value, &end, 10);
            if (value[0] == '\0' || *end != '\0') {
                fprintf(stderr, "--interval must be a whole number of milliseconds, got '%s'\n",
                        value);
                return 2;
            }
            if (parsed == 0) {
                fprintf(stderr, "--interval must be a positive number of milliseconds\n");
                return 2;
            }
            interval_ms = (unsigned)parsed;
        } else {
            fprintf(stderr, "unknown option: %s\n\n", argv[i]);
            print_usage();
            return 2;
        }
    }

    LP_DEBUG("monotonic clock reads %llu ns", (unsigned long long)lp_clock_monotonic_ns());

    if (want_tray) {
        return lp_tray_run(&sampler_config, use_bits, interval_ms);
    }

    if (want_watch) {
        return watch_rate(&sampler_config, use_bits, interval_ms);
    }

    if (want_list) {
        return list_interfaces();
    }

    print_usage();
    return 0;
}
