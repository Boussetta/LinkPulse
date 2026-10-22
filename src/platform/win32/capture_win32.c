#include "linkpulse/capture.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LP_PCAP_ERRBUF_SIZE 256
/* Enough for Ethernet + one VLAN tag + an IPv6 header; payload bytes are never captured.
   header->len still reports the full on-wire length for byte counting. */
#define LP_CAPTURE_SNAPLEN 96

typedef struct pcap pcap_t;
typedef struct pcap_addr {
    struct pcap_addr *next;
    struct sockaddr *addr;
    struct sockaddr *netmask;
    struct sockaddr *broadaddr;
    struct sockaddr *dstaddr;
} pcap_addr_t;
typedef struct pcap_if {
    struct pcap_if *next;
    char *name;
    char *description;
    pcap_addr_t *addresses;
    unsigned flags;
} pcap_if_t;
typedef struct {
    struct timeval ts;
    unsigned caplen;
    unsigned len;
} pcap_pkthdr_t;
typedef struct {
    unsigned bf_len;
    void *bf_insns;
} bpf_program_t;
typedef int(__cdecl *pcap_findalldevs_fn)(pcap_if_t **, char *);
typedef void(__cdecl *pcap_freealldevs_fn)(pcap_if_t *);
typedef pcap_t *(__cdecl *pcap_open_live_fn)(const char *, int, int, int, char *);
typedef int(__cdecl *pcap_compile_fn)(pcap_t *, bpf_program_t *, const char *, int, unsigned);
typedef int(__cdecl *pcap_setfilter_fn)(pcap_t *, bpf_program_t *);
typedef void(__cdecl *pcap_freecode_fn)(bpf_program_t *);
typedef int(__cdecl *pcap_next_ex_fn)(pcap_t *, pcap_pkthdr_t **, const unsigned char **);
typedef int(__cdecl *pcap_datalink_fn)(pcap_t *);
typedef char *(__cdecl *pcap_geterr_fn)(pcap_t *);
typedef void(__cdecl *pcap_close_fn)(pcap_t *);

/* Npcap entry points resolved at runtime so the base build never links against it. */
typedef struct {
    HMODULE module;
    pcap_findalldevs_fn find_all_devs;
    pcap_freealldevs_fn free_all_devs;
    pcap_open_live_fn open_live;
    pcap_compile_fn compile_filter;
    pcap_setfilter_fn set_filter;
    pcap_freecode_fn free_code;
    pcap_next_ex_fn next_packet;
    pcap_datalink_fn datalink;
    pcap_geterr_fn get_error;
    pcap_close_fn close_handle;
} lp_pcap_api_t;

typedef void (*lp_proc_fn)(void);

/* Goes through void (*)(void) so GCC's -Wcast-function-type accepts the per-symbol casts. */
static lp_proc_fn resolve(HMODULE module, const char *name)
{
    return (lp_proc_fn)GetProcAddress(module, name);
}

/* Loads wpcap.dll only from system directories so a DLL planted next to the executable or on
   PATH is never picked up. Npcap installs to System32\Npcap, or to System32 itself in
   WinPcap-compatible mode; Packet.dll is resolved from the same directory. */
static HMODULE load_wpcap(void)
{
    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        static const wchar_t suffix[] = L"\\Npcap\\wpcap.dll";
        if ((size_t)length + sizeof(suffix) / sizeof(suffix[0]) <= MAX_PATH) {
            memcpy(path + length, suffix, sizeof(suffix));
            HMODULE module = LoadLibraryExW(
                path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (module != NULL) {
                return module;
            }
        }
    }
    return LoadLibraryExW(L"wpcap.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

static bool load_pcap_api(lp_pcap_api_t *api)
{
    memset(api, 0, sizeof(*api));
    api->module = load_wpcap();
    if (api->module == NULL) {
        return false;
    }
    api->find_all_devs = (pcap_findalldevs_fn)resolve(api->module, "pcap_findalldevs");
    api->free_all_devs = (pcap_freealldevs_fn)resolve(api->module, "pcap_freealldevs");
    api->open_live = (pcap_open_live_fn)resolve(api->module, "pcap_open_live");
    api->compile_filter = (pcap_compile_fn)resolve(api->module, "pcap_compile");
    api->set_filter = (pcap_setfilter_fn)resolve(api->module, "pcap_setfilter");
    api->free_code = (pcap_freecode_fn)resolve(api->module, "pcap_freecode");
    api->next_packet = (pcap_next_ex_fn)resolve(api->module, "pcap_next_ex");
    api->datalink = (pcap_datalink_fn)resolve(api->module, "pcap_datalink");
    api->get_error = (pcap_geterr_fn)resolve(api->module, "pcap_geterr");
    api->close_handle = (pcap_close_fn)resolve(api->module, "pcap_close");
    return api->find_all_devs != NULL && api->free_all_devs != NULL && api->open_live != NULL &&
           api->compile_filter != NULL && api->set_filter != NULL && api->free_code != NULL &&
           api->next_packet != NULL && api->datalink != NULL && api->get_error != NULL &&
           api->close_handle != NULL;
}

/* Builds the Npcap device name (\Device\NPF_{GUID}) of the adapter carrying the default route. */
static bool default_route_capture_name(char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0) {
        return false;
    }
    out[0] = '\0';
    SOCKADDR_INET destination = {0};
    destination.si_family = AF_INET;
    (void)InetPtonA(AF_INET, "8.8.8.8", &destination.Ipv4.sin_addr);
    MIB_IPFORWARD_ROW2 route = {0};
    if (GetBestRoute2(NULL, 0, NULL, &destination, 0, &route, NULL) != NO_ERROR) {
        return false;
    }
    /* Documented pattern: start at 15 KB and retry if the list grows between calls. */
    ULONG size = 15u * 1024u;
    IP_ADAPTER_ADDRESSES *adapters = NULL;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        free(adapters);
        adapters = malloc(size);
        if (adapters == NULL) {
            return false;
        }
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, adapters, &size);
    }
    if (result != NO_ERROR) {
        free(adapters);
        return false;
    }
    bool found = false;
    for (IP_ADAPTER_ADDRESSES *adapter = adapters; adapter != NULL && !found;
         adapter = adapter->Next) {
        if (adapter->IfIndex != route.InterfaceIndex || adapter->AdapterName == NULL ||
            adapter->AdapterName[0] == '\0') {
            continue;
        }
        /* AdapterName is already the braced GUID ("{...}"). */
        if (strncmp(adapter->AdapterName, "\\Device\\NPF_", 12) == 0) {
            snprintf(out, out_cap, "%s", adapter->AdapterName);
        } else if (adapter->AdapterName[0] == '{') {
            snprintf(out, out_cap, "\\Device\\NPF_%s", adapter->AdapterName);
        } else {
            snprintf(out, out_cap, "\\Device\\NPF_{%s}", adapter->AdapterName);
        }
        found = true;
    }
    free(adapters);
    return found;
}

#define LP_CAPTURE_MAX_LOCAL_ADDRESSES 32

/* Collects this PC's IPv4/IPv6 unicast addresses so packets exchanged with it can be told apart
   from the device's traffic with other hosts. Returns the number stored (0 on failure). */
static size_t local_unicast_addresses(lp_capture_address_t *out, size_t out_cap)
{
    ULONG size = 15u * 1024u;
    IP_ADAPTER_ADDRESSES *adapters = NULL;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        free(adapters);
        adapters = malloc(size);
        if (adapters == NULL) {
            return 0;
        }
        result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, adapters, &size);
    }
    if (result != NO_ERROR) {
        free(adapters);
        return 0;
    }
    size_t count = 0;
    for (IP_ADAPTER_ADDRESSES *adapter = adapters; adapter != NULL && count < out_cap;
         adapter = adapter->Next) {
        for (IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress;
             unicast != NULL && count < out_cap; unicast = unicast->Next) {
            const struct sockaddr *address = unicast->Address.lpSockaddr;
            if (address == NULL) {
                continue;
            }
            memset(&out[count], 0, sizeof(out[count]));
            if (address->sa_family == AF_INET) {
                memcpy(out[count].bytes, &((const struct sockaddr_in *)address)->sin_addr, 4);
                ++count;
            } else if (address->sa_family == AF_INET6) {
                out[count].is_ipv6 = true;
                memcpy(out[count].bytes, &((const struct sockaddr_in6 *)address)->sin6_addr, 16);
                ++count;
            }
        }
    }
    free(adapters);
    return count;
}

/* Parses the device's IP (dropping any IPv6 zone suffix) into `address` and `filter_ip`. */
static bool parse_device_address(const char *device_ip, lp_capture_address_t *address,
                                 char *filter_ip, size_t filter_ip_cap)
{
    snprintf(filter_ip, filter_ip_cap, "%s", device_ip);
    char *zone = strchr(filter_ip, '%');
    if (zone != NULL) {
        *zone = '\0';
    }
    memset(address, 0, sizeof(*address));
    if (InetPtonA(AF_INET, filter_ip, address->bytes) == 1) {
        return true;
    }
    if (InetPtonA(AF_INET6, filter_ip, address->bytes) == 1) {
        address->is_ipv6 = true;
        return true;
    }
    return false;
}

/* True when Npcap was installed with "Restrict Npcap driver's access to Administrators only". */
static bool npcap_is_admin_only(void)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_LOCAL_MACHINE,
                        L"SYSTEM\\CurrentControlSet\\Services\\npcap\\Parameters", L"AdminOnly",
                        RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS &&
           value != 0;
}

static bool process_is_elevated(void)
{
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation = {0};
    DWORD size = 0;
    const bool elevated =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

/* In admin-only mode a non-elevated process either sees no adapters or is denied when opening
   one, so any open failure is reported as the restriction rather than a generic error. */
static bool report_admin_only(lp_capture_activity_t *out, const char *error)
{
    const bool access_denied = error != NULL && (strstr(error, "Access is denied") != NULL ||
                                                 strstr(error, "(5)") != NULL);
    if (!access_denied && !(npcap_is_admin_only() && !process_is_elevated())) {
        return false;
    }
    out->restricted_to_admins = true;
    snprintf(out->detail, sizeof(out->detail), "Npcap is restricted to administrators");
    return true;
}

static pcap_t *open_default_route_interface(const lp_pcap_api_t *api, lp_capture_activity_t *out,
                                            lp_status_t *status)
{
    char *detail = out->detail;
    const size_t detail_cap = sizeof(out->detail);
    char error[LP_PCAP_ERRBUF_SIZE] = "";
    pcap_if_t *devices = NULL;
    if (api->find_all_devs(&devices, error) != 0) {
        if (report_admin_only(out, error)) {
            *status = LP_ERR_UNSUPPORTED;
            return NULL;
        }
        snprintf(detail, detail_cap, "Npcap: %.110s", error);
        *status = LP_ERR_IO;
        return NULL;
    }
    char capture_name[128] = "";
    pcap_t *handle = NULL;
    if (default_route_capture_name(capture_name, sizeof(capture_name))) {
        for (pcap_if_t *item = devices; item != NULL && handle == NULL; item = item->next) {
            if (item->name != NULL && strcmp(item->name, capture_name) == 0) {
                handle = api->open_live(item->name, LP_CAPTURE_SNAPLEN, 0, 250, error);
            }
        }
    }
    api->free_all_devs(devices);
    if (handle == NULL) {
        if (report_admin_only(out, error)) {
            *status = LP_ERR_UNSUPPORTED;
            return NULL;
        }
        if (capture_name[0] == '\0') {
            snprintf(detail, detail_cap, "No default-route adapter found");
        } else {
            snprintf(detail, detail_cap, "No capture interface matches %.80s", capture_name);
        }
        *status = LP_ERR_NOT_FOUND;
    }
    return handle;
}

static bool is_cancelled(volatile long *cancel)
{
    return cancel != NULL && InterlockedCompareExchange(cancel, 0, 0) != 0;
}

lp_status_t lp_win32_capture_device_activity(const char *device_ip, unsigned duration_ms,
                                             volatile long *cancel, lp_capture_activity_t *out)
{
    if (device_ip == NULL || device_ip[0] == '\0' || out == NULL || duration_ms == 0) {
        return LP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    lp_capture_address_t device;
    char filter_ip[LP_IP_STR_MAX];
    if (!parse_device_address(device_ip, &device, filter_ip, sizeof(filter_ip))) {
        snprintf(out->detail, sizeof(out->detail), "Unrecognized device address");
        return LP_ERR_INVALID_ARG;
    }

    lp_pcap_api_t api;
    if (!load_pcap_api(&api)) {
        if (api.module != NULL) {
            FreeLibrary(api.module);
        }
        snprintf(out->detail, sizeof(out->detail), "Npcap is not installed");
        return LP_ERR_UNSUPPORTED;
    }
    out->available = true;

    lp_status_t status = LP_OK;
    pcap_t *handle = open_default_route_interface(&api, out, &status);
    if (handle == NULL) {
        FreeLibrary(api.module);
        return status;
    }

    const int link_type = api.datalink(handle);
    if (link_type != LP_CAPTURE_LINKTYPE_ETHERNET) {
        snprintf(out->detail, sizeof(out->detail), "Unsupported capture link type %d", link_type);
        api.close_handle(handle);
        FreeLibrary(api.module);
        return LP_ERR_UNSUPPORTED;
    }

    char filter[96];
    snprintf(filter, sizeof(filter), "host %s and (tcp or udp)", filter_ip);
    bpf_program_t program = {0};
    if (api.compile_filter(handle, &program, filter, 1, 0) != 0) {
        snprintf(out->detail, sizeof(out->detail), "Could not filter the selected device");
        api.close_handle(handle);
        FreeLibrary(api.module);
        return LP_ERR_IO;
    }
    if (api.set_filter(handle, &program) != 0) {
        snprintf(out->detail, sizeof(out->detail), "Could not filter the selected device");
        api.free_code(&program);
        api.close_handle(handle);
        FreeLibrary(api.module);
        return LP_ERR_IO;
    }

    lp_capture_address_t local_addresses[LP_CAPTURE_MAX_LOCAL_ADDRESSES];
    const size_t local_count =
        local_unicast_addresses(local_addresses, LP_CAPTURE_MAX_LOCAL_ADDRESSES);

    const DWORD until = GetTickCount() + duration_ms;
    while ((LONG)(GetTickCount() - until) < 0) {
        if (is_cancelled(cancel)) {
            snprintf(out->detail, sizeof(out->detail), "Capture cancelled");
            status = LP_ERR_IO;
            break;
        }
        pcap_pkthdr_t *header = NULL;
        const unsigned char *packet = NULL;
        const int result = api.next_packet(handle, &header, &packet);
        if (result < 0) {
            /* -1 is a read error (e.g. adapter removed), -2 end of capture: stop either way. */
            snprintf(out->detail, sizeof(out->detail), "Capture stopped: %.100s",
                     result == -1 ? api.get_error(handle) : "end of capture");
            status = LP_ERR_IO;
            break;
        }
        if (result != 1 || header == NULL || packet == NULL) {
            continue;
        }
        lp_capture_address_t peer;
        const lp_capture_direction_t direction =
            lp_capture_classify_frame_peer(link_type, packet, header->caplen, &device, &peer);
        if (direction == LP_CAPTURE_FRAME_FROM_DEVICE) {
            out->tx_bytes += header->len;
        } else if (direction == LP_CAPTURE_FRAME_TO_DEVICE) {
            out->rx_bytes += header->len;
        } else {
            continue;
        }
        ++out->packet_count;
        if (lp_capture_address_in(&peer, local_addresses, local_count)) {
            ++out->local_packets;
        }
    }

    api.free_code(&program);
    api.close_handle(handle);
    FreeLibrary(api.module);
    out->device_visible = out->packet_count != 0;
    out->sees_only_local_host = out->device_visible && out->local_packets == out->packet_count;
    if (status == LP_OK) {
        const char *summary = "No selected-device traffic visible";
        if (out->sees_only_local_host) {
            summary = "Capture sees only traffic with this PC";
        } else if (out->device_visible) {
            summary = "Visible traffic captured";
        }
        snprintf(out->detail, sizeof(out->detail), "%s", summary);
    }
    return status;
}
