#include "linkpulse/capture.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pcap pcap_t;
typedef struct pcap_addr { struct pcap_addr *next; struct sockaddr *addr; struct sockaddr *netmask; struct sockaddr *broadaddr; struct sockaddr *dstaddr; } pcap_addr_t;
typedef struct pcap_if { struct pcap_if *next; char *name; char *description; pcap_addr_t *addresses; unsigned flags; } pcap_if_t;
typedef struct { struct timeval ts; unsigned caplen; unsigned len; } pcap_pkthdr_t;
typedef struct { unsigned bf_len; void *bf_insns; } bpf_program_t;
typedef int (__cdecl *pcap_findalldevs_fn)(pcap_if_t **, char *);
typedef void (__cdecl *pcap_freealldevs_fn)(pcap_if_t *);
typedef pcap_t *(__cdecl *pcap_open_live_fn)(const char *, int, int, int, char *);
typedef int (__cdecl *pcap_compile_fn)(pcap_t *, bpf_program_t *, const char *, int, unsigned);
typedef int (__cdecl *pcap_setfilter_fn)(pcap_t *, bpf_program_t *);
typedef void (__cdecl *pcap_freecode_fn)(bpf_program_t *);
typedef int (__cdecl *pcap_next_ex_fn)(pcap_t *, pcap_pkthdr_t **, const unsigned char **);
typedef void (__cdecl *pcap_close_fn)(pcap_t *);

static bool default_route_capture_name(char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0) return false;
    out[0] = '\0';
    SOCKADDR_INET destination = {0};
    destination.si_family = AF_INET;
    (void)InetPtonA(AF_INET, "8.8.8.8", &destination.Ipv4.sin_addr);
    MIB_IPFORWARD_ROW2 route = {0};
    if (GetBestRoute2(NULL, 0, NULL, &destination, 0, &route, NULL) != NO_ERROR) return false;
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, NULL, &size) !=
        ERROR_BUFFER_OVERFLOW) return false;
    IP_ADAPTER_ADDRESSES *adapters = malloc(size);
    if (adapters == NULL || GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, adapters,
                                                 &size) != NO_ERROR) {
        free(adapters);
        return false;
    }
    bool found = false;
    for (IP_ADAPTER_ADDRESSES *adapter = adapters; adapter != NULL && !found; adapter = adapter->Next) {
        if (adapter->IfIndex != route.InterfaceIndex) continue;
        if (adapter->AdapterName != NULL && adapter->AdapterName[0] != '\0') {
            if (strncmp(adapter->AdapterName, "\\Device\\NPF_", 12) == 0) {
                snprintf(out, out_cap, "%s", adapter->AdapterName);
            } else {
                snprintf(out, out_cap, "\\Device\\NPF_{%s}", adapter->AdapterName);
            }
            found = true;
        }
    }
    free(adapters);
    return found;
}

lp_status_t lp_win32_capture_device_activity(const char *device_ip, unsigned duration_ms,
                                             lp_capture_activity_t *out)
{
    if (device_ip == NULL || device_ip[0] == '\0' || out == NULL || duration_ms == 0) return LP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    HMODULE module = LoadLibraryA("wpcap.dll");
    if (module == NULL) { snprintf(out->detail, sizeof(out->detail), "Npcap is not installed"); return LP_ERR_UNSUPPORTED; }
    pcap_findalldevs_fn find_all = (pcap_findalldevs_fn)GetProcAddress(module, "pcap_findalldevs");
    pcap_freealldevs_fn free_all = (pcap_freealldevs_fn)GetProcAddress(module, "pcap_freealldevs");
    pcap_open_live_fn open_live = (pcap_open_live_fn)GetProcAddress(module, "pcap_open_live");
    pcap_compile_fn compile = (pcap_compile_fn)GetProcAddress(module, "pcap_compile");
    pcap_setfilter_fn set_filter = (pcap_setfilter_fn)GetProcAddress(module, "pcap_setfilter");
    pcap_freecode_fn free_code = (pcap_freecode_fn)GetProcAddress(module, "pcap_freecode");
    pcap_next_ex_fn next = (pcap_next_ex_fn)GetProcAddress(module, "pcap_next_ex");
    pcap_close_fn close = (pcap_close_fn)GetProcAddress(module, "pcap_close");
    if (!find_all || !free_all || !open_live || !compile || !set_filter || !free_code || !next || !close) { FreeLibrary(module); return LP_ERR_UNSUPPORTED; }
    out->available = true;
    char error[256] = ""; pcap_if_t *devices = NULL;
    if (find_all(&devices, error) != 0) { snprintf(out->detail, sizeof(out->detail), "Npcap: %.110s", error); FreeLibrary(module); return LP_ERR_IO; }
    char default_capture_name[128];
    pcap_t *handle = NULL;
    if (default_route_capture_name(default_capture_name, sizeof(default_capture_name))) {
        for (pcap_if_t *item = devices; item != NULL && handle == NULL; item = item->next) {
            if (strcmp(item->name, default_capture_name) == 0) {
            handle = open_live(item->name, 65535, 1, 250, error);
            }
        }
    }
    free_all(devices);
    if (handle == NULL) {
        snprintf(out->detail, sizeof(out->detail), "No capture interface matches %.80s",
                 default_capture_name);
        FreeLibrary(module);
        return LP_ERR_NOT_FOUND;
    }
    char filter[96]; snprintf(filter, sizeof(filter), "host %s and (tcp or udp)", device_ip);
    bpf_program_t program = {0};
    if (compile(handle, &program, filter, 1, 0) != 0 || set_filter(handle, &program) != 0) { free_code(&program); close(handle); FreeLibrary(module); snprintf(out->detail, sizeof(out->detail), "Could not filter the selected device"); return LP_ERR_IO; }
    const DWORD until = GetTickCount() + duration_ms;
    while ((LONG)(GetTickCount() - until) < 0) { pcap_pkthdr_t *header = NULL; const unsigned char *packet = NULL; if (next(handle, &header, &packet) == 1 && header != NULL && header->caplen >= 34) { const bool outbound = packet[26] == (unsigned char)inet_addr(device_ip); if (outbound) out->tx_bytes += header->len; else out->rx_bytes += header->len; ++out->flow_count; } }
    free_code(&program); close(handle); FreeLibrary(module);
    out->device_visible = out->flow_count != 0;
    snprintf(out->detail, sizeof(out->detail), out->device_visible ? "Visible traffic captured" : "No selected-device traffic visible");
    return LP_OK;
}