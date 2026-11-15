#include "aether_vita_packet_validator.h"
#include "crdv.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

enum {
    VITA_HEADER_BYTES = 28U,
    VITA_PACKET_BYTES = 1052U
};

static void expect(const char *name, int condition)
{
    printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", name);
    failures += condition ? 0 : 1;
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

int main(void)
{
    const uint32_t stream = 0x4a000001U;
    crdv_vita_audio audio;
    memset(&audio, 0, sizeof(audio));
    audio.stream_id = stream;
    uint8_t packet[VITA_PACKET_BYTES + 1U];
    size_t written = 0U;
    expect("fixture builds", crdv_vita_build_audio(
        &audio, packet, sizeof(packet), &written) == CRDV_OK);
    expect("valid production packet is admitted",
           aether_vita_packet_validate(packet, written, stream) == 0);
    expect("trailing byte is rejected",
           aether_vita_packet_validate(packet, written + 1U, stream) != 0);

    packet[12] ^= 0x01U;
    expect("wrong information class is rejected",
           aether_vita_packet_validate(packet, written, stream) != 0);
    packet[12] ^= 0x01U;

    packet[0] = 0x10U;
    expect("wrong packet flags are rejected",
           aether_vita_packet_validate(packet, written, stream) != 0);
    packet[0] = 0x18U;

    uint32_t non_finite = 0x7fc00000U;
    write_be32(packet + VITA_HEADER_BYTES, non_finite);
    expect("non-finite audio is rejected",
           aether_vita_packet_validate(packet, written, stream) != 0);

    return failures == 0 ? 0 : 1;
}
