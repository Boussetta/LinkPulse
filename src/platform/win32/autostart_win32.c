#include "linkpulse/autostart.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>

#define LP_AUTOSTART_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define LP_AUTOSTART_VALUE "LinkPulse"
#define LP_AUTOSTART_EXE_NAME "linkpulse-tray.exe"

/* Resolves to linkpulse-tray.exe next to whichever binary is currently
   running, not the currently running binary itself: toggling this from
   linkpulse.exe (the console/debug CLI) must not register that one for
   autostart, since it would flash a console at every login. */
static bool tray_exe_path(char *out, size_t cap)
{
    char exe_path[MAX_PATH];
    const DWORD len = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    if (len == 0 || len >= sizeof(exe_path)) {
        return false;
    }

    char *last_slash = strrchr(exe_path, '\\');
    if (last_slash != NULL) {
        *(last_slash + 1) = '\0';
    } else {
        exe_path[0] = '\0';
    }

    const int needed = snprintf(out, cap, "%s%s", exe_path, LP_AUTOSTART_EXE_NAME);
    return needed >= 0 && (size_t)needed < cap;
}

bool lp_autostart_is_enabled(void)
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, LP_AUTOSTART_KEY, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    const LONG result = RegQueryValueExA(key, LP_AUTOSTART_VALUE, NULL, &type, NULL, &size);
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
        if (!tray_exe_path(exe_path, sizeof(exe_path))) {
            RegCloseKey(key);
            return LP_ERR_IO;
        }

        char command[MAX_PATH + 4];
        snprintf(command, sizeof(command), "\"%s\"", exe_path);

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
