#define COBJMACROS

#include "linkpulse/device_store.h"
#include "linkpulse/log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <windows.h>
#include <shlwapi.h>
#include <xmllite.h>

#define LP_DEVICE_STORE_MAX 256
#define LP_DEVICE_STORE_PATH_MAX MAX_PATH

typedef struct {
    char mac[LP_MAC_STR_MAX];
    char label[LP_HOSTNAME_MAX];
    char hostname[LP_HOSTNAME_MAX];
    char vendor[LP_VENDOR_MAX];
    char icon[LP_VENDOR_MAX];
    lp_device_type_t device_type;
    bool has_device_type;
    bool trusted;
    unsigned long long first_seen;
    unsigned long long last_seen;
} lp_device_record_t;

typedef struct {
    char path[LP_DEVICE_STORE_PATH_MAX];
    lp_device_record_t records[LP_DEVICE_STORE_MAX];
    size_t count;
} lp_device_store_t;

static void copy_text(char *out, size_t cap, const char *value)
{
    if (out == NULL || cap == 0) {
        return;
    }
    snprintf(out, cap, "%s", value != NULL ? value : "");
}

static bool utf8_to_wide(const char *value, wchar_t *out, size_t cap)
{
    if (value == NULL || out == NULL || cap == 0) {
        return false;
    }
    const int written = MultiByteToWideChar(CP_UTF8, 0, value, -1, out, (int)cap);
    return written > 0;
}

static bool wide_to_utf8(const WCHAR *value, char *out, size_t cap)
{
    if (value == NULL || out == NULL || cap == 0) {
        return false;
    }
    const int written = WideCharToMultiByte(CP_UTF8, 0, value, -1, out, (int)cap, NULL, NULL);
    if (written <= 0) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static bool store_path(char *out, size_t cap)
{
    WCHAR local_app_data[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data,
                                                  ARRAYSIZE(local_app_data));
    if (length == 0 || length >= ARRAYSIZE(local_app_data)) {
        return false;
    }
    WCHAR directory[MAX_PATH];
    if (swprintf(directory, ARRAYSIZE(directory), L"%ls\\LinkPulse", local_app_data) < 0) {
        return false;
    }
    if (CreateDirectoryW(directory, NULL) == 0 && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    WCHAR path[MAX_PATH];
    if (swprintf(path, ARRAYSIZE(path), L"%ls\\devices.xml", directory) < 0) {
        return false;
    }
    return WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)cap, NULL, NULL) > 0;
}

static lp_device_type_t type_from_text(const char *value)
{
    if (strcmp(value, "laptop") == 0) return LP_DEVICE_LAPTOP;
    if (strcmp(value, "mobile") == 0) return LP_DEVICE_MOBILE;
    if (strcmp(value, "smartwatch") == 0) return LP_DEVICE_SMARTWATCH;
    if (strcmp(value, "printer") == 0) return LP_DEVICE_PRINTER;
    if (strcmp(value, "television") == 0) return LP_DEVICE_TELEVISION;
    if (strcmp(value, "router") == 0) return LP_DEVICE_ROUTER;
    if (strcmp(value, "desktop") == 0) return LP_DEVICE_DESKTOP;
    return LP_DEVICE_UNKNOWN;
}

static const char *type_to_text(lp_device_type_t type)
{
    switch (type) {
    case LP_DEVICE_LAPTOP: return "laptop";
    case LP_DEVICE_MOBILE: return "mobile";
    case LP_DEVICE_SMARTWATCH: return "smartwatch";
    case LP_DEVICE_PRINTER: return "printer";
    case LP_DEVICE_TELEVISION: return "television";
    case LP_DEVICE_ROUTER: return "router";
    case LP_DEVICE_DESKTOP: return "desktop";
    default: return "unknown";
    }
}

static unsigned long long now_seconds(void)
{
    const time_t now = time(NULL);
    return now > 0 ? (unsigned long long)now : 0;
}

static long find_record(const lp_device_store_t *store, const char *mac)
{
    for (size_t i = 0; i < store->count; ++i) {
        if (_stricmp(store->records[i].mac, mac) == 0) {
            return (long)i;
        }
    }
    return -1;
}

static void apply_record(const lp_device_record_t *record, lp_neighbor_t *neighbor)
{
    if (record->label[0] != '\0') copy_text(neighbor->label, sizeof(neighbor->label), record->label);
    if (record->icon[0] != '\0') copy_text(neighbor->icon, sizeof(neighbor->icon), record->icon);
    if (record->trusted) neighbor->trusted = true;
    if (record->has_device_type) {
        neighbor->device_type = record->device_type;
        neighbor->has_device_type_override = true;
    }
}

static void read_device_attributes(IXmlReader *reader, lp_device_record_t *record)
{
    if (FAILED(IXmlReader_MoveToFirstAttribute(reader))) {
        return;
    }
    do {
        const WCHAR *name = NULL;
        const WCHAR *value = NULL;
        if (FAILED(IXmlReader_GetQualifiedName(reader, &name, NULL)) ||
            FAILED(IXmlReader_GetValue(reader, &value, NULL))) {
            continue;
        }
        char name_utf8[64];
        char value_utf8[LP_HOSTNAME_MAX];
        if (!wide_to_utf8(name, name_utf8, sizeof(name_utf8)) ||
            !wide_to_utf8(value, value_utf8, sizeof(value_utf8))) {
            continue;
        }
        if (strcmp(name_utf8, "mac") == 0) copy_text(record->mac, sizeof(record->mac), value_utf8);
        else if (strcmp(name_utf8, "label") == 0) copy_text(record->label, sizeof(record->label), value_utf8);
        else if (strcmp(name_utf8, "hostname") == 0) copy_text(record->hostname, sizeof(record->hostname), value_utf8);
        else if (strcmp(name_utf8, "vendor") == 0) copy_text(record->vendor, sizeof(record->vendor), value_utf8);
        else if (strcmp(name_utf8, "icon") == 0) copy_text(record->icon, sizeof(record->icon), value_utf8);
        else if (strcmp(name_utf8, "type") == 0) {
            record->device_type = type_from_text(value_utf8);
            record->has_device_type = record->device_type != LP_DEVICE_UNKNOWN;
        } else if (strcmp(name_utf8, "trusted") == 0) {
            record->trusted = strcmp(value_utf8, "true") == 0 || strcmp(value_utf8, "1") == 0;
        } else if (strcmp(name_utf8, "firstSeen") == 0) {
            record->first_seen = _strtoui64(value_utf8, NULL, 10);
        } else if (strcmp(name_utf8, "lastSeen") == 0) {
            record->last_seen = _strtoui64(value_utf8, NULL, 10);
        }
    } while (SUCCEEDED(IXmlReader_MoveToNextAttribute(reader)));
    (void)IXmlReader_MoveToElement(reader);
}

static void load_store(lp_device_store_t *store)
{
    const HRESULT com_result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(com_result);
    WCHAR path[MAX_PATH];
    if (!utf8_to_wide(store->path, path, ARRAYSIZE(path))) {
        if (uninitialize) CoUninitialize();
        return;
    }
    IStream *stream = NULL;
    if (FAILED(SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, NULL,
                                      &stream))) {
        if (uninitialize) CoUninitialize();
        return;
    }
    IXmlReader *reader = NULL;
    if (FAILED(CreateXmlReader(&IID_IXmlReader, (void **)&reader, NULL)) ||
        FAILED(IXmlReader_SetInput(reader, (IUnknown *)stream))) {
        if (reader != NULL) IXmlReader_Release(reader);
        IStream_Release(stream);
        if (uninitialize) CoUninitialize();
        return;
    }
    XmlNodeType node_type;
    while (SUCCEEDED(IXmlReader_Read(reader, &node_type)) && node_type != XmlNodeType_None) {
        if (node_type != XmlNodeType_Element) continue;
        const WCHAR *name = NULL;
        if (FAILED(IXmlReader_GetQualifiedName(reader, &name, NULL)) || name == NULL) continue;
        char name_utf8[64];
        if (!wide_to_utf8(name, name_utf8, sizeof(name_utf8)) || strcmp(name_utf8, "device") != 0) continue;
        if (store->count >= LP_DEVICE_STORE_MAX) continue;
        lp_device_record_t *record = &store->records[store->count];
        memset(record, 0, sizeof(*record));
        read_device_attributes(reader, record);
        if (record->mac[0] != '\0') ++store->count;
    }
    IXmlReader_Release(reader);
    IStream_Release(stream);
    if (uninitialize) CoUninitialize();
}

static void write_attribute(IXmlWriter *writer, const char *name, const char *value)
{
    wchar_t name_wide[64];
    wchar_t value_wide[LP_HOSTNAME_MAX];
    if (utf8_to_wide(name, name_wide, ARRAYSIZE(name_wide)) &&
        utf8_to_wide(value, value_wide, ARRAYSIZE(value_wide))) {
        (void)IXmlWriter_WriteAttributeString(writer, NULL, name_wide, NULL, value_wide);
    }
}

static void save_store(const lp_device_store_t *store)
{
    const HRESULT com_result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(com_result);
    char temp_path[LP_DEVICE_STORE_PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", store->path);
    WCHAR temp_wide[MAX_PATH];
    if (!utf8_to_wide(temp_path, temp_wide, ARRAYSIZE(temp_wide))) {
        if (uninitialize) CoUninitialize();
        return;
    }
    IStream *stream = NULL;
    if (FAILED(SHCreateStreamOnFileEx(temp_wide, STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_NONE,
                                      FILE_ATTRIBUTE_NORMAL, TRUE, NULL, &stream))) {
        if (uninitialize) CoUninitialize();
        return;
    }
    IXmlWriter *writer = NULL;
    if (FAILED(CreateXmlWriter(&IID_IXmlWriter, (void **)&writer, NULL)) ||
        FAILED(IXmlWriter_SetOutput(writer, (IUnknown *)stream))) {
        if (writer != NULL) IXmlWriter_Release(writer);
        IStream_Release(stream);
        DeleteFileA(temp_path);
        if (uninitialize) CoUninitialize();
        return;
    }
    (void)IXmlWriter_WriteStartDocument(writer, XmlStandalone_Omit);
    (void)IXmlWriter_WriteStartElement(writer, NULL, L"devices", NULL);
    for (size_t i = 0; i < store->count; ++i) {
        const lp_device_record_t *record = &store->records[i];
        char number[32];
        (void)IXmlWriter_WriteStartElement(writer, NULL, L"device", NULL);
        write_attribute(writer, "mac", record->mac);
        write_attribute(writer, "label", record->label);
        write_attribute(writer, "hostname", record->hostname);
        write_attribute(writer, "vendor", record->vendor);
        write_attribute(writer, "type", type_to_text(record->device_type));
        write_attribute(writer, "icon", record->icon);
        write_attribute(writer, "trusted", record->trusted ? "true" : "false");
        _ui64toa(record->first_seen, number, 10);
        write_attribute(writer, "firstSeen", number);
        _ui64toa(record->last_seen, number, 10);
        write_attribute(writer, "lastSeen", number);
        (void)IXmlWriter_WriteEndElement(writer);
    }
    (void)IXmlWriter_WriteEndElement(writer);
    (void)IXmlWriter_WriteEndDocument(writer);
    (void)IXmlWriter_Flush(writer);
    IXmlWriter_Release(writer);
    IStream_Release(stream);
    if (!MoveFileExA(temp_path, store->path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temp_path);
    }
    if (uninitialize) CoUninitialize();
}

int lp_win32_device_store_open(void **store_out)
{
    if (store_out == NULL) return 1;
    *store_out = NULL;
    lp_device_store_t *store = calloc(1, sizeof(*store));
    if (store == NULL || !store_path(store->path, sizeof(store->path))) {
        free(store);
        return 1;
    }
    load_store(store);
    *store_out = store;
    LP_DEBUG("device store opened: records=%zu", store->count);
    return 0;
}

void lp_win32_device_store_apply(void *opaque, lp_neighbor_t *neighbor)
{
    lp_device_store_t *store = opaque;
    if (store == NULL || neighbor == NULL || neighbor->mac[0] == '\0') return;
    const long index = find_record(store, neighbor->mac);
    if (index >= 0) apply_record(&store->records[index], neighbor);
}

void lp_win32_device_store_observe(void *opaque, const lp_neighbor_t *neighbor)
{
    lp_device_store_t *store = opaque;
    if (store == NULL || neighbor == NULL || neighbor->mac[0] == '\0') return;
    long index = find_record(store, neighbor->mac);
    if (index < 0) {
        if (store->count >= LP_DEVICE_STORE_MAX) return;
        index = (long)store->count++;
        memset(&store->records[index], 0, sizeof(store->records[index]));
        copy_text(store->records[index].mac, sizeof(store->records[index].mac), neighbor->mac);
        store->records[index].first_seen = now_seconds();
    }
    lp_device_record_t *record = &store->records[index];
    copy_text(record->hostname, sizeof(record->hostname), neighbor->hostname);
    copy_text(record->vendor, sizeof(record->vendor), neighbor->vendor);
    if (!record->has_device_type && neighbor->device_type != LP_DEVICE_UNKNOWN) {
        record->device_type = neighbor->device_type;
        record->has_device_type = true;
    }
    record->last_seen = now_seconds();
    save_store(store);
}

void lp_win32_device_store_close(void *opaque)
{
    lp_device_store_t *store = opaque;
    if (store == NULL) return;
    free(store);
}
