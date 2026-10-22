#include "linkpulse/config.h"
#include "linkpulse/log.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>

/* %APPDATA%\LinkPulse\config.ini, built with wide APIs so profile paths outside
   the ANSI code page survive. Returns false if %APPDATA% is unset (should not
   happen in a normal interactive session, but this is not a hard error --
   callers just fall back to defaults). Creates the directory when asked. */
static bool config_path(wchar_t *out, size_t cap, bool create_dir)
{
    wchar_t appdata[MAX_PATH];
    const DWORD len = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }

    wchar_t dir[MAX_PATH + 32];
    int written = swprintf(dir, sizeof(dir) / sizeof(dir[0]), L"%ls\\LinkPulse", appdata);
    if (written < 0 || (size_t)written >= sizeof(dir) / sizeof(dir[0])) {
        return false; /* %APPDATA% too long to safely append our subdirectory to */
    }
    if (create_dir && !CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }

    written = swprintf(out, cap, L"%ls\\config.ini", dir);
    return written >= 0 && (size_t)written < cap;
}

/* Loads the bounded per-user config file, treating a missing file as first run. */
lp_status_t lp_config_load(lp_config_t *config)
{
    LP_DEBUG("loading configuration");
    lp_config_defaults(config);

    wchar_t path[MAX_PATH + 32];
    if (!config_path(path, sizeof(path) / sizeof(path[0]), false)) {
        return LP_OK; /* no %APPDATA%: defaults are still a valid, working config */
    }

    FILE *file = _wfopen(path, L"rb");
    if (file == NULL) {
        const DWORD attrs = GetFileAttributesW(path);
        if (attrs == INVALID_FILE_ATTRIBUTES && (GetLastError() == ERROR_FILE_NOT_FOUND ||
                                                 GetLastError() == ERROR_PATH_NOT_FOUND)) {
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

/* Writes `len` bytes to a new file at `path` and forces them to disk. */
static bool write_file_durably(const wchar_t *path, const char *data, size_t len)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, data, (DWORD)len, &written, NULL) && written == (DWORD)len &&
                    FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

/* Creates the per-user directory and replaces the settings file atomically: the
   new contents go to a temporary sibling that is flushed to disk and then
   renamed over the old file, so a crash mid-save leaves either the previous or
   the new settings, never a truncated file. */
lp_status_t lp_config_save(const lp_config_t *config)
{
    LP_DEBUG("saving configuration");
    wchar_t path[MAX_PATH + 32];
    if (!config_path(path, sizeof(path) / sizeof(path[0]), true)) {
        return LP_ERR_IO;
    }
    wchar_t temp_path[MAX_PATH + 48];
    const int temp_len =
        swprintf(temp_path, sizeof(temp_path) / sizeof(temp_path[0]), L"%ls.tmp", path);
    if (temp_len < 0 || (size_t)temp_len >= sizeof(temp_path) / sizeof(temp_path[0])) {
        return LP_ERR_IO;
    }

    char text[4096];
    const size_t len = lp_config_serialize(config, text, sizeof(text));

    if (!write_file_durably(temp_path, text, len)) {
        (void)DeleteFileW(temp_path);
        return LP_ERR_IO;
    }
    if (!MoveFileExW(temp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LP_WARN("could not replace configuration file: error %lu", GetLastError());
        (void)DeleteFileW(temp_path);
        return LP_ERR_IO;
    }
    LP_DEBUG("configuration saved");
    return LP_OK;
}
