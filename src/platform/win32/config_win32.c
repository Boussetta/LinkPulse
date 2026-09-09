#include "linkpulse/config.h"
#include "linkpulse/log.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>

/* %APPDATA%\LinkPulse\config.ini; returns false if %APPDATA% is unset (should
   not happen in a normal interactive session, but this is not a hard error --
   callers just fall back to defaults). */
static bool config_path(char *out, size_t cap)
{
    char appdata[MAX_PATH];
    const DWORD len = GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata));
    if (len == 0 || len >= sizeof(appdata)) {
        return false;
    }

    char dir[MAX_PATH + 32];
    int written = snprintf(dir, sizeof(dir), "%s\\LinkPulse", appdata);
    if (written < 0 || (size_t)written >= sizeof(dir)) {
        return false; /* %APPDATA% too long to safely append our subdirectory to */
    }
    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }

    written = snprintf(out, cap, "%s\\LinkPulse\\config.ini", appdata);
    return written >= 0 && (size_t)written < cap;
}

/* Loads the bounded per-user config file, treating a missing file as first run. */
lp_status_t lp_config_load(lp_config_t *config)
{
    LP_DEBUG("loading configuration");
    lp_config_defaults(config);

    char appdata[MAX_PATH];
    const DWORD appdata_len = GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata));
    if (appdata_len == 0 || appdata_len >= sizeof(appdata)) {
        return LP_OK; /* no %APPDATA%: defaults are still a valid, working config */
    }

    char path[MAX_PATH];
    const int written = snprintf(path, sizeof(path), "%s\\LinkPulse\\config.ini", appdata);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return LP_OK;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        const DWORD attrs = GetFileAttributesA(path);
        if (attrs == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
            LP_DEBUG("configuration file not found; using defaults");
            return LP_OK; /* first run: no config file yet */
        }
        return LP_ERR_IO;
    }

    char text[4096];
    const size_t read = fread(text, 1, sizeof(text) - 1, file);
    const bool had_error = (ferror(file) != 0);
    fclose(file);
    if (had_error) {
        return LP_ERR_IO;
    }
    text[read] = '\0';

    lp_config_parse(config, text);
    LP_DEBUG("configuration loaded: mode=%d iface=%s virtual=%s bits=%s interval_ms=%u",
             config->mode, config->iface_name[0] != '\0' ? config->iface_name : "(default)",
             config->include_virtual ? "yes" : "no", config->use_bits ? "yes" : "no",
             config->interval_ms);
    return LP_OK;
}

/* Creates the per-user directory and atomically writes the serialized settings stream. */
lp_status_t lp_config_save(const lp_config_t *config)
{
    LP_DEBUG("saving configuration");
    char path[MAX_PATH + 32];
    if (!config_path(path, sizeof(path))) {
        return LP_ERR_IO;
    }

    char text[4096];
    const size_t len = lp_config_serialize(config, text, sizeof(text));

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return LP_ERR_IO;
    }
    const size_t written = fwrite(text, 1, len, file);
    const bool had_error = (ferror(file) != 0);
    fclose(file);
    const lp_status_t status = (had_error || written != len) ? LP_ERR_IO : LP_OK;
    if (status == LP_OK) {
        LP_DEBUG("configuration saved");
    }
    return status;
}
