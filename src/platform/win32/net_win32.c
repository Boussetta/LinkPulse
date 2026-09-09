#include "linkpulse/net.h"

#include <winsock2.h>
#include <ws2tcpip.h>
/* windows.h must follow winsock2.h, and iphlpapi.h must follow both. */
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Converts Windows adapter names to the UTF-8 strings used by the portable core. */
static void wide_to_utf8(const WCHAR *src, char *dst, size_t dst_cap)
{
    if (dst == NULL || dst_cap == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL || src[0] == L'\0') {
        return;
    }
    const size_t bounded_dst_cap = (dst_cap > (size_t)INT_MAX) ? (size_t)INT_MAX : dst_cap;
    const int written =
        WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)bounded_dst_cap, NULL, NULL);
    if (written <= 0) {
        dst[0] = '\0';
    }
}

/* Converts GetIfTable2 rows into a heap-owned portable interface snapshot. */
lp_status_t lp_net_snapshot(lp_iface_list_t *out)
{
    if (out == NULL) {
        return LP_ERR_INVALID_ARG;
    }
    out->items = NULL;
    out->count = 0;

    MIB_IF_TABLE2 *table = NULL;
    if (GetIfTable2(&table) != NO_ERROR || table == NULL) {
        return LP_ERR_IO;
    }

    lp_status_t status = LP_OK;
    lp_iface_t *items = NULL;

    if (table->NumEntries > 0) {
        items = calloc(table->NumEntries, sizeof(*items));
        if (items == NULL) {
            status = LP_ERR_NO_MEMORY;
            goto done;
        }
    }

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2 *row = &table->Table[i];
        lp_iface_t *iface = &items[i];

        wide_to_utf8(row->Alias, iface->name, sizeof(iface->name));
        wide_to_utf8(row->Description, iface->description, sizeof(iface->description));

        iface->rx_bytes = row->InOctets;
        iface->tx_bytes = row->OutOctets;
        iface->is_up = (row->OperStatus == IfOperStatusUp);
        iface->is_loopback = (row->Type == IF_TYPE_SOFTWARE_LOOPBACK);
        /* Anything the OS does not back with real hardware: WSL vEthernet, Hyper-V,
           VPN tunnels, and packet-filter pseudo-adapters. */
        iface->is_virtual = !row->InterfaceAndOperStatusFlags.HardwareInterface ||
                            row->InterfaceAndOperStatusFlags.FilterInterface;
    }

    out->items = items;
    out->count = (size_t)table->NumEntries;
    items = NULL;

done:
    free(items);
    FreeMibTable(table);
    return status;
}

/* Resolves the adapter selected by Windows for a public default-route destination. */
lp_status_t lp_net_default_iface(char *name_out, size_t name_cap)
{
    if (name_out == NULL || name_cap == 0) {
        return LP_ERR_INVALID_ARG;
    }
    name_out[0] = '\0';

    /* Any routable public address works; this resolves the default route without
       sending a packet. */
    SOCKADDR_INET destination;
    memset(&destination, 0, sizeof(destination));
    destination.Ipv4.sin_family = AF_INET;
    destination.Ipv4.sin_addr.S_un.S_addr = htonl(0x08080808); /* 8.8.8.8 */

    MIB_IPFORWARD_ROW2 route;
    SOCKADDR_INET best_source;
    if (GetBestRoute2(NULL, 0, NULL, &destination, 0, &route, &best_source) != NO_ERROR) {
        return LP_ERR_NOT_FOUND;
    }

    WCHAR alias[IF_MAX_STRING_SIZE + 1];
    if (ConvertInterfaceLuidToAlias(&route.InterfaceLuid, alias, ARRAYSIZE(alias)) != NO_ERROR) {
        return LP_ERR_NOT_FOUND;
    }

    wide_to_utf8(alias, name_out, name_cap);
    return (name_out[0] != '\0') ? LP_OK : LP_ERR_NOT_FOUND;
}
