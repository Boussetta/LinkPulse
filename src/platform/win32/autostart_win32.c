#include "linkpulse/autostart.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>

#define LP_AUTOSTART_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define LP_AUTOSTART_VALUE L"LinkPulse"
#define LP_AUTOSTART_EXE_NAME L"linkpulse-tray.exe"

/* Resolves to linkpulse-tray.exe next to whichever binary is currently
   running, not the currently running binary itself: toggling this from
   linkpulse.exe (the console/debug CLI) must not register that one for
   autostart, since it would flash a console at every login. Wide APIs keep
   install paths outside the ANSI code page intact. */
static bool tray_exe_path(wchar_t *out, size_t cap)
{
    wchar_t exe_path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }

    wchar_t *last_slash = wcsrchr(exe_path, L'\\');
    if (last_slash != NULL) {
        *(last_slash + 1) = L'\0';
    } else {
        exe_path[0] = L'\0';
    }

    const int needed = swprintf(out, cap, L"%ls%ls", exe_path, LP_AUTOSTART_EXE_NAME);
    return needed >= 0 && (size_t)needed < cap;
}

/* Reads the HKCU Run value directly so external startup changes are visible. */
bool lp_autostart_is_enabled(void)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, LP_AUTOSTART_KEY, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    const LONG result = RegQueryValueExW(key, LP_AUTOSTART_VALUE, NULL, &type, NULL, &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

/* Adds or removes the per-user startup command for the tray-only executable. */
lp_status_t lp_autostart_set(bool enabled)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, LP_AUTOSTART_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key,
                        NULL) != ERROR_SUCCESS) {
        return LP_ERR_IO;
    }

    lp_status_t status = LP_OK;
    if (enabled) {
        wchar_t exe_path[MAX_PATH];
        if (!tray_exe_path(exe_path, MAX_PATH)) {
            RegCloseKey(key);
            return LP_ERR_IO;
        }

        wchar_t command[MAX_PATH + 4];
        swprintf(command, MAX_PATH + 4, L"\"%ls\"", exe_path);

        const DWORD bytes = (DWORD)((wcslen(command) + 1) * sizeof(command[0]));
        if (RegSetValueExW(key, LP_AUTOSTART_VALUE, 0, REG_SZ, (const BYTE *)command, bytes) !=
            ERROR_SUCCESS) {
            status = LP_ERR_IO;
        }
    } else {
        const LONG result = RegDeleteValueW(key, LP_AUTOSTART_VALUE);
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
            status = LP_ERR_IO;
        }
    }

    RegCloseKey(key);
    return status;
}
