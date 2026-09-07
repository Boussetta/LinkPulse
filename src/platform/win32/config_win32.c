#include "linkpulse/config.h"

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
    snprintf(out, cap, "%s\\LinkPulse", appdata);
    if (!CreateDirectoryA(out, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    snprintf(out, cap, "%s\\LinkPulse\\config.ini", appdata);
    return true;
}

lp_status_t lp_config_load(lp_config_t *config)
{
    lp_config_defaults(config);

    char path[MAX_PATH];
    if (!config_path(path, sizeof(path))) {
        return LP_OK; /* no %APPDATA%: defaults are still a valid, working config */
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return LP_OK; /* first run: no config file yet */
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
    return LP_OK;
}

lp_status_t lp_config_save(const lp_config_t *config)
{
    char path[MAX_PATH];
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
    return (had_error || written != len) ? LP_ERR_IO : LP_OK;
}
