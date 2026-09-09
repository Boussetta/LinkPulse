#include "linkpulse/platform_log.h"

#include "linkpulse/log.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>

#define LP_LOG_MAX_BYTES (1024L * 1024L)

static FILE *g_file;

/* Opens or rotates the per-user log before attaching it to the core logger. */
int lp_win32_log_start(void)
{
    char local_app_data[MAX_PATH];
    const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", local_app_data,
                                                 sizeof(local_app_data));
    if (length == 0 || length >= sizeof(local_app_data)) {
        return 1;
    }

    char directory[MAX_PATH];
    const int directory_length = snprintf(directory, sizeof(directory), "%s\\LinkPulse",
                                          local_app_data);
    if (directory_length < 0 || (size_t)directory_length >= sizeof(directory) ||
        (CreateDirectoryA(directory, NULL) == 0 && GetLastError() != ERROR_ALREADY_EXISTS)) {
        return 1;
    }

    char path[MAX_PATH];
    const int path_length = snprintf(path, sizeof(path), "%s\\LinkPulse.log", directory);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        return 1;
    }

    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &attributes)) {
        const ULARGE_INTEGER file_size = {
            .LowPart = attributes.nFileSizeLow,
            .HighPart = attributes.nFileSizeHigh,
        };
        if (file_size.QuadPart >= LP_LOG_MAX_BYTES) {
            char backup[MAX_PATH];
            const int backup_length = snprintf(backup, sizeof(backup), "%s.1", path);
            if (backup_length >= 0 && (size_t)backup_length < sizeof(backup)) {
                DeleteFileA(backup);
                MoveFileA(path, backup);
            }
        }
    }

    g_file = fopen(path, "ab");
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
