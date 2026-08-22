#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "psk_reporter.h"

static uint16_t read_u16(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8U) | source[1]);
}

static uint32_t read_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) |
           ((uint32_t)source[1] << 16U) |
           ((uint32_t)source[2] << 8U) | source[3];
}

static size_t find_bytes(const uint8_t *data, size_t size,
                         const uint8_t *needle, size_t needle_size)
{
    for (size_t index = 0U; index + needle_size <= size; index++) {
        if (memcmp(data + index, needle, needle_size) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

int main(void)
{
    const uint8_t receiver_record[] = {0x99, 0x92};
    const uint8_t sender_record[] = {0x99, 0x93};
    const uint8_t encoded_sender[] = {
        0x04, 'R', '7', 'I', 'W',
        0x00, 0xd6, 0xc0, 0x1b,
        0xf6,
        0x03, 'F', 'T', '8',
        0x01,
        0x65, 0x53, 0xf1, 0x00
    };
    struct psk_report report = {
        .sender_call = "R7IW",
        .frequency_hz = 14073883U,
        .snr = -10,
        .flow_start = 1700000000
    };
    uint8_t packet[PSK_REPORT_PACKET_CAPACITY];
    size_t packet_size = 0U;
    struct psk_reporter reporter;

    assert(psk_reporter_build_packet(packet, sizeof(packet),
                                     "VU3CER", "MK68xm",
                                     "airspyhf-wsprd 0.8", &report, 1U,
                                     42U, 0x12345678U, 1700000015,
                                     true, &packet_size) == 0);
    assert(packet[0] == 0x00 && packet[1] == 0x0a);
    assert(read_u16(packet + 2U) == packet_size);
    assert(read_u32(packet + 4U) == 1700000015U);
    assert(read_u32(packet + 8U) == 42U);
    assert(read_u32(packet + 12U) == 0x12345678U);
    assert(find_bytes(packet, packet_size, receiver_record,
                      sizeof(receiver_record)) != SIZE_MAX);
    assert(find_bytes(packet, packet_size, sender_record,
                      sizeof(sender_record)) != SIZE_MAX);
    assert(find_bytes(packet, packet_size, encoded_sender,
                      sizeof(encoded_sender)) != SIZE_MAX);

    assert(psk_reporter_start(&reporter, "VU3CER", "MK68xm",
                              "airspyhf-wsprd test") == 0);
    assert(psk_reporter_enqueue(&reporter, &report));
    report.snr = -5;
    assert(psk_reporter_enqueue(&reporter, &report));
    assert(psk_reporter_pending(&reporter) == 1U);
    psk_reporter_stop(&reporter);

    puts("PSK Reporter: IPFIX fields and RAM duplicate suppression verified");
    return 0;
}
