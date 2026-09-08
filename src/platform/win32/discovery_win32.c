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

static bool is_unicast_address(const SOCKADDR_INET *address)
{
    if (address->si_family == AF_INET) {
        const ULONG host_address = ntohl(address->Ipv4.sin_addr.S_un.S_addr);
        const unsigned first_octet = (unsigned)(host_address >> 24);
        return host_address != 0xFFFFFFFFUL && first_octet < 224;
    }
    if (address->si_family == AF_INET6) {
        return !IN6_IS_ADDR_MULTICAST(&address->Ipv6.sin6_addr);
    }
    return false;
}

static void resolve_hostname(const SOCKADDR_INET *address, char *out, size_t out_cap)
{
    out[0] = '\0';
    const int address_length = address->si_family == AF_INET ? sizeof(SOCKADDR_IN)
                                                             : sizeof(SOCKADDR_IN6);
    if (GetNameInfoA((const SOCKADDR *)address, address_length, out, (DWORD)out_cap, NULL, 0,
                     NI_NAMEREQD) != 0) {
        out[0] = '\0';
    }
}

lp_status_t lp_net_neighbor_snapshot(lp_neighbor_list_t *out)
{
    if (out == NULL) {
        return LP_ERR_INVALID_ARG;
    }
    out->count = 0;

    WSADATA winsock_data;
    const bool winsock_ready = WSAStartup(MAKEWORD(2, 2), &winsock_data) == 0;

    MIB_IPNET_TABLE2 *table = NULL;
    if (GetIpNetTable2(AF_UNSPEC, &table) != NO_ERROR || table == NULL) {
        if (winsock_ready) {
            WSACleanup();
        }
        return LP_ERR_IO;
    }

    for (ULONG i = 0; i < table->NumEntries && out->count < LP_DISCOVERY_MAX_NEIGHBORS; ++i) {
        const MIB_IPNET_ROW2 *row = &table->Table[i];
        if (!is_live_state(row->State) || !is_unicast_address(&row->Address)) {
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
        if (winsock_ready) {
            resolve_hostname(&row->Address, neighbor->hostname, sizeof(neighbor->hostname));
        }
        ++out->count;
    }

    FreeMibTable(table);
    if (winsock_ready) {
        WSACleanup();
    }
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
    const ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS;
    DWORD result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, NULL, &buffer_size);
    if (result != ERROR_BUFFER_OVERFLOW || buffer_size == 0) {
        return LP_ERR_IO;
    }

    IP_ADAPTER_ADDRESSES *adapters =
        (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(), 0, buffer_size);
    if (adapters == NULL) {
        return LP_ERR_NO_MEMORY;
    }
    result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, adapters, &buffer_size);
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
