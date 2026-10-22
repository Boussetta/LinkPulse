#include "lp_test.h"

#include "linkpulse/capture.h"

#include <string.h>

static const uint8_t DEVICE_V4[4] = {192, 168, 1, 20};
static const uint8_t PEER_V4[4] = {192, 168, 1, 1};
static const uint8_t DEVICE_V6[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x20};
static const uint8_t PEER_V6[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};

static lp_capture_address_t ipv4_device(void)
{
    lp_capture_address_t device;
    memset(&device, 0, sizeof(device));
    memcpy(device.bytes, DEVICE_V4, sizeof(DEVICE_V4));
    return device;
}

static lp_capture_address_t ipv6_device(void)
{
    lp_capture_address_t device;
    memset(&device, 0, sizeof(device));
    device.is_ipv6 = true;
    memcpy(device.bytes, DEVICE_V6, sizeof(DEVICE_V6));
    return device;
}

/* Writes an Ethernet header (optionally 802.1Q tagged) and returns the IP header offset. */
static size_t write_ethernet(uint8_t *frame, unsigned ethertype, bool vlan)
{
    memset(frame, 0, 12);
    size_t offset = 12;
    if (vlan) {
        frame[offset++] = 0x81;
        frame[offset++] = 0x00;
        frame[offset++] = 0x00;
        frame[offset++] = 0x05;
    }
    frame[offset++] = (uint8_t)(ethertype >> 8);
    frame[offset++] = (uint8_t)(ethertype & 0xFFu);
    return offset;
}

static size_t build_ipv4(uint8_t *frame, const uint8_t *source, const uint8_t *destination,
                         bool vlan, unsigned ihl_words)
{
    const size_t ip = write_ethernet(frame, 0x0800, vlan);
    memset(frame + ip, 0, 60);
    frame[ip] = (uint8_t)(0x40u | (ihl_words & 0x0Fu));
    memcpy(frame + ip + 12, source, 4);
    memcpy(frame + ip + 16, destination, 4);
    return ip + 20;
}

static size_t build_ipv6(uint8_t *frame, const uint8_t *source, const uint8_t *destination)
{
    const size_t ip = write_ethernet(frame, 0x86DD, false);
    memset(frame + ip, 0, 40);
    frame[ip] = 0x60;
    memcpy(frame + ip + 8, source, 16);
    memcpy(frame + ip + 24, destination, 16);
    return ip + 40;
}

static void test_ipv4_direction_uses_full_addresses(void)
{
    uint8_t frame[128];
    const lp_capture_address_t device = ipv4_device();

    size_t length = build_ipv4(frame, DEVICE_V4, PEER_V4, false, 5);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length, &device) ==
             LP_CAPTURE_FRAME_FROM_DEVICE);

    length = build_ipv4(frame, PEER_V4, DEVICE_V4, false, 5);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length, &device) ==
             LP_CAPTURE_FRAME_TO_DEVICE);

    /* Same first octet as the device, so the old first-byte comparison misclassified it. */
    const uint8_t other[4] = {192, 168, 1, 99};
    length = build_ipv4(frame, other, PEER_V4, false, 5);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length, &device) ==
             LP_CAPTURE_FRAME_IGNORED);
}

static void test_vlan_tagged_frame_is_parsed(void)
{
    uint8_t frame[128];
    const lp_capture_address_t device = ipv4_device();
    const size_t length = build_ipv4(frame, PEER_V4, DEVICE_V4, true, 5);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length, &device) ==
             LP_CAPTURE_FRAME_TO_DEVICE);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, 17, &device) ==
             LP_CAPTURE_FRAME_IGNORED);
}

static void test_ipv6_frames_match_ipv6_devices_only(void)
{
    uint8_t frame[128];
    const lp_capture_address_t device = ipv6_device();
    size_t length = build_ipv6(frame, DEVICE_V6, PEER_V6);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length, &device) ==
             LP_CAPTURE_FRAME_FROM_DEVICE);
    length = build_ipv6(frame, PEER_V6, DEVICE_V6);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length, &device) ==
             LP_CAPTURE_FRAME_TO_DEVICE);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length - 1, &device) ==
             LP_CAPTURE_FRAME_IGNORED);

    const lp_capture_address_t v4 = ipv4_device();
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length, &v4) ==
             LP_CAPTURE_FRAME_IGNORED);
}

static void test_truncated_and_bad_headers_are_ignored(void)
{
    uint8_t frame[128];
    const lp_capture_address_t device = ipv4_device();
    const size_t length = build_ipv4(frame, DEVICE_V4, PEER_V4, false, 5);
    for (size_t caplen = 0; caplen < length; ++caplen) {
        LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, caplen, &device) ==
                 LP_CAPTURE_FRAME_IGNORED);
    }

    /* IHL below the 20-byte minimum. */
    (void)build_ipv4(frame, DEVICE_V4, PEER_V4, false, 4);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length, &device) ==
             LP_CAPTURE_FRAME_IGNORED);

    /* IHL claims options that were not captured. */
    (void)build_ipv4(frame, DEVICE_V4, PEER_V4, false, 15);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length, &device) ==
             LP_CAPTURE_FRAME_IGNORED);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, 14 + 60, &device) ==
             LP_CAPTURE_FRAME_FROM_DEVICE);

    /* Version nibble does not match the EtherType. */
    (void)build_ipv4(frame, DEVICE_V4, PEER_V4, false, 5);
    frame[14] = 0x65;
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length, &device) ==
             LP_CAPTURE_FRAME_IGNORED);
}

static void test_non_ip_and_unknown_links_are_ignored(void)
{
    uint8_t frame[128];
    const lp_capture_address_t device = ipv4_device();
    size_t length = build_ipv4(frame, DEVICE_V4, PEER_V4, false, 5);
    LP_CHECK(lp_capture_classify_frame(0, frame, length, &device) == LP_CAPTURE_FRAME_IGNORED);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, NULL, length, &device) ==
             LP_CAPTURE_FRAME_IGNORED);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length, NULL) ==
             LP_CAPTURE_FRAME_IGNORED);

    length = write_ethernet(frame, 0x0806, false); /* ARP */
    memset(frame + length, 0, 40);
    LP_CHECK(lp_capture_classify_frame(LP_CAPTURE_LINKTYPE_ETHERNET, frame, length + 40,
                                       &device) == LP_CAPTURE_FRAME_IGNORED);
}

int main(void)
{
    test_ipv4_direction_uses_full_addresses();
    test_vlan_tagged_frame_is_parsed();
    test_ipv6_frames_match_ipv6_devices_only();
    test_truncated_and_bad_headers_are_ignored();
    test_non_ip_and_unknown_links_are_ignored();
    LP_TEST_RETURN();
}
