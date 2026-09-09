#include "linkpulse/config.h"
#include "linkpulse/activation.h"
#include "linkpulse/log.h"
#include "linkpulse/platform_log.h"
#include "linkpulse/shortcut.h"
#include "linkpulse/tray.h"
#include "linkpulse/update.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <shellapi.h>

/* Dedicated tray-only entry point: handles toast/protocol activation arguments
   and otherwise runs the tray app with no interactive console I/O -- see
   CMakeLists.txt for why this isn't just linkpulse.exe --tray with a GUI
   subsystem. Always configured from the saved config file; use linkpulse.exe
   --tray (console subsystem) instead if you need to override settings or see
   debug log output while testing. */
/* Handles protocol/COM activation or starts the normal argument-free tray app. */
int main(int argc, char **argv)
{
    if (lp_win32_log_start() != 0) {
        LP_WARN("failed to start persistent file logging");
    }
LP_INFO("tray entrypoint started: argc=%d mode=%s", argc,
        argc > 1 ? argv[1] : "(none)");
    if (argc > 1 &&
        (strcmp(argv[1], "/ToastActivator") == 0 || strcmp(argv[1], "-Embedding") == 0)) {
        LP_INFO("starting toast COM activator");
        const int result = lp_win32_run_toast_activator();
        lp_win32_log_stop();
        return result;
    }
    if (argc > 1 && strcmp(argv[1], "/RegisterToastShortcut") == 0) {
        const int result = lp_win32_register_toast_shortcut();
        lp_win32_log_stop();
        return result;
    }
    if (argc > 1 &&
        (strcmp(argv[1], "linkpulse://download-update") == 0 ||
         strcmp(argv[1], "linkpulse://download-update/") == 0)) {
        LP_INFO("protocol update activation received");
        char installer_path[MAX_PATH];
        if (lp_update_download_latest(installer_path, sizeof(installer_path)) != LP_OK) {
            MessageBoxA(NULL, "Could not download the LinkPulse update.", "LinkPulse update",
                        MB_OK | MB_ICONERROR);
            lp_win32_log_stop();
            return 1;
        }
        if ((INT_PTR)ShellExecuteA(NULL, "open", installer_path, NULL, NULL, SW_SHOWNORMAL) <= 32) {
            DeleteFileA(installer_path);
            MessageBoxA(NULL, "Could not launch the LinkPulse installer.", "LinkPulse update",
                        MB_OK | MB_ICONERROR);
            lp_win32_log_stop();
            return 1;
        }
        lp_win32_log_stop();
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

    const int result = lp_tray_run(&sampler_config, config.use_bits, config.interval_ms);
    lp_win32_log_stop();
    return result;
}
