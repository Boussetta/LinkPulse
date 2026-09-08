#include "linkpulse/config.h"
#include "linkpulse/tray.h"
#include "linkpulse/update.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <shellapi.h>

/* Dedicated tray-only entry point: no argument parsing, no console I/O -- see
   CMakeLists.txt for why this isn't just linkpulse.exe --tray with a GUI
   subsystem. Always configured from the saved config file; use linkpulse.exe
   --tray (console subsystem) instead if you need to override settings or see
   debug log output while testing. */
int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "linkpulse://download-update") == 0) {
        char installer_path[MAX_PATH];
        if (lp_update_download_latest(installer_path, sizeof(installer_path)) != LP_OK) {
            MessageBoxA(NULL, "Could not download the LinkPulse update.", "LinkPulse update",
                        MB_OK | MB_ICONERROR);
            return 1;
        }
        if ((INT_PTR)ShellExecuteA(NULL, "open", installer_path, NULL, NULL, SW_SHOWNORMAL) <= 32) {
            DeleteFileA(installer_path);
            MessageBoxA(NULL, "Could not launch the LinkPulse installer.", "LinkPulse update",
                        MB_OK | MB_ICONERROR);
            return 1;
        }
        return 0;
    }

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
