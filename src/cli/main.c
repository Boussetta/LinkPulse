#include "linkpulse/clock.h"
#include "linkpulse/log.h"
#include "linkpulse/net.h"

#include <stdio.h>
#include <string.h>

#ifndef LP_VERSION
#define LP_VERSION "0.0.0-unknown" /* overridden by the CMake project version */
#endif

static void print_usage(void)
{
    printf("LinkPulse " LP_VERSION " - local network activity monitor\n\n"
           "Usage: linkpulse [options]\n\n"
           "  --list       List network interfaces and their current byte counters\n"
           "  --debug      Enable debug logging\n"
           "  --version    Print version and exit\n"
           "  --help       Show this help\n");
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

int main(int argc, char **argv)
{
    bool want_list = false;

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
        } else {
            fprintf(stderr, "unknown option: %s\n\n", argv[i]);
            print_usage();
            return 2;
        }
    }

    LP_DEBUG("monotonic clock reads %llu ns", (unsigned long long)lp_clock_monotonic_ns());

    if (want_list) {
        return list_interfaces();
    }

    print_usage();
    return 0;
}
