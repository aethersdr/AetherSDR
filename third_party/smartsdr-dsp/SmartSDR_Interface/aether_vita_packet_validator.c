/* Production admission policy for Flex digital-voice VITA audio packets. */

#include "aether_vita_packet_validator.h"

#include "crdv.h"

int aether_vita_packet_validate(const uint8_t *packet,
                                size_t length,
                                uint32_t expected_stream)
{
    crdv_vita_audio decoded;
    return crdv_vita_parse_audio(packet, length, expected_stream, &decoded)
        == CRDV_OK ? 0 : -1;
}
