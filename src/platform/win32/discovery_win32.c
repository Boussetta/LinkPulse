#include "linkpulse/discovery.h"
#include "linkpulse/log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
/* windows.h must follow winsock2.h, and iphlpapi.h/netioapi.h must follow both. */
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Active probing is bounded to avoid turning discovery into a large LAN scan. */
#define LP_ACTIVE_SCAN_MAX_PREFIX 24
#define LP_ACTIVE_SCAN_MAX_SUBNETS 8
#define LP_ACTIVE_SCAN_MAX_PROBES 32
#define LP_MDNS_PORT 5353
#define LP_MDNS_TIMEOUT_MS 250
#define LP_DNS_PACKET_MAX 1500

static uint32_t g_active_probe_offset;

/* Keeps neighbors with recent or active reachability evidence. Stale and
    permanent cache entries are excluded because they can outlive a device. */
static bool is_live_state(NL_NEIGHBOR_STATE state)
{
    switch (state) {
    case NlnsReachable:
    case NlnsDelay:
    case NlnsProbe:
        return true;
    default:
        return false;
    }
}

/* Formats only six-byte Ethernet addresses; unsupported link types stay blank. */
static void format_mac(const UCHAR *address, ULONG length, char *out, size_t out_cap)
{
    out[0] = '\0';
    if (length != 6) { /* only handle Ethernet-style MACs; anything else is left blank */
        return;
    }
    snprintf(out, out_cap, "%02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1], address[2],
             address[3], address[4], address[5]);
}

static bool is_locally_administered_mac(const char *mac)
{
    unsigned first_octet = 0;
    if (mac == NULL || sscanf(mac, "%2x", &first_octet) != 1) {
        return false;
    }
    return (first_octet & 0x02u) != 0 && (first_octet & 0x01u) == 0;
}

/* Excludes broadcast, multicast, and unspecified addresses from inventory. */
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

static uint16_t dns_read_u16(const unsigned char *packet, size_t offset)
{
    return (uint16_t)((packet[offset] << 8) | packet[offset + 1]);
}

static uint32_t dns_read_u32(const unsigned char *packet, size_t offset)
{
    return ((uint32_t)packet[offset] << 24) | ((uint32_t)packet[offset + 1] << 16) |
           ((uint32_t)packet[offset + 2] << 8) | (uint32_t)packet[offset + 3];
}

static bool dns_skip_name(const unsigned char *packet, size_t packet_len, size_t *offset)
{
    for (int guard = 0; guard < 128 && *offset < packet_len; ++guard) {
        const unsigned char label_len = packet[*offset];
        if (label_len == 0) {
            ++(*offset);
            return true;
        }
        if ((label_len & 0xC0u) == 0xC0u) {
            if (*offset + 1 >= packet_len) return false;
            *offset += 2;
            return true;
        }
        if ((label_len & 0xC0u) != 0 || *offset + 1 + label_len > packet_len) {
            return false;
        }
        *offset += 1 + label_len;
    }
    return false;
}

static bool dns_read_name(const unsigned char *packet, size_t packet_len, size_t *offset,
                          char *out, size_t out_cap)
{
    size_t cursor = *offset;
    size_t out_len = 0;
    bool jumped = false;
    out[0] = '\0';
    for (int guard = 0; guard < 128 && cursor < packet_len; ++guard) {
        const unsigned char label_len = packet[cursor];
        if (label_len == 0) {
            if (!jumped) *offset = cursor + 1;
            return out_len > 0;
        }
        if ((label_len & 0xC0u) == 0xC0u) {
            if (cursor + 1 >= packet_len) return false;
            const size_t target = (size_t)(((label_len & 0x3Fu) << 8) | packet[cursor + 1]);
            if (!jumped) *offset = cursor + 2;
            cursor = target;
            jumped = true;
            continue;
        }
        if ((label_len & 0xC0u) != 0 || cursor + 1 + label_len > packet_len) {
            return false;
        }
        if (out_len > 0 && out_len + 1 < out_cap) {
            out[out_len++] = '.';
        }
        for (size_t i = 0; i < label_len && out_len + 1 < out_cap; ++i) {
            out[out_len++] = (char)packet[cursor + 1 + i];
        }
        out[out_len] = '\0';
        cursor += 1 + label_len;
    }
    return false;
}

static bool dns_write_qname(unsigned char *packet, size_t packet_cap, size_t *offset,
                            const char *name)
{
    const char *label = name;
    while (*label != '\0') {
        const char *dot = strchr(label, '.');
        const size_t label_len = dot != NULL ? (size_t)(dot - label) : strlen(label);
        if (label_len == 0 || label_len > 63 || *offset + 1 + label_len >= packet_cap) {
            return false;
        }
        packet[(*offset)++] = (unsigned char)label_len;
        memcpy(packet + *offset, label, label_len);
        *offset += label_len;
        if (dot == NULL) break;
        label = dot + 1;
    }
    if (*offset >= packet_cap) return false;
    packet[(*offset)++] = 0;
    return true;
}

static bool resolve_mdns_hostname_ipv4(const SOCKADDR_INET *address, char *out, size_t out_cap)
{
    unsigned char query[512] = {0};
    char reverse_name[64];
    const uint32_t host_address = ntohl(address->Ipv4.sin_addr.S_un.S_addr);
    snprintf(reverse_name, sizeof(reverse_name), "%u.%u.%u.%u.in-addr.arpa",
             (unsigned)(host_address & 0xffu), (unsigned)((host_address >> 8) & 0xffu),
             (unsigned)((host_address >> 16) & 0xffu), (unsigned)((host_address >> 24) & 0xffu));

    query[5] = 1; /* QDCOUNT */
    size_t query_len = 12;
    if (!dns_write_qname(query, sizeof(query), &query_len, reverse_name) ||
        query_len + 4 > sizeof(query)) {
        return false;
    }
    query[query_len++] = 0;
    query[query_len++] = 12; /* PTR */
    query[query_len++] = 0;
    query[query_len++] = 1; /* IN */

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return false;
    }
    DWORD timeout = LP_MDNS_TIMEOUT_MS;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    unsigned char ttl = 255;
    (void)setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));

    SOCKADDR_IN destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(LP_MDNS_PORT);
    if (InetPtonA(AF_INET, "224.0.0.251", &destination.sin_addr) != 1) {
        closesocket(sock);
        return false;
    }
    const int sent = sendto(sock, (const char *)query, (int)query_len, 0,
                            (const SOCKADDR *)&destination, sizeof(destination));
    if (sent == SOCKET_ERROR) {
        closesocket(sock);
        return false;
    }

    unsigned char response[LP_DNS_PACKET_MAX];
    const int received = recvfrom(sock, (char *)response, sizeof(response), 0, NULL, NULL);
    closesocket(sock);
    if (received < 12) {
        return false;
    }

    const size_t response_len = (size_t)received;
    const uint16_t qdcount = dns_read_u16(response, 4);
    const uint16_t ancount = dns_read_u16(response, 6);
    const uint16_t nscount = dns_read_u16(response, 8);
    const uint16_t arcount = dns_read_u16(response, 10);
    size_t offset = 12;
    for (uint16_t i = 0; i < qdcount; ++i) {
        if (!dns_skip_name(response, response_len, &offset) || offset + 4 > response_len) {
            return false;
        }
        offset += 4;
    }
    const uint32_t record_count = (uint32_t)ancount + (uint32_t)nscount + (uint32_t)arcount;
    for (uint32_t i = 0; i < record_count; ++i) {
        if (!dns_skip_name(response, response_len, &offset) || offset + 10 > response_len) {
            return false;
        }
        const uint16_t type = dns_read_u16(response, offset);
        (void)dns_read_u16(response, offset + 2);
        (void)dns_read_u32(response, offset + 4);
        const uint16_t rdlength = dns_read_u16(response, offset + 8);
        offset += 10;
        if (offset + rdlength > response_len) {
            return false;
        }
        if (type == 12) {
            size_t rdata_offset = offset;
            if (dns_read_name(response, response_len, &rdata_offset, out, out_cap)) {
                return true;
            }
        }
        offset += rdlength;
    }
    return false;
}

/* Performs best-effort reverse DNS without making discovery fail on lookup errors. */
static void resolve_hostname(const SOCKADDR_INET *address, char *out, size_t out_cap)
{
    out[0] = '\0';
    const int address_length = address->si_family == AF_INET ? sizeof(SOCKADDR_IN)
                                                             : sizeof(SOCKADDR_IN6);
    if (GetNameInfoA((const SOCKADDR *)address, address_length, out, (DWORD)out_cap, NULL, 0,
                     NI_NAMEREQD) != 0) {
        out[0] = '\0';
    }
    if (out[0] == '\0' && address->si_family == AF_INET) {
        (void)resolve_mdns_hostname_ipv4(address, out, out_cap);
    }
}

/* Performs a case-insensitive substring check for hostname classification rules. */
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

/* Applies conservative hostname heuristics and records a confidence score. */
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

/* Fills the user-facing identity fields derived from a neighbor hostname. */
static void classify_neighbor(lp_neighbor_t *neighbor)
{
    classify_identity(neighbor->hostname, neighbor->vendor, sizeof(neighbor->vendor),
                      &neighbor->device_type, &neighbor->device_confidence);
    if (neighbor->device_type == LP_DEVICE_UNKNOWN &&
        is_locally_administered_mac(neighbor->mac)) {
        snprintf(neighbor->vendor, sizeof(neighbor->vendor), "Private Wi-Fi MAC");
        neighbor->device_type = LP_DEVICE_MOBILE;
        neighbor->device_confidence = 35;
    }
}

/* Maps the Windows adapter media type to the portable connection enum. */
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

/* Avoids emitting duplicate neighbor entries when the table has repeated MACs. */
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

/* Avoids re-adding the same address twice within one snapshot pass. */
static long find_neighbor_ip(const lp_neighbor_list_t *list, const char *ip)
{
    if (ip == NULL || ip[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i].ip, ip) == 0) {
            return (long)i;
        }
    }
    return -1;
}

/* Some public/isolated Wi-Fi networks proxy-ARP: the access point answers for
   every client with its own MAC, so distinct devices end up sharing one MAC
   in our neighbor table. Treating that shared MAC as a stable identity would
   merge those devices into one entry (or hide all but one), so any MAC seen
   on more than one IP is blanked out and those neighbors fall back to
   IP-based identity instead. */
static void clear_ambiguous_macs(lp_neighbor_list_t *list)
{
    for (size_t i = 0; i < list->count; ++i) {
        if (list->items[i].mac[0] == '\0') {
            continue;
        }
        size_t sharing_count = 1;
        for (size_t j = i + 1; j < list->count; ++j) {
            if (strcmp(list->items[j].mac, list->items[i].mac) == 0) {
                ++sharing_count;
            }
        }
        if (sharing_count <= 1) {
            continue;
        }
        LP_DEBUG("clearing ambiguous MAC %s shared by %llu addresses (likely proxy ARP)",
                list->items[i].mac, (unsigned long long)sharing_count);
        char ambiguous_mac[LP_MAC_STR_MAX];
        snprintf(ambiguous_mac, sizeof(ambiguous_mac), "%s", list->items[i].mac);
        for (size_t j = i; j < list->count; ++j) {
            if (strcmp(list->items[j].mac, ambiguous_mac) == 0) {
                list->items[j].mac[0] = '\0';
            }
        }
    }
}

/* Matches active results against passive entries without duplicating devices. */
static long find_neighbor_index(const lp_neighbor_list_t *list, const char *ip,
                                const char *mac)
{
    if (mac != NULL && mac[0] != '\0') {
        const long by_mac = find_neighbor_mac(list, mac);
        if (by_mac >= 0) {
            return by_mac;
        }
    }
    if (ip == NULL || ip[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i].ip, ip) == 0) {
            return (long)i;
        }
    }
    return -1;
}

/* Adds one successful ARP response while preserving any richer passive data. */
static void append_active_neighbor(lp_neighbor_list_t *out, const char *ip,
                                    const char *mac, const NET_LUID *interface_luid)
{
    const long existing = find_neighbor_index(out, ip, mac);
    if (existing >= 0 || out->count >= LP_DISCOVERY_MAX_NEIGHBORS) {
        return;
    }

    lp_neighbor_t *neighbor = &out->items[out->count++];
    memset(neighbor, 0, sizeof(*neighbor));
    snprintf(neighbor->ip, sizeof(neighbor->ip), "%s", ip);
    snprintf(neighbor->mac, sizeof(neighbor->mac), "%s", mac);
    neighbor->connection_type = connection_type_for_interface(interface_luid);
    neighbor->hostname[0] = '\0';
    SOCKADDR_INET address;
    memset(&address, 0, sizeof(address));
    address.Ipv4.sin_family = AF_INET;
    InetPtonA(AF_INET, ip, &address.Ipv4.sin_addr);
    resolve_hostname(&address, neighbor->hostname, sizeof(neighbor->hostname));
    classify_neighbor(neighbor);
}

typedef struct {
    IPAddr destination;
    IPAddr source;
    UCHAR mac[6];
    ULONG mac_length;
    DWORD status;
} lp_arp_probe_t;

/* Runs one blocking ARP request away from the discovery worker's main loop. */
static DWORD WINAPI active_probe_thread_proc(LPVOID param)
{
    lp_arp_probe_t *probe = (lp_arp_probe_t *)param;
    probe->mac_length = sizeof(probe->mac);
    probe->status = SendARP(probe->destination, probe->source, (PULONG)probe->mac,
                            &probe->mac_length);
    return 0;
}

/* Probes bounded on-link IPv4 networks so idle devices need not be in the ARP cache. */
static size_t probe_active_subnets(lp_neighbor_list_t *out)
{
    ULONG buffer_size = 0;
    const ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    DWORD result = GetAdaptersAddresses(AF_INET, flags, NULL, NULL, &buffer_size);
    if (result != ERROR_BUFFER_OVERFLOW || buffer_size == 0) {
        return 0;
    }

    IP_ADAPTER_ADDRESSES *adapters =
        (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(), 0, buffer_size);
    if (adapters == NULL) {
        return 0;
    }
    result = GetAdaptersAddresses(AF_INET, flags, NULL, adapters, &buffer_size);
    if (result != NO_ERROR) {
        HeapFree(GetProcessHeap(), 0, adapters);
        return 0;
    }

    uint32_t scanned_networks[LP_ACTIVE_SCAN_MAX_SUBNETS] = {0};
    uint8_t scanned_prefixes[LP_ACTIVE_SCAN_MAX_SUBNETS] = {0};
    size_t scanned_count = 0;
    size_t discovered_count = 0;
    size_t probes_sent = 0;

    for (IP_ADAPTER_ADDRESSES *adapter = adapters; adapter != NULL;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress;
             unicast != NULL; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == NULL ||
                unicast->Address.lpSockaddr->sa_family != AF_INET ||
                unicast->OnLinkPrefixLength < LP_ACTIVE_SCAN_MAX_PREFIX) {
                continue;
            }

            const SOCKADDR_IN *local = (const SOCKADDR_IN *)unicast->Address.lpSockaddr;
            const uint32_t local_host = ntohl(local->sin_addr.S_un.S_addr);
            if ((local_host >> 24) == 169 && ((local_host >> 16) & 0xffu) == 254) {
                continue;
            }
            const uint32_t mask = 0xffffffffu << (32u - unicast->OnLinkPrefixLength);
            const uint32_t network = local_host & mask;
            bool already_scanned = false;
            for (size_t i = 0; i < scanned_count; ++i) {
                if (scanned_networks[i] == network &&
                    scanned_prefixes[i] == unicast->OnLinkPrefixLength) {
                    already_scanned = true;
                    break;
                }
            }
            if (already_scanned || scanned_count >= LP_ACTIVE_SCAN_MAX_SUBNETS) {
                continue;
            }
            scanned_networks[scanned_count] = network;
            scanned_prefixes[scanned_count++] = unicast->OnLinkPrefixLength;

            const uint32_t host_count = 1u << (32u - unicast->OnLinkPrefixLength);
            if (host_count <= 2) {
                continue;
            }
            const uint32_t usable_count = host_count - 2;
            const uint32_t start = g_active_probe_offset % usable_count;
            const uint32_t probes_this_subnet =
                (usable_count < LP_ACTIVE_SCAN_MAX_PROBES) ? usable_count
                                                           : LP_ACTIVE_SCAN_MAX_PROBES;
            lp_arp_probe_t probes[LP_ACTIVE_SCAN_MAX_PROBES] = {0};
            HANDLE probe_threads[LP_ACTIVE_SCAN_MAX_PROBES] = {0};
            size_t probe_count = 0;
            for (uint32_t probe = 0; probe < probes_this_subnet &&
                                       probes_sent < LP_ACTIVE_SCAN_MAX_PROBES &&
                                       probe_count < LP_ACTIVE_SCAN_MAX_PROBES;
                 ++probe) {
                const uint32_t host = 1 + ((start + probe) % usable_count);
                const uint32_t candidate = network + host;
                if (candidate == local_host) {
                    continue;
                }
                ++probes_sent;
                probes[probe_count].destination = htonl(candidate);
                probes[probe_count].source = local->sin_addr.S_un.S_addr;
                ++probe_count;
            }

            for (size_t i = 0; i < probe_count; ++i) {
                probe_threads[i] = CreateThread(NULL, 0, active_probe_thread_proc, &probes[i], 0,
                                                NULL);
                if (probe_threads[i] == NULL) {
                    probes[i].status = ERROR_NOT_ENOUGH_MEMORY;
                }
            }
            if (probe_count > 0) {
                (void)WaitForMultipleObjects((DWORD)probe_count, probe_threads, TRUE, 5000);
            }
            for (size_t i = 0; i < probe_count; ++i) {
                if (probe_threads[i] != NULL) {
                    CloseHandle(probe_threads[i]);
                }
                if (probes[i].status != NO_ERROR || probes[i].mac_length != sizeof(probes[i].mac)) {
                    continue;
                }
                char ip[LP_IP_STR_MAX] = {0};
                IN_ADDR address;
                address.S_un.S_addr = probes[i].destination;
                if (InetNtopA(AF_INET, &address, ip, sizeof(ip)) == NULL) {
                    continue;
                }
                char mac[LP_MAC_STR_MAX];
                format_mac(probes[i].mac, probes[i].mac_length, mac, sizeof(mac));
                const size_t before = out->count;
                append_active_neighbor(out, ip, mac, &adapter->Luid);
                if (out->count > before) {
                    ++discovered_count;
                }
                if (out->count >= LP_DISCOVERY_MAX_NEIGHBORS) {
                    HeapFree(GetProcessHeap(), 0, adapters);
                    return discovered_count;
                }
            }
            g_active_probe_offset = (uint32_t)((start + probe_count) % usable_count);
            if (probes_sent >= LP_ACTIVE_SCAN_MAX_PROBES) {
                HeapFree(GetProcessHeap(), 0, adapters);
                return discovered_count;
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, adapters);
    return discovered_count;
}

/* Reads the passive ARP/NDP table and enriches live entries with best-effort names. */
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

        if (find_neighbor_ip(out, ip) >= 0) {
            continue;
        }
        lp_neighbor_t *neighbor = &out->items[out->count];
        memset(neighbor, 0, sizeof(*neighbor));
        snprintf(neighbor->ip, sizeof(neighbor->ip), "%s", ip);
        format_mac(row->PhysicalAddress, row->PhysicalAddressLength, neighbor->mac,
                   sizeof(neighbor->mac));
        neighbor->connection_type = connection_type_for_interface(&row->InterfaceLuid);
        neighbor->hostname[0] = '\0';
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
    const size_t active_discovered = probe_active_subnets(out);
    if (active_discovered > 0) {
        LP_DEBUG("active discovery added %llu neighbor(s)", (unsigned long long)active_discovered);
    }
    clear_ambiguous_macs(out);
    return LP_OK;
}

/* Converts an IPv4 or IPv6 Windows socket address to presentation text. */
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

/* Resolves the gateway and adapter selected by the default route. */
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

/* Looks up the gateway's MAC in the current IPv4 neighbor table. */
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

/* Enumerates local prefixes and attaches gateway identity to the default adapter. */
lp_status_t lp_net_local_networks(lp_local_network_list_t *out)
{
    if (out == NULL) {
        return LP_ERR_INVALID_ARG;
    }
    out->count = 0;
    DWORD hostname_size = LP_HOSTNAME_MAX;
    (void)GetComputerNameExA(ComputerNamePhysicalDnsHostname, out->local_hostname,
                             &hostname_size);

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
        const bool is_default_adapter =
            has_default_gateway && adapter->Luid.Value == default_interface_luid.Value;
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
            if (out->local_ip[0] == '\0' &&
                unicast->Address.lpSockaddr->sa_family == AF_INET && is_default_adapter) {
                snprintf(out->local_ip, sizeof(out->local_ip), "%s", address);
                out->local_connection_type = connection_type_for_interface(&adapter->Luid);
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, adapters);
    return LP_OK;
}
