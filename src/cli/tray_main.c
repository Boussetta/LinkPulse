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

/* Parses the optional diagnostic level before the GUI process starts logging. */
static bool parse_verbosity(const char *value, lp_log_level_t *level)
{
    if (value == NULL || level == NULL) {
        return false;
    }
    if (strcmp(value, "error") == 0) {
        *level = LP_LOG_ERROR;
    } else if (strcmp(value, "warn") == 0 || strcmp(value, "warning") == 0) {
        *level = LP_LOG_WARN;
    } else if (strcmp(value, "info") == 0) {
        *level = LP_LOG_INFO;
    } else if (strcmp(value, "debug") == 0) {
        *level = LP_LOG_DEBUG;
    } else {
        return false;
    }
    return true;
}

/* Applies diagnostic command-line options; GUI output is written to LinkPulse.log. */
static bool configure_logging_from_args(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--debug") == 0) {
            lp_log_set_level(LP_LOG_DEBUG);
        } else if (strcmp(argv[i], "--verbosity") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0' || argv[i + 1][0] == '-') {
                return false;
            }
            lp_log_level_t level;
            if (!parse_verbosity(argv[++i], &level)) {
                return false;
            }
            lp_log_set_level(level);
        }
    }
    return true;
}

/* Dedicated tray-only entry point: handles toast/protocol activation arguments
   and otherwise runs the tray app with no interactive console I/O -- see
   CMakeLists.txt for why this isn't just linkpulse.exe --tray with a GUI
   subsystem. Always configured from the saved config file; use linkpulse.exe
   --tray (console subsystem) instead if you need to override settings or see
   debug log output while testing. */
/* Handles protocol/COM activation or starts the normal argument-free tray app. */
int main(int argc, char **argv)
{
    const bool logging_args_valid = configure_logging_from_args(argc, argv);
    if (lp_win32_log_start() != 0) {
        LP_WARN("failed to start persistent file logging");
    }
    LP_INFO("tray entrypoint started: argc=%d mode=%s", argc,
            argc > 1 ? argv[1] : "(none)");
    if (!logging_args_valid) {
        LP_ERROR("invalid logging arguments; use --verbosity error|warn|info|debug");
        lp_win32_log_stop();
        return 2;
    }
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
        LP_WARN("config load failed; using defaults");
        lp_config_defaults(&config);
    }

    lp_sampler_config_t sampler_config;
    sampler_config.mode = config.mode;
    sampler_config.include_virtual = config.include_virtual;
    snprintf(sampler_config.iface_name, sizeof(sampler_config.iface_name), "%s", config.iface_name);

        LP_INFO("starting tray: selection=%d iface=%s virtual=%s bits=%s interval_ms=%u",
            sampler_config.mode,
            sampler_config.iface_name[0] != '\0' ? sampler_config.iface_name : "(default)",
            sampler_config.include_virtual ? "yes" : "no", config.use_bits ? "yes" : "no",
            config.interval_ms);

    const int result = lp_tray_run(&sampler_config, config.use_bits, config.interval_ms);
    lp_win32_log_stop();
    return result;
}
