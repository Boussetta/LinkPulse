#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define COBJMACROS
#define INITGUID
#define WIDL_using_Windows_Data_Xml_Dom
#define WIDL_using_Windows_UI_Notifications
#include <roapi.h>
#include <shobjidl.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <windows.h>
#include <winstring.h>

#define LP_APP_USER_MODEL_ID L"Boussetta.LinkPulse"

static HRESULT make_hstring(PCWSTR value, HSTRING *out)
{
    return WindowsCreateString(value, (UINT32)wcslen(value), out);
}

bool lp_win32_show_update_toast(const char *version)
{
    if (version == NULL) {
        return false;
    }

    wchar_t xml[512];
    const int written = swprintf(xml, sizeof(xml) / sizeof(xml[0]),
                                 L"<toast><visual><binding template=\"ToastText02\">"
                                 L"<text id=\"1\">LinkPulse update</text>"
                                 L"<text id=\"2\">Version %hs is available. Open LinkPulse to download it.</text>"
                                 L"</binding></visual></toast>",
                                 version);
    if (written < 0 || (size_t)written >= sizeof(xml) / sizeof(xml[0])) {
        return false;
    }

    HRESULT result = RoInitialize(RO_INIT_MULTITHREADED);
    const bool initialized_here = SUCCEEDED(result);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
        return false;
    }

    HSTRING class_id = NULL;
    HSTRING xml_text = NULL;
    HSTRING app_id = NULL;
    IInspectable *xml_inspectable = NULL;
    __x_ABI_CWindows_CData_CXml_CDom_CIXmlDocumentIO *xml_io = NULL;
    __x_ABI_CWindows_CData_CXml_CDom_CIXmlDocument *document = NULL;
    __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics *manager = NULL;
    __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationFactory *factory = NULL;
    __x_ABI_CWindows_CUI_CNotifications_CIToastNotifier *notifier = NULL;
    __x_ABI_CWindows_CUI_CNotifications_CIToastNotification *notification = NULL;

    result = make_hstring(L"Windows.Data.Xml.Dom.XmlDocument", &class_id);
    if (SUCCEEDED(result)) {
        result = RoActivateInstance(class_id, &xml_inspectable);
    }
    if (SUCCEEDED(result)) {
        result = IInspectable_QueryInterface(
            xml_inspectable, &IID_IXmlDocumentIO, (void **)&xml_io);
    }
    if (SUCCEEDED(result)) {
        result = make_hstring(xml, &xml_text);
    }
    if (SUCCEEDED(result)) {
        result = IXmlDocumentIO_LoadXml(xml_io, xml_text);
    }
    if (SUCCEEDED(result)) {
        result = IXmlDocumentIO_QueryInterface(xml_io, &IID_IXmlDocument, (void **)&document);
    }
    if (SUCCEEDED(result)) {
        result = make_hstring(L"Windows.UI.Notifications.ToastNotificationManager", &class_id);
    }
    if (SUCCEEDED(result)) {
        result = RoGetActivationFactory(class_id, &IID_IToastNotificationManagerStatics,
                                        (void **)&manager);
    }
    if (SUCCEEDED(result)) {
        result = make_hstring(L"Boussetta.LinkPulse", &app_id);
    }
    if (SUCCEEDED(result)) {
        result = IToastNotificationManagerStatics_CreateToastNotifierWithId(
            manager, app_id, &notifier);
    }
    if (SUCCEEDED(result)) {
        result = make_hstring(L"Windows.UI.Notifications.ToastNotification", &class_id);
    }
    if (SUCCEEDED(result)) {
        result = RoGetActivationFactory(class_id, &IID_IToastNotificationFactory,
                                        (void **)&factory);
    }
    if (SUCCEEDED(result)) {
        result = IToastNotificationFactory_CreateToastNotification(factory, document,
                                                                    &notification);
    }
    if (SUCCEEDED(result)) {
        result = IToastNotifier_Show(notifier, notification);
    }

    if (notification != NULL) {
        IToastNotification_Release(notification);
    }
    if (notifier != NULL) {
        IToastNotifier_Release(notifier);
    }
    if (factory != NULL) {
        IToastNotificationFactory_Release(factory);
    }
    if (manager != NULL) {
        IToastNotificationManagerStatics_Release(manager);
    }
    if (document != NULL) {
        IXmlDocument_Release(document);
    }
    if (xml_io != NULL) {
        IXmlDocumentIO_Release(xml_io);
    }
    if (xml_inspectable != NULL) {
        IInspectable_Release(xml_inspectable);
    }
    if (app_id != NULL) {
        WindowsDeleteString(app_id);
    }
    if (xml_text != NULL) {
        WindowsDeleteString(xml_text);
    }
    if (class_id != NULL) {
        WindowsDeleteString(class_id);
    }
    if (initialized_here) {
        RoUninitialize();
    }
    return SUCCEEDED(result);
}

void lp_win32_set_app_user_model_id(void)
{
    SetCurrentProcessExplicitAppUserModelID(LP_APP_USER_MODEL_ID);
}
