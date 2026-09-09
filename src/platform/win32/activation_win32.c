#include "linkpulse/activation.h"

#include "linkpulse/update.h"
#include "linkpulse/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <windows.h>
#include <shellapi.h>

#if defined(_MSC_VER)
#define INITGUID
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

static DWORD activator_thread_id = 0;

/* Exposes the COM interfaces implemented by the toast callback object. */
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

/* Retains a callback object across COM clients and worker dispatch. */
static ULONG STDMETHODCALLTYPE callback_add_ref(LP_NOTIFICATION_ACTIVATION_CALLBACK *self)
{
    return (ULONG)InterlockedIncrement(&self->refs);
}

/* Releases the callback allocation when its final COM reference disappears. */
static ULONG STDMETHODCALLTYPE callback_release(LP_NOTIFICATION_ACTIVATION_CALLBACK *self)
{
    const ULONG refs = (ULONG)InterlockedDecrement(&self->refs);
    if (refs == 0) {
        HeapFree(GetProcessHeap(), 0, self);
    }
    return refs;
}

/* Downloads and launches an update away from the COM activation callback thread. */
static DWORD WINAPI activation_download_thread_proc(LPVOID param)
{
    (void)param;

    char installer_path[MAX_PATH];
    if (lp_update_download_latest(installer_path, sizeof(installer_path)) != LP_OK) {
        LP_ERROR("toast update download failed");
        return 1;
    }
    LP_INFO("toast update downloaded to %s", installer_path);
    if ((INT_PTR)ShellExecuteA(NULL, "open", installer_path, NULL, NULL, SW_SHOWNORMAL) <= 32) {
        DeleteFileA(installer_path);
        LP_ERROR("toast installer launch failed");
        return 1;
    }
    return 0;
}

/* Stops the local-server message loop after activation has been dispatched. */
static HRESULT request_activator_shutdown(void)
{
    if (activator_thread_id == 0) {
        return E_UNEXPECTED;
    }
    if (!PostThreadMessageA(activator_thread_id, WM_QUIT, 0, 0)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

/* Handles the toast protocol argument and requests activator shutdown. */
static HRESULT STDMETHODCALLTYPE callback_activate(LP_NOTIFICATION_ACTIVATION_CALLBACK *self,
                                                   LPCWSTR app_user_model_id, LPCWSTR invoked_args,
                                                   const LP_NOTIFICATION_USER_INPUT_DATA *data,
                                                   ULONG data_count)
{
    (void)self;
    (void)app_user_model_id;
    (void)data;
    (void)data_count;
    LP_INFO("toast activation received: args=%ls", invoked_args != NULL ? invoked_args : L"(null)");
    HRESULT result = S_OK;
    if (invoked_args != NULL && lstrcmpW(invoked_args, L"linkpulse://download-update") == 0) {
        HANDLE download_thread =
            CreateThread(NULL, 0, activation_download_thread_proc, NULL, 0, NULL);
        if (download_thread == NULL) {
            result = E_FAIL;
        } else {
            CloseHandle(download_thread);
        }
    }

    const HRESULT shutdown_result = request_activator_shutdown();
    if (FAILED(result)) {
        return result;
    }
    return shutdown_result;
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

/* Exposes the IClassFactory interfaces required by COM local-server registration. */
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

/* Retains the process-lifetime class factory reference. */
static ULONG STDMETHODCALLTYPE factory_add_ref(LP_ACTIVATOR_FACTORY *self)
{
    return (ULONG)InterlockedIncrement(&self->refs);
}

/* Drops a class-factory reference; the stack-owned factory remains process-lived. */
static ULONG STDMETHODCALLTYPE factory_release(LP_ACTIVATOR_FACTORY *self)
{
    return (ULONG)InterlockedDecrement(&self->refs);
}

/* Creates a callback object for the COM toast activation request. */
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

/* Accepts COM server-lock requests; activation lifetime is controlled by the message loop. */
static HRESULT STDMETHODCALLTYPE factory_lock_server(LP_ACTIVATOR_FACTORY *self, BOOL lock)
{
    (void)self;
    (void)lock;
    return S_OK;
}

static const LP_ACTIVATOR_FACTORY_VTBL factory_vtbl = {
    factory_query_interface, factory_add_ref, factory_release, factory_create_instance,
    factory_lock_server};

/* Registers the COM class factory and services toast activations until WM_QUIT. */
int lp_win32_run_toast_activator(void)
{
    HRESULT result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(result)) {
        return 1;
    }

    LP_ACTIVATOR_FACTORY factory = {&factory_vtbl, 1};
    DWORD cookie = 0;
    MSG message;
    activator_thread_id = GetCurrentThreadId();
    PeekMessageA(&message, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    result = CoRegisterClassObject(&CLSID_LinkPulseToastActivator, (IUnknown *)&factory,
                                   CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &cookie);
    if (FAILED(result)) {
        activator_thread_id = 0;
        CoUninitialize();
        return 1;
    }

    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    activator_thread_id = 0;
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
