#include "linkpulse/shortcut.h"

#include <stdbool.h>
#include <stdio.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <objbase.h>
#include <windows.h>

#ifndef PKEY_AppUserModel_ToastActivatorCLSID
static const PROPERTYKEY LP_PKEY_AppUserModel_ToastActivatorCLSID = {
    {0x9f4c2855, 0x9f79, 0x4b39, {0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3}},
    26};
#define PKEY_AppUserModel_ToastActivatorCLSID LP_PKEY_AppUserModel_ToastActivatorCLSID
#endif

#define LP_TOAST_ACTIVATOR_CLSID_STRING L"{7F2D2E64-9B2A-4B2D-8B4D-714C5A832E11}"
#define LP_APP_USER_MODEL_ID L"LinkPulse.NetworkMonitor"

int lp_win32_register_toast_shortcut(void)
{
    wchar_t programs_path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, programs_path))) {
        return 1;
    }

    wchar_t shortcut_path[MAX_PATH];
    const int written = swprintf(shortcut_path, sizeof(shortcut_path) / sizeof(shortcut_path[0]),
                                 L"%ls\\LinkPulse\\LinkPulse.lnk",
                                 programs_path);
    if (written < 0 || (size_t)written >= sizeof(shortcut_path) / sizeof(shortcut_path[0])) {
        return 1;
    }

    HRESULT result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    const bool initialized_here = SUCCEEDED(result);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
        return 1;
    }

    IShellLinkW *link = NULL;
    IPropertyStore *properties = NULL;
    IPersistFile *persist = NULL;
    result = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW,
                              (void **)&link);
    if (SUCCEEDED(result)) {
        result = link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void **)&persist);
    }
    if (SUCCEEDED(result)) {
        result = persist->lpVtbl->Load(persist, shortcut_path, STGM_READWRITE);
    }
    if (SUCCEEDED(result)) {
        result = link->lpVtbl->QueryInterface(link, &IID_IPropertyStore, (void **)&properties);
    }
    if (SUCCEEDED(result)) {
        CLSID activator_clsid;
        result = CLSIDFromString(LP_TOAST_ACTIVATOR_CLSID_STRING, &activator_clsid);
        if (SUCCEEDED(result)) {
            PROPVARIANT value;
            PropVariantInit(&value);
            value.vt = VT_CLSID;
            value.puuid = (CLSID *)CoTaskMemAlloc(sizeof(CLSID));
            if (value.puuid == NULL) {
                result = E_OUTOFMEMORY;
            } else {
                *value.puuid = activator_clsid;
                result = properties->lpVtbl->SetValue(properties, &PKEY_AppUserModel_ToastActivatorCLSID,
                                                       &value);
                if (SUCCEEDED(result)) {
                    result = properties->lpVtbl->Commit(properties);
                }
            }
            PropVariantClear(&value);
        }
    }
    if (properties != NULL) {
        properties->lpVtbl->Release(properties);
    }
    if (persist != NULL) {
        persist->lpVtbl->Release(persist);
    }
    if (link != NULL) {
        link->lpVtbl->Release(link);
    }
    if (initialized_here) {
        CoUninitialize();
    }
    return SUCCEEDED(result) ? 0 : 1;
}
