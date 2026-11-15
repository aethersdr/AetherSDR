#ifndef VITA_PACKET_SEQUENCE_H_
#define VITA_PACKET_SEQUENCE_H_

#include "datatypes.h"

#define VITA_PACKET_SEQUENCE_STREAM_SLOTS 64U

typedef enum _vita_packet_sequence_status {
    VITA_PACKET_SEQUENCE_FIRST,
    VITA_PACKET_SEQUENCE_IN_ORDER,
    VITA_PACKET_SEQUENCE_GAP,
    VITA_PACKET_SEQUENCE_DUPLICATE
} vita_packet_sequence_status;

typedef struct _vita_packet_sequence_result {
    vita_packet_sequence_status status;
    uint32 expected;
    uint32 received;
    uint32 missing;
} vita_packet_sequence_result;

typedef struct _vita_packet_sequence_entry {
    uint32 stream_id;
    uint32 last_count;
    uint32 next_count;
    BOOL in_use;
} vita_packet_sequence_entry;

typedef struct _vita_packet_sequence_tracker {
    vita_packet_sequence_entry entries[VITA_PACKET_SEQUENCE_STREAM_SLOTS];
} vita_packet_sequence_tracker;

void vita_packet_sequence_reset(vita_packet_sequence_tracker * tracker);
vita_packet_sequence_result vita_packet_sequence_observe(
    vita_packet_sequence_tracker * tracker,
    uint32 stream_id,
    uint32 packet_count);

#endif /* VITA_PACKET_SEQUENCE_H_ */
