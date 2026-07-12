#include "vita_packet_sequence.h"

#include <string.h>

void vita_packet_sequence_reset(vita_packet_sequence_tracker * tracker)
{
    if (tracker != NULL) {
        memset(tracker, 0, sizeof(*tracker));
    }
}

vita_packet_sequence_result vita_packet_sequence_observe(
    vita_packet_sequence_tracker * tracker,
    uint32 stream_id,
    uint32 packet_count)
{
    vita_packet_sequence_result result = {
        .status = VITA_PACKET_SEQUENCE_FIRST,
        .expected = packet_count & 0xFU,
        .received = packet_count & 0xFU,
        .missing = 0U
    };
    if (tracker == NULL) {
        return result;
    }

    const uint32 received = packet_count & 0xFU;
    uint32 free_slot = VITA_PACKET_SEQUENCE_STREAM_SLOTS;
    uint32 i;
    for (i = 0U; i < VITA_PACKET_SEQUENCE_STREAM_SLOTS; i++) {
        vita_packet_sequence_entry * entry = &tracker->entries[i];
        if (entry->in_use && entry->stream_id == stream_id) {
            result.expected = entry->next_count;
            if (received == entry->next_count) {
                result.status = VITA_PACKET_SEQUENCE_IN_ORDER;
                entry->last_count = received;
                entry->next_count = (received + 1U) & 0xFU;
            } else if (received == entry->last_count) {
                result.status = VITA_PACKET_SEQUENCE_DUPLICATE;
            } else {
                result.status = VITA_PACKET_SEQUENCE_GAP;
                result.missing = (received - entry->next_count) & 0xFU;
                entry->last_count = received;
                entry->next_count = (received + 1U) & 0xFU;
            }
            return result;
        }
        if (!entry->in_use && free_slot == VITA_PACKET_SEQUENCE_STREAM_SLOTS) {
            free_slot = i;
        }
    }

    if (free_slot == VITA_PACKET_SEQUENCE_STREAM_SLOTS) {
        free_slot = stream_id % VITA_PACKET_SEQUENCE_STREAM_SLOTS;
    }
    vita_packet_sequence_entry * entry = &tracker->entries[free_slot];
    entry->in_use = TRUE;
    entry->stream_id = stream_id;
    entry->last_count = received;
    entry->next_count = (received + 1U) & 0xFU;
    return result;
}
