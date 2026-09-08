#include "linkpulse/discovery.h"

#include <winsock2.h>
#include <ws2tcpip.h>
/* windows.h must follow winsock2.h, and iphlpapi.h/netioapi.h must follow both. */
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <stdio.h>

/* States that mean "the OS currently believes this neighbour is present" --
   excludes NlnsIncomplete (resolution in progress, not yet confirmed) and
   NlnsUnreachable (resolution failed / timed out). */
static bool is_live_state(NL_NEIGHBOR_STATE state)
{
    switch (state) {
    case NlnsReachable:
    case NlnsStale:
    case NlnsDelay:
    case NlnsProbe:
    case NlnsPermanent:
        return true;
    default:
        return false;
    }
}

static void format_mac(const UCHAR *address, ULONG length, char *out, size_t out_cap)
{
    out[0] = '\0';
    if (length != 6) { /* only handle Ethernet-style MACs; anything else is left blank */
        return;
    }
    snprintf(out, out_cap, "%02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1], address[2],
             address[3], address[4], address[5]);
}

lp_status_t lp_net_neighbor_snapshot(lp_neighbor_list_t *out)
{
    if (out == NULL) {
        return LP_ERR_INVALID_ARG;
    }
    out->count = 0;

    MIB_IPNET_TABLE2 *table = NULL;
    if (GetIpNetTable2(AF_UNSPEC, &table) != NO_ERROR || table == NULL) {
        return LP_ERR_IO;
    }

    for (ULONG i = 0; i < table->NumEntries && out->count < LP_DISCOVERY_MAX_NEIGHBORS; ++i) {
        const MIB_IPNET_ROW2 *row = &table->Table[i];
        if (!is_live_state(row->State)) {
            continue;
        }

        char ip[LP_IP_STR_MAX] = {0};
        if (row->Address.si_family == AF_INET) {
            InetNtopA(AF_INET, (PVOID)&row->Address.Ipv4.sin_addr, ip, sizeof(ip));
        } else if (row->Address.si_family == AF_INET6) {
            InetNtopA(AF_INET6, (PVOID)&row->Address.Ipv6.sin6_addr, ip, sizeof(ip));
        } else {
            continue;
        }
        if (ip[0] == '\0') {
            continue;
        }

        lp_neighbor_t *neighbor = &out->items[out->count];
        snprintf(neighbor->ip, sizeof(neighbor->ip), "%s", ip);
        format_mac(row->PhysicalAddress, row->PhysicalAddressLength, neighbor->mac,
                   sizeof(neighbor->mac));
        ++out->count;
    }

    FreeMibTable(table);
    return LP_OK;
}

static bool format_sockaddr(const SOCKADDR *address, char *out, size_t out_cap)
{
    if (address == NULL) {
        return false;
    }
    if (address->sa_family == AF_INET) {
        return InetNtopA(AF_INET, &((const SOCKADDR_IN *)address)->sin_addr, out,
                         (DWORD)out_cap) != NULL;
    }
    if (address->sa_family == AF_INET6) {
        return InetNtopA(AF_INET6, &((const SOCKADDR_IN6 *)address)->sin6_addr, out,
                         (DWORD)out_cap) != NULL;
    }
    return false;
}

lp_status_t lp_net_local_networks(lp_local_network_list_t *out)
{
    if (out == NULL) {
        return LP_ERR_INVALID_ARG;
    }
    out->count = 0;

    ULONG buffer_size = 0;
    DWORD result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, NULL,
                                        &buffer_size);
    if (result != ERROR_BUFFER_OVERFLOW || buffer_size == 0) {
        return LP_ERR_IO;
    }

    IP_ADAPTER_ADDRESSES *adapters =
        (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(), 0, buffer_size);
    if (adapters == NULL) {
        return LP_ERR_NO_MEMORY;
    }
    result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, NULL, adapters,
                                  &buffer_size);
    if (result != NO_ERROR) {
        HeapFree(GetProcessHeap(), 0, adapters);
        return LP_ERR_IO;
    }

    for (IP_ADAPTER_ADDRESSES *adapter = adapters; adapter != NULL;
         adapter = adapter->Next) {
        char gateway[LP_IP_STR_MAX] = {0};
        if (adapter->FirstGatewayAddress != NULL) {
            (void)format_sockaddr(adapter->FirstGatewayAddress->Address.lpSockaddr, gateway,
                                  sizeof(gateway));
        }

        for (IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress;
             unicast != NULL && out->count < LP_DISCOVERY_MAX_NETWORKS;
             unicast = unicast->Next) {
            char address[LP_IP_STR_MAX] = {0};
            if (!format_sockaddr(unicast->Address.lpSockaddr, address, sizeof(address))) {
                continue;
            }
            lp_local_network_t *network = &out->items[out->count++];
            snprintf(network->address, sizeof(network->address), "%s", address);
            network->prefix_length = unicast->OnLinkPrefixLength;
            snprintf(network->gateway, sizeof(network->gateway), "%s", gateway);
        }
    }

    HeapFree(GetProcessHeap(), 0, adapters);
    return LP_OK;
}
