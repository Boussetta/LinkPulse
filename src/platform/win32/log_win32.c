#include "linkpulse/platform_log.h"

#include "linkpulse/log.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>

#define LP_LOG_MAX_BYTES (1024L * 1024L)

static FILE *g_file;

/* Opens or rotates the per-user log before attaching it to the core logger.
   Uses wide APIs so profile paths outside the ANSI code page still work. */
int lp_win32_log_start(void)
{
    wchar_t local_app_data[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return 1;
    }

    wchar_t directory[MAX_PATH];
    const int directory_length =
        swprintf(directory, MAX_PATH, L"%ls\\LinkPulse", local_app_data);
    if (directory_length < 0 || directory_length >= MAX_PATH ||
        (CreateDirectoryW(directory, NULL) == 0 && GetLastError() != ERROR_ALREADY_EXISTS)) {
        return 1;
    }

    wchar_t path[MAX_PATH];
    const int path_length = swprintf(path, MAX_PATH, L"%ls\\LinkPulse.log", directory);
    if (path_length < 0 || path_length >= MAX_PATH) {
        return 1;
    }

    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &attributes)) {
        const ULARGE_INTEGER file_size = {
            .LowPart = attributes.nFileSizeLow,
            .HighPart = attributes.nFileSizeHigh,
        };
        if (file_size.QuadPart >= LP_LOG_MAX_BYTES) {
            wchar_t backup[MAX_PATH];
            const int backup_length = swprintf(backup, MAX_PATH, L"%ls.1", path);
            if (backup_length >= 0 && backup_length < MAX_PATH) {
                DeleteFileW(backup);
                MoveFileW(path, backup);
            }
        }
    }

    g_file = _wfopen(path, L"ab");
    if (g_file == NULL) {
        return 1;
    }
    lp_log_set_file(g_file);
    LP_INFO("file logging started");
    return 0;
}

/* Detaches and closes the file sink while preserving stderr logging. */
void lp_win32_log_stop(void)
{
    if (g_file != NULL) {
        LP_INFO("file logging stopped");
        lp_log_set_file(NULL);
        fclose(g_file);
        g_file = NULL;
    }
}
