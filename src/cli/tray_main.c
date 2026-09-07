#include "linkpulse/config.h"
#include "linkpulse/tray.h"

#include <stdio.h>

/* Dedicated tray-only entry point: no argument parsing, no console I/O -- see
   CMakeLists.txt for why this isn't just linkpulse.exe --tray with a GUI
   subsystem. Always configured from the saved config file; use linkpulse.exe
   --tray (console subsystem) instead if you need to override settings or see
   debug log output while testing. */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    lp_config_t config;
    if (lp_config_load(&config) != LP_OK) {
        lp_config_defaults(&config);
    }

    lp_sampler_config_t sampler_config;
    sampler_config.mode = config.mode;
    sampler_config.include_virtual = config.include_virtual;
    snprintf(sampler_config.iface_name, sizeof(sampler_config.iface_name), "%s", config.iface_name);

    return lp_tray_run(&sampler_config, config.use_bits, config.interval_ms);
}
