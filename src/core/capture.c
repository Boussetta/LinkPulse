#include "linkpulse/capture.h"

#include <string.h>

#define LP_ETHERNET_HEADER_LEN 14u
#define LP_VLAN_TAG_LEN 4u
#define LP_ETHERTYPE_IPV4 0x0800u
#define LP_ETHERTYPE_IPV6 0x86DDu
#define LP_ETHERTYPE_VLAN 0x8100u
#define LP_IPV4_MIN_HEADER_LEN 20u
#define LP_IPV6_HEADER_LEN 40u

static unsigned read_be16(const uint8_t *bytes)
{
    return ((unsigned)bytes[0] << 8) | (unsigned)bytes[1];
}

static void store_peer(lp_capture_address_t *peer, const uint8_t *bytes, size_t length)
{
    if (peer == NULL) {
        return;
    }
    memset(peer, 0, sizeof(*peer));
    peer->is_ipv6 = length == 16;
    memcpy(peer->bytes, bytes, length);
}

static lp_capture_direction_t classify_addresses(const uint8_t *source, const uint8_t *destination,
                                                 size_t length, const lp_capture_address_t *device,
                                                 lp_capture_address_t *peer)
{
    if (memcmp(source, device->bytes, length) == 0) {
        store_peer(peer, destination, length);
        return LP_CAPTURE_FRAME_FROM_DEVICE;
    }
    if (memcmp(destination, device->bytes, length) == 0) {
        store_peer(peer, source, length);
        return LP_CAPTURE_FRAME_TO_DEVICE;
    }
    return LP_CAPTURE_FRAME_IGNORED;
}

lp_capture_direction_t lp_capture_classify_frame(int link_type, const uint8_t *frame,
                                                 size_t caplen,
                                                 const lp_capture_address_t *device)
{
    return lp_capture_classify_frame_peer(link_type, frame, caplen, device, NULL);
}

bool lp_capture_address_in(const lp_capture_address_t *address, const lp_capture_address_t *set,
                           size_t count)
{
    if (address == NULL || set == NULL) {
        return false;
    }
    const size_t length = address->is_ipv6 ? 16u : 4u;
    for (size_t i = 0; i < count; ++i) {
        if (set[i].is_ipv6 == address->is_ipv6 &&
            memcmp(set[i].bytes, address->bytes, length) == 0) {
            return true;
        }
    }
    return false;
}

lp_capture_direction_t lp_capture_classify_frame_peer(int link_type, const uint8_t *frame,
                                                      size_t caplen,
                                                      const lp_capture_address_t *device,
                                                      lp_capture_address_t *peer)
{
    if (frame == NULL || device == NULL || link_type != LP_CAPTURE_LINKTYPE_ETHERNET ||
        caplen < LP_ETHERNET_HEADER_LEN) {
        return LP_CAPTURE_FRAME_IGNORED;
    }

    size_t offset = LP_ETHERNET_HEADER_LEN;
    unsigned ethertype = read_be16(frame + 12);
    if (ethertype == LP_ETHERTYPE_VLAN) {
        if (caplen < LP_ETHERNET_HEADER_LEN + LP_VLAN_TAG_LEN) {
            return LP_CAPTURE_FRAME_IGNORED;
        }
        ethertype = read_be16(frame + 16);
        offset += LP_VLAN_TAG_LEN;
    }

    const uint8_t *ip = frame + offset;
    const size_t remaining = caplen - offset;
    if (ethertype == LP_ETHERTYPE_IPV4) {
        if (device->is_ipv6 || remaining < LP_IPV4_MIN_HEADER_LEN || (ip[0] >> 4) != 4) {
            return LP_CAPTURE_FRAME_IGNORED;
        }
        const size_t header_len = (size_t)(ip[0] & 0x0Fu) * 4u;
        if (header_len < LP_IPV4_MIN_HEADER_LEN || header_len > remaining) {
            return LP_CAPTURE_FRAME_IGNORED;
        }
        return classify_addresses(ip + 12, ip + 16, 4, device, peer);
    }
    if (ethertype == LP_ETHERTYPE_IPV6) {
        if (!device->is_ipv6 || remaining < LP_IPV6_HEADER_LEN || (ip[0] >> 4) != 6) {
            return LP_CAPTURE_FRAME_IGNORED;
        }
        return classify_addresses(ip + 8, ip + 24, 16, device, peer);
    }
    return LP_CAPTURE_FRAME_IGNORED;
}
