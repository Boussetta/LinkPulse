#include "linkpulse/autostart.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>

#define LP_AUTOSTART_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define LP_AUTOSTART_VALUE "LinkPulse"

bool lp_autostart_is_enabled(void)
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, LP_AUTOSTART_KEY, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegQueryValueExA(key, LP_AUTOSTART_VALUE, NULL, NULL, NULL, NULL);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

lp_status_t lp_autostart_set(bool enabled)
{
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, LP_AUTOSTART_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key,
                        NULL) != ERROR_SUCCESS) {
        return LP_ERR_IO;
    }

    lp_status_t status = LP_OK;
    if (enabled) {
        char exe_path[MAX_PATH];
        const DWORD len = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
        if (len == 0 || len >= sizeof(exe_path)) {
            RegCloseKey(key);
            return LP_ERR_IO;
        }

        char command[MAX_PATH + 16];
        snprintf(command, sizeof(command), "\"%s\" --tray", exe_path);

        if (RegSetValueExA(key, LP_AUTOSTART_VALUE, 0, REG_SZ, (const BYTE *)command,
                           (DWORD)strlen(command) + 1) != ERROR_SUCCESS) {
            status = LP_ERR_IO;
        }
    } else {
        const LONG result = RegDeleteValueA(key, LP_AUTOSTART_VALUE);
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
            status = LP_ERR_IO;
        }
    }

    RegCloseKey(key);
    return status;
}
