#define COBJMACROS
#if defined(__MINGW32__)
/* Must precede <windows.h>: mingw-w64's guiddef.h fixes the DEFINE_GUID
   expansion (declare-only vs. define) the first time it is pulled in, so
   INITGUID has to be set before windows.h, not just before xmllite.h.
   mingw-w64's xmllite.h only defines (rather than declares) IID_IXmlReader/
   IID_IXmlWriter when INITGUID is set, and unlike MSVC's uuid.lib, mingw's
   libuuid.a does not provide them, which otherwise fails at link time. */
#define INITGUID
#endif

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

/* Bump when the on-disk schema changes, and add a migration step below (see
   the comment above load_store) so older files upgrade in place at startup. */
#define LP_DEVICE_STORE_SCHEMA_VERSION 2
#define LP_DEVICE_STORE_PATH_MAX MAX_PATH
/* Key used for devices/network context seen without a resolved gateway MAC,
   and for legacy (pre-v2, flat) records migrated from a single global list. */
#define LP_DEVICE_STORE_UNKNOWN_NETWORK_KEY ""

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

/* One physical network (identified by its gateway's MAC address), so the same
   laptop connecting to different routers - home, work, a relative's house -
   keeps separate device lists and ISP identities instead of one global mix. */
typedef struct {
    char gateway_mac[LP_MAC_STR_MAX];
    char gateway_hostname[LP_HOSTNAME_MAX];
    char gateway_vendor[LP_VENDOR_MAX];
    lp_isp_info_t isp;
    unsigned long long first_seen;
    unsigned long long last_seen;
    lp_device_record_t *devices;
    size_t device_count;
    size_t device_capacity;
} lp_network_record_t;

typedef struct {
    char path[LP_DEVICE_STORE_PATH_MAX];
    lp_network_record_t *networks;
    size_t network_count;
    size_t network_capacity;
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

static lp_network_record_t *find_network(lp_device_store_t *store, const char *gateway_mac)
{
    for (size_t i = 0; i < store->network_count; ++i) {
        if (_stricmp(store->networks[i].gateway_mac, gateway_mac) == 0) {
            return &store->networks[i];
        }
    }
    return NULL;
}

/* Grows without a fixed cap: occasional travel between networks (home, work,
   a relative's house) must never silently stop being tracked. */
static lp_network_record_t *find_or_create_network(lp_device_store_t *store, const char *gateway_mac)
{
    lp_network_record_t *existing = find_network(store, gateway_mac);
    if (existing != NULL) {
        return existing;
    }
    if (store->network_count >= store->network_capacity) {
        const size_t new_capacity = store->network_capacity == 0 ? 4 : store->network_capacity * 2;
        lp_network_record_t *grown = realloc(store->networks, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        store->networks = grown;
        store->network_capacity = new_capacity;
    }
    lp_network_record_t *network = &store->networks[store->network_count++];
    memset(network, 0, sizeof(*network));
    copy_text(network->gateway_mac, sizeof(network->gateway_mac), gateway_mac);
    return network;
}

static lp_device_record_t *find_device(lp_network_record_t *network, const char *mac)
{
    for (size_t i = 0; i < network->device_count; ++i) {
        if (_stricmp(network->devices[i].mac, mac) == 0) {
            return &network->devices[i];
        }
    }
    return NULL;
}

static lp_device_record_t *find_or_create_device(lp_network_record_t *network, const char *mac)
{
    lp_device_record_t *existing = find_device(network, mac);
    if (existing != NULL) {
        return existing;
    }
    if (network->device_count >= network->device_capacity) {
        const size_t new_capacity = network->device_capacity == 0 ? 8 : network->device_capacity * 2;
        lp_device_record_t *grown = realloc(network->devices, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        network->devices = grown;
        network->device_capacity = new_capacity;
    }
    lp_device_record_t *device = &network->devices[network->device_count++];
    memset(device, 0, sizeof(*device));
    copy_text(device->mac, sizeof(device->mac), mac);
    return device;
}

static void apply_record(const lp_device_record_t *record, lp_neighbor_t *neighbor)
{
    if (record->label[0] != '\0') copy_text(neighbor->label, sizeof(neighbor->label), record->label);
    /* Falls back to a previously learned name/vendor when this poll's live
       resolution came back empty (e.g. a transient reverse-DNS failure). */
    if (neighbor->hostname[0] == '\0' && record->hostname[0] != '\0') {
        copy_text(neighbor->hostname, sizeof(neighbor->hostname), record->hostname);
    }
    if (neighbor->vendor[0] == '\0' && record->vendor[0] != '\0') {
        copy_text(neighbor->vendor, sizeof(neighbor->vendor), record->vendor);
    }
    if (record->icon[0] != '\0') copy_text(neighbor->icon, sizeof(neighbor->icon), record->icon);
    if (record->trusted) neighbor->trusted = true;
    if (record->has_device_type) {
        neighbor->device_type = record->device_type;
        neighbor->has_device_type_override = true;
    }
}

static void read_device_attributes(IXmlReader *reader, lp_device_record_t *record)
{
    if (IXmlReader_MoveToFirstAttribute(reader) != S_OK) {
        return;
    }
    /* Bounded by attribute count, but capped defensively in case a future
       schema change or malformed file confuses the attribute cursor. */
    for (int guard = 0; guard < 64; ++guard) {
        const WCHAR *name = NULL;
        const WCHAR *value = NULL;
        if (FAILED(IXmlReader_GetQualifiedName(reader, &name, NULL)) ||
            FAILED(IXmlReader_GetValue(reader, &value, NULL))) {
            if (IXmlReader_MoveToNextAttribute(reader) != S_OK) break;
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
        if (IXmlReader_MoveToNextAttribute(reader) != S_OK) {
            break;
        }
    }
    (void)IXmlReader_MoveToElement(reader);
}

static void read_network_attributes(IXmlReader *reader, lp_network_record_t *network)
{
    if (IXmlReader_MoveToFirstAttribute(reader) != S_OK) {
        return;
    }
    for (int guard = 0; guard < 64; ++guard) {
        const WCHAR *name = NULL;
        const WCHAR *value = NULL;
        if (FAILED(IXmlReader_GetQualifiedName(reader, &name, NULL)) ||
            FAILED(IXmlReader_GetValue(reader, &value, NULL))) {
            if (IXmlReader_MoveToNextAttribute(reader) != S_OK) break;
            continue;
        }
        char name_utf8[64];
        char value_utf8[LP_HOSTNAME_MAX];
        if (!wide_to_utf8(name, name_utf8, sizeof(name_utf8)) ||
            !wide_to_utf8(value, value_utf8, sizeof(value_utf8))) {
            continue;
        }
        if (strcmp(name_utf8, "gateway-hostname") == 0)
            copy_text(network->gateway_hostname, sizeof(network->gateway_hostname), value_utf8);
        else if (strcmp(name_utf8, "gateway-vendor") == 0)
            copy_text(network->gateway_vendor, sizeof(network->gateway_vendor), value_utf8);
        else if (strcmp(name_utf8, "isp") == 0)
            copy_text(network->isp.isp, sizeof(network->isp.isp), value_utf8);
        else if (strcmp(name_utf8, "isp-asn") == 0)
            copy_text(network->isp.asn, sizeof(network->isp.asn), value_utf8);
        else if (strcmp(name_utf8, "isp-public-ip") == 0)
            copy_text(network->isp.public_ip, sizeof(network->isp.public_ip), value_utf8);
        else if (strcmp(name_utf8, "isp-hostname") == 0)
            copy_text(network->isp.hostname, sizeof(network->isp.hostname), value_utf8);
        else if (strcmp(name_utf8, "isp-city") == 0)
            copy_text(network->isp.city, sizeof(network->isp.city), value_utf8);
        else if (strcmp(name_utf8, "isp-region") == 0)
            copy_text(network->isp.region, sizeof(network->isp.region), value_utf8);
        else if (strcmp(name_utf8, "isp-country") == 0)
            copy_text(network->isp.country, sizeof(network->isp.country), value_utf8);
        else if (strcmp(name_utf8, "isp-postal") == 0)
            copy_text(network->isp.postal, sizeof(network->isp.postal), value_utf8);
        else if (strcmp(name_utf8, "isp-timezone") == 0)
            copy_text(network->isp.timezone, sizeof(network->isp.timezone), value_utf8);
        else if (strcmp(name_utf8, "isp-loc") == 0)
            copy_text(network->isp.loc, sizeof(network->isp.loc), value_utf8);
        else if (strcmp(name_utf8, "firstSeen") == 0) {
            network->first_seen = _strtoui64(value_utf8, NULL, 10);
        } else if (strcmp(name_utf8, "lastSeen") == 0) {
            network->last_seen = _strtoui64(value_utf8, NULL, 10);
        }
        /* "gateway-mac" is read separately by the caller before this record
           exists, since it is the lookup/creation key. */
        if (IXmlReader_MoveToNextAttribute(reader) != S_OK) {
            break;
        }
    }
    (void)IXmlReader_MoveToElement(reader);
}

/* Reads a single named attribute's value as a UTF-8 string, if present. */
static bool read_attribute_by_name(IXmlReader *reader, const wchar_t *name, char *out, size_t cap)
{
    out[0] = '\0';
    if (IXmlReader_MoveToAttributeByName(reader, name, NULL) != S_OK) {
        return false;
    }
    const WCHAR *value = NULL;
    const bool ok =
        SUCCEEDED(IXmlReader_GetValue(reader, &value, NULL)) && wide_to_utf8(value, out, cap);
    (void)IXmlReader_MoveToElement(reader);
    return ok;
}

/*
 * Schema versioning and migration:
 *
 * The root element's "version" attribute (absent/1 on the original flat
 * <devices><device .../></devices> layout that shipped in v0.2.0) selects how
 * this function interprets the file. To add a future schema change, bump
 * LP_DEVICE_STORE_SCHEMA_VERSION, keep this function able to read the OLD
 * shape into the CURRENT in-memory structures (as done below for v1), and set
 * *migrated so lp_win32_device_store_open() rewrites the file once at startup
 * in the new format instead of waiting for the next observed device.
 */
static void load_store(lp_device_store_t *store, bool *migrated)
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

    bool root_seen = false;
    int declared_version = 1;
    lp_network_record_t *current_network = NULL;
    XmlNodeType node_type;
    /* Hard-capped rather than an unconditional while(Read()): guarantees
       termination even if a reader implementation ever fails to report
       end-of-document via node_type, which previously caused the tray to
       hang forever (100% CPU, tray icon never created) parsing a real file. */
    for (int guard = 0; guard < 100000; ++guard) {
        const HRESULT read_result = IXmlReader_Read(reader, &node_type);
        if (read_result == S_FALSE || node_type == XmlNodeType_None) {
            break; /* end of document */
        }
        if (FAILED(read_result)) {
            break;
        }
        if (node_type != XmlNodeType_Element) continue;
        const WCHAR *name = NULL;
        if (FAILED(IXmlReader_GetQualifiedName(reader, &name, NULL)) || name == NULL) continue;
        char name_utf8[64];
        if (!wide_to_utf8(name, name_utf8, sizeof(name_utf8))) continue;

        if (!root_seen) {
            root_seen = true;
            char version_text[16];
            if (read_attribute_by_name(reader, L"version", version_text, sizeof(version_text))) {
                declared_version = atoi(version_text);
            }
            continue;
        }

        if (strcmp(name_utf8, "network") == 0) {
            char gateway_mac[LP_MAC_STR_MAX];
            if (!read_attribute_by_name(reader, L"gateway-mac", gateway_mac, sizeof(gateway_mac))) {
                gateway_mac[0] = '\0';
            }
            current_network = find_or_create_network(store, gateway_mac);
            if (current_network != NULL) {
                read_network_attributes(reader, current_network);
            }
        } else if (strcmp(name_utf8, "device") == 0) {
            /* A device with no enclosing <network> only occurs in a v1 file
               (a flat list at the document root); bucket it under the
               "unknown network" key so nothing already learned is lost. */
            if (current_network == NULL) {
                current_network =
                    find_or_create_network(store, LP_DEVICE_STORE_UNKNOWN_NETWORK_KEY);
            }
            if (current_network == NULL) continue;
            lp_device_record_t header = {0};
            read_device_attributes(reader, &header);
            if (header.mac[0] == '\0') continue;
            lp_device_record_t *device = find_or_create_device(current_network, header.mac);
            if (device != NULL) {
                *device = header;
            }
        }
    }
    IXmlReader_Release(reader);
    IStream_Release(stream);
    if (uninitialize) CoUninitialize();

    if (declared_version < LP_DEVICE_STORE_SCHEMA_VERSION) {
        LP_INFO("migrating device store from schema v%d to v%d", declared_version,
                LP_DEVICE_STORE_SCHEMA_VERSION);
        *migrated = true;
    }
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
    char temp_path[LP_DEVICE_STORE_PATH_MAX + 8];
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
    (void)IXmlWriter_WriteStartElement(writer, NULL, L"linkpulse-devices", NULL);
    write_attribute(writer, "version", "2");
    for (size_t i = 0; i < store->network_count; ++i) {
        const lp_network_record_t *network = &store->networks[i];
        char number[32];
        (void)IXmlWriter_WriteStartElement(writer, NULL, L"network", NULL);
        write_attribute(writer, "gateway-mac", network->gateway_mac);
        write_attribute(writer, "gateway-hostname", network->gateway_hostname);
        write_attribute(writer, "gateway-vendor", network->gateway_vendor);
        write_attribute(writer, "isp", network->isp.isp);
        write_attribute(writer, "isp-asn", network->isp.asn);
        write_attribute(writer, "isp-public-ip", network->isp.public_ip);
        write_attribute(writer, "isp-hostname", network->isp.hostname);
        write_attribute(writer, "isp-city", network->isp.city);
        write_attribute(writer, "isp-region", network->isp.region);
        write_attribute(writer, "isp-country", network->isp.country);
        write_attribute(writer, "isp-postal", network->isp.postal);
        write_attribute(writer, "isp-timezone", network->isp.timezone);
        write_attribute(writer, "isp-loc", network->isp.loc);
        _ui64toa(network->first_seen, number, 10);
        write_attribute(writer, "firstSeen", number);
        _ui64toa(network->last_seen, number, 10);
        write_attribute(writer, "lastSeen", number);
        for (size_t j = 0; j < network->device_count; ++j) {
            const lp_device_record_t *record = &network->devices[j];
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
    bool migrated = false;
    load_store(store, &migrated);
    if (migrated) {
        save_store(store);
    }
    *store_out = store;
    LP_DEBUG("device store opened: networks=%llu", (unsigned long long)store->network_count);
    return 0;
}

void lp_win32_device_store_apply(void *opaque, const char *gateway_mac, lp_neighbor_t *neighbor)
{
    lp_device_store_t *store = opaque;
    if (store == NULL || neighbor == NULL || neighbor->mac[0] == '\0') return;
    lp_network_record_t *network =
        find_network(store, gateway_mac != NULL ? gateway_mac : LP_DEVICE_STORE_UNKNOWN_NETWORK_KEY);
    if (network == NULL) return;
    const lp_device_record_t *device = find_device(network, neighbor->mac);
    if (device != NULL) apply_record(device, neighbor);
}

void lp_win32_device_store_observe(void *opaque, const lp_network_context_t *network_context,
                                    const lp_neighbor_t *neighbor)
{
    lp_device_store_t *store = opaque;
    if (store == NULL || neighbor == NULL || neighbor->mac[0] == '\0') return;
    const char *gateway_mac =
        (network_context != NULL && network_context->gateway_mac != NULL)
            ? network_context->gateway_mac
            : LP_DEVICE_STORE_UNKNOWN_NETWORK_KEY;
    lp_network_record_t *network = find_or_create_network(store, gateway_mac);
    if (network == NULL) return;
    if (network->first_seen == 0) network->first_seen = now_seconds();
    network->last_seen = now_seconds();
    if (network_context != NULL) {
        if (network_context->gateway_hostname != NULL && network_context->gateway_hostname[0] != '\0') {
            copy_text(network->gateway_hostname, sizeof(network->gateway_hostname),
                     network_context->gateway_hostname);
        }
        if (network_context->gateway_vendor != NULL && network_context->gateway_vendor[0] != '\0') {
            copy_text(network->gateway_vendor, sizeof(network->gateway_vendor),
                     network_context->gateway_vendor);
        }
        if (network_context->isp != NULL && network_context->isp->isp[0] != '\0') {
            network->isp = *network_context->isp;
        }
    }

    lp_device_record_t *device = find_or_create_device(network, neighbor->mac);
    if (device == NULL) return;
    if (device->first_seen == 0) device->first_seen = now_seconds();
    copy_text(device->hostname, sizeof(device->hostname), neighbor->hostname);
    copy_text(device->vendor, sizeof(device->vendor), neighbor->vendor);
    if (!device->has_device_type && neighbor->device_type != LP_DEVICE_UNKNOWN) {
        device->device_type = neighbor->device_type;
        device->has_device_type = true;
    }
    device->last_seen = now_seconds();
    save_store(store);
}

void lp_win32_device_store_close(void *opaque)
{
    lp_device_store_t *store = opaque;
    if (store == NULL) return;
    for (size_t i = 0; i < store->network_count; ++i) {
        free(store->networks[i].devices);
    }
    free(store->networks);
    free(store);
}
