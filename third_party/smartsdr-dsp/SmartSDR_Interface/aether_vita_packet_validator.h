/* Production admission policy for Flex digital-voice VITA audio packets. */

#ifndef AETHER_VITA_PACKET_VALIDATOR_H_
#define AETHER_VITA_PACKET_VALIDATOR_H_

#include <stddef.h>
#include <stdint.h>

int aether_vita_packet_validate(const uint8_t *packet,
                                size_t length,
                                uint32_t expected_stream);

#endif /* AETHER_VITA_PACKET_VALIDATOR_H_ */
