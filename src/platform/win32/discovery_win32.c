#include "linkpulse/discovery.h"

#include <winsock2.h>
#include <ws2tcpip.h>
/* windows.h must follow winsock2.h, and iphlpapi.h/netioapi.h must follow both. */
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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

static bool contains_token(const char *value, const char *token)
{
    if (value == NULL || token == NULL || token[0] == '\0') {
        return false;
    }
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        const char *left = cursor;
        const char *right = token;
        while (*left != '\0' && *right != '\0' &&
               tolower((unsigned char)*left) == tolower((unsigned char)*right)) {
            ++left;
            ++right;
        }
        if (*right == '\0') {
            return true;
        }
    }
    return false;
}

static void classify_identity(const char *hostname, char *vendor, size_t vendor_cap,
                              lp_device_type_t *device_type, uint8_t *confidence)
{
    vendor[0] = '\0';
    *device_type = LP_DEVICE_UNKNOWN;
    *confidence = 0;
    if (contains_token(hostname, "epson")) {
        snprintf(vendor, vendor_cap, "Epson");
        *device_type = LP_DEVICE_PRINTER;
        *confidence = 95;
    } else if (contains_token(hostname, "printer") || contains_token(hostname, "print")) {
        *device_type = LP_DEVICE_PRINTER;
        *confidence = 90;
    } else if (contains_token(hostname, "iphone") || contains_token(hostname, "android") ||
               contains_token(hostname, "pixel") || contains_token(hostname, "galaxy")) {
        *device_type = LP_DEVICE_MOBILE;
        *confidence = 85;
    } else if (contains_token(hostname, "watch") || contains_token(hostname, "fitbit")) {
        *device_type = LP_DEVICE_SMARTWATCH;
        *confidence = 85;
    } else if (contains_token(hostname, "laptop") || contains_token(hostname, "macbook")) {
        *device_type = LP_DEVICE_LAPTOP;
        *confidence = 80;
    } else if (contains_token(hostname, "desktop") || contains_token(hostname, "computer")) {
        *device_type = LP_DEVICE_DESKTOP;
        *confidence = 80;
    } else if (contains_token(hostname, "tv") || contains_token(hostname, "roku") ||
               contains_token(hostname, "chromecast")) {
        *device_type = LP_DEVICE_TELEVISION;
        *confidence = 80;
    } else if (contains_token(hostname, "router") || contains_token(hostname, "gateway") ||
               contains_token(hostname, "fritz")) {
        snprintf(vendor, vendor_cap, "FRITZ!Box");
        *device_type = LP_DEVICE_ROUTER;
        *confidence = 90;
    }
}

static void classify_neighbor(lp_neighbor_t *neighbor)
{
    classify_identity(neighbor->hostname, neighbor->vendor, sizeof(neighbor->vendor),
                      &neighbor->device_type, &neighbor->device_confidence);
}

static lp_connection_type_t connection_type_for_interface(const NET_LUID *interface_luid)
{
    MIB_IF_ROW2 interface_row;
    memset(&interface_row, 0, sizeof(interface_row));
    interface_row.InterfaceLuid = *interface_luid;
    if (GetIfEntry2(&interface_row) != NO_ERROR) {
        return LP_CONNECTION_UNKNOWN;
    }
    if (interface_row.Type == IF_TYPE_IEEE80211) {
        return LP_CONNECTION_WIFI;
    }
    if (interface_row.Type == IF_TYPE_ETHERNET_CSMACD) {
        return LP_CONNECTION_ETHERNET;
    }
    return LP_CONNECTION_UNKNOWN;
}

static long find_neighbor_mac(const lp_neighbor_list_t *list, const char *mac)
{
    if (mac[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i].mac, mac) == 0) {
            return (long)i;
        }
    }
    return -1;
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
        if (find_neighbor_mac(out, neighbor->mac) >= 0) {
            continue;
        }
        neighbor->connection_type = connection_type_for_interface(&row->InterfaceLuid);
        if (winsock_ready) {
            resolve_hostname(&row->Address, neighbor->hostname, sizeof(neighbor->hostname));
        }
        classify_neighbor(neighbor);
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

static bool get_default_gateway(NET_LUID *interface_luid, char *gateway, size_t gateway_cap)
{
    SOCKADDR_INET destination;
    memset(&destination, 0, sizeof(destination));
    destination.Ipv4.sin_family = AF_INET;
    destination.Ipv4.sin_addr.S_un.S_addr = htonl(0x08080808); /* 8.8.8.8 */

    MIB_IPFORWARD_ROW2 route;
    SOCKADDR_INET best_source;
    if (GetBestRoute2(NULL, 0, NULL, &destination, 0, &route, &best_source) != NO_ERROR ||
        !format_sockaddr((const SOCKADDR *)&route.NextHop, gateway, gateway_cap) ||
        strcmp(gateway, "0.0.0.0") == 0) {
        gateway[0] = '\0';
        return false;
    }
    *interface_luid = route.InterfaceLuid;
    return true;
}

static void get_default_gateway_mac(const char *gateway, char *mac, size_t mac_cap)
{
    mac[0] = '\0';
    MIB_IPNET_TABLE2 *table = NULL;
    if (GetIpNetTable2(AF_INET, &table) != NO_ERROR || table == NULL) {
        return;
    }
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IPNET_ROW2 *row = &table->Table[i];
        if (row->Address.si_family != AF_INET || row->PhysicalAddressLength != 6 ||
            !is_live_state(row->State)) {
            continue;
        }
        char address[LP_IP_STR_MAX] = {0};
        if (InetNtopA(AF_INET, (PVOID)&row->Address.Ipv4.sin_addr, address, sizeof(address)) != NULL &&
            strcmp(address, gateway) == 0) {
            format_mac(row->PhysicalAddress, row->PhysicalAddressLength, mac, mac_cap);
            break;
        }
    }
    FreeMibTable(table);
}

lp_status_t lp_net_local_networks(lp_local_network_list_t *out)
{
    if (out == NULL) {
        return LP_ERR_INVALID_ARG;
    }
    out->count = 0;

    NET_LUID default_interface_luid;
    memset(&default_interface_luid, 0, sizeof(default_interface_luid));
    char default_gateway[LP_IP_STR_MAX] = {0};
    const bool has_default_gateway = get_default_gateway(
        &default_interface_luid, default_gateway, sizeof(default_gateway));
    char default_gateway_mac[LP_MAC_STR_MAX] = {0};
    if (has_default_gateway) {
        get_default_gateway_mac(default_gateway, default_gateway_mac,
                                sizeof(default_gateway_mac));
    }
    char default_gateway_hostname[LP_HOSTNAME_MAX] = {0};
    lp_device_type_t default_gateway_type = LP_DEVICE_UNKNOWN;
    uint8_t default_gateway_confidence = 0;
    if (has_default_gateway) {
        SOCKADDR_INET gateway_address;
        memset(&gateway_address, 0, sizeof(gateway_address));
        gateway_address.Ipv4.sin_family = AF_INET;
        if (InetPtonA(AF_INET, default_gateway, &gateway_address.Ipv4.sin_addr) == 1) {
            WSADATA winsock_data;
            if (WSAStartup(MAKEWORD(2, 2), &winsock_data) == 0) {
                resolve_hostname(&gateway_address, default_gateway_hostname,
                                 sizeof(default_gateway_hostname));
                WSACleanup();
            }
        }
    }
    char default_gateway_vendor[LP_VENDOR_MAX];
    classify_identity(default_gateway_hostname, default_gateway_vendor,
                      sizeof(default_gateway_vendor), &default_gateway_type,
                      &default_gateway_confidence);

    ULONG buffer_size = 0;
    const ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
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
        if (has_default_gateway && adapter->Luid.Value == default_interface_luid.Value) {
            snprintf(gateway, sizeof(gateway), "%s", default_gateway);
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
            if (gateway[0] != '\0') {
                snprintf(network->gateway_mac, sizeof(network->gateway_mac), "%s",
                         default_gateway_mac);
                snprintf(network->gateway_hostname, sizeof(network->gateway_hostname), "%s",
                         default_gateway_hostname);
                snprintf(network->gateway_vendor, sizeof(network->gateway_vendor), "%s",
                         default_gateway_vendor);
                network->gateway_device_type = default_gateway_type;
                network->gateway_confidence = default_gateway_confidence;
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, adapters);
    return LP_OK;
}
