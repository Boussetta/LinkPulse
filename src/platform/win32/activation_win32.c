#include "linkpulse/activation.h"

#include "linkpulse/update.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <windows.h>
#include <shellapi.h>

#if defined(_MSC_VER)
#include <objbase.h>
#include <unknwn.h>

DEFINE_GUID(CLSID_LinkPulseToastActivator,
            0x7f2d2e64, 0x9b2a, 0x4b2d, 0x8b, 0x4d, 0x71, 0x4c, 0x5a, 0x83, 0x2e, 0x11);
DEFINE_GUID(IID_LinkPulseNotificationActivationCallback,
            0x53e31837, 0x6600, 0x4a81, 0x93, 0x95, 0x75, 0xcf, 0xfe, 0x74, 0x6f, 0x94);

#ifndef NOTIFICATION_USER_INPUT_DATA_DEFINED
#define NOTIFICATION_USER_INPUT_DATA_DEFINED
typedef struct {
    LPCWSTR Key;
    LPCWSTR Value;
} LP_NOTIFICATION_USER_INPUT_DATA;
#endif

typedef struct LP_NOTIFICATION_ACTIVATION_CALLBACK LP_NOTIFICATION_ACTIVATION_CALLBACK;
typedef struct LP_NOTIFICATION_ACTIVATION_CALLBACK_VTBL {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(LP_NOTIFICATION_ACTIVATION_CALLBACK *, REFIID,
                                                void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(LP_NOTIFICATION_ACTIVATION_CALLBACK *);
    ULONG(STDMETHODCALLTYPE *Release)(LP_NOTIFICATION_ACTIVATION_CALLBACK *);
    HRESULT(STDMETHODCALLTYPE *Activate)(LP_NOTIFICATION_ACTIVATION_CALLBACK *, LPCWSTR,
                                         LPCWSTR, const LP_NOTIFICATION_USER_INPUT_DATA *, ULONG);
} LP_NOTIFICATION_ACTIVATION_CALLBACK_VTBL;

struct LP_NOTIFICATION_ACTIVATION_CALLBACK {
    const LP_NOTIFICATION_ACTIVATION_CALLBACK_VTBL *lpVtbl;
    LONG refs;
};

static HRESULT STDMETHODCALLTYPE callback_query_interface(LP_NOTIFICATION_ACTIVATION_CALLBACK *self,
                                                            REFIID iid, void **object)
{
    if (object == NULL) {
        return E_POINTER;
    }
    *object = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) ||
        IsEqualIID(iid, &IID_LinkPulseNotificationActivationCallback)) {
        *object = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE callback_add_ref(LP_NOTIFICATION_ACTIVATION_CALLBACK *self)
{
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG STDMETHODCALLTYPE callback_release(LP_NOTIFICATION_ACTIVATION_CALLBACK *self)
{
    const ULONG refs = (ULONG)InterlockedDecrement(&self->refs);
    if (refs == 0) {
        HeapFree(GetProcessHeap(), 0, self);
    }
    return refs;
}

static HRESULT STDMETHODCALLTYPE callback_activate(LP_NOTIFICATION_ACTIVATION_CALLBACK *self,
                                                   LPCWSTR app_user_model_id, LPCWSTR invoked_args,
                                                   const LP_NOTIFICATION_USER_INPUT_DATA *data,
                                                   ULONG data_count)
{
    (void)self;
    (void)app_user_model_id;
    (void)data;
    (void)data_count;
    if (invoked_args == NULL || lstrcmpW(invoked_args, L"linkpulse://download-update") != 0) {
        return S_OK;
    }

    char installer_path[MAX_PATH];
    if (lp_update_download_latest(installer_path, sizeof(installer_path)) != LP_OK) {
        return E_FAIL;
    }
    if ((INT_PTR)ShellExecuteA(NULL, "open", installer_path, NULL, NULL, SW_SHOWNORMAL) <= 32) {
        DeleteFileA(installer_path);
        return E_FAIL;
    }
    return S_OK;
}

static const LP_NOTIFICATION_ACTIVATION_CALLBACK_VTBL callback_vtbl = {
    callback_query_interface, callback_add_ref, callback_release, callback_activate};

typedef struct LP_ACTIVATOR_FACTORY LP_ACTIVATOR_FACTORY;
typedef struct LP_ACTIVATOR_FACTORY_VTBL {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(LP_ACTIVATOR_FACTORY *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(LP_ACTIVATOR_FACTORY *);
    ULONG(STDMETHODCALLTYPE *Release)(LP_ACTIVATOR_FACTORY *);
    HRESULT(STDMETHODCALLTYPE *CreateInstance)(LP_ACTIVATOR_FACTORY *, IUnknown *, REFIID, void **);
    HRESULT(STDMETHODCALLTYPE *LockServer)(LP_ACTIVATOR_FACTORY *, BOOL);
} LP_ACTIVATOR_FACTORY_VTBL;

struct LP_ACTIVATOR_FACTORY {
    const LP_ACTIVATOR_FACTORY_VTBL *lpVtbl;
    LONG refs;
};

static HRESULT STDMETHODCALLTYPE factory_query_interface(LP_ACTIVATOR_FACTORY *self, REFIID iid,
                                                          void **object)
{
    if (object == NULL) {
        return E_POINTER;
    }
    *object = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory)) {
        *object = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE factory_add_ref(LP_ACTIVATOR_FACTORY *self)
{
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG STDMETHODCALLTYPE factory_release(LP_ACTIVATOR_FACTORY *self)
{
    return (ULONG)InterlockedDecrement(&self->refs);
}

static HRESULT STDMETHODCALLTYPE factory_create_instance(LP_ACTIVATOR_FACTORY *self, IUnknown *outer,
                                                           REFIID iid, void **object)
{
    (void)self;
    if (outer != NULL) {
        return CLASS_E_NOAGGREGATION;
    }
    LP_NOTIFICATION_ACTIVATION_CALLBACK *callback =
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*callback));
    if (callback == NULL) {
        return E_OUTOFMEMORY;
    }
    callback->lpVtbl = &callback_vtbl;
    callback->refs = 1;
    const HRESULT result = callback->lpVtbl->QueryInterface(callback, iid, object);
    callback->lpVtbl->Release(callback);
    return result;
}

static HRESULT STDMETHODCALLTYPE factory_lock_server(LP_ACTIVATOR_FACTORY *self, BOOL lock)
{
    (void)self;
    (void)lock;
    return S_OK;
}

static const LP_ACTIVATOR_FACTORY_VTBL factory_vtbl = {
    factory_query_interface, factory_add_ref, factory_release, factory_create_instance,
    factory_lock_server};

int lp_win32_run_toast_activator(void)
{
    HRESULT result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(result)) {
        return 1;
    }

    LP_ACTIVATOR_FACTORY factory = {&factory_vtbl, 1};
    DWORD cookie = 0;
    result = CoRegisterClassObject(&CLSID_LinkPulseToastActivator, (IUnknown *)&factory,
                                   CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &cookie);
    if (FAILED(result)) {
        CoUninitialize();
        return 1;
    }

    MSG message;
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    CoRevokeClassObject(cookie);
    CoUninitialize();
    return 0;
}

#else

int lp_win32_run_toast_activator(void)
{
    return 1;
}

#endif
