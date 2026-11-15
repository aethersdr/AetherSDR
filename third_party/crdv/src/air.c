/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "crdv.h"

#include <limits.h>
#include <string.h>

static uint8_t bit_get(const uint8_t *packed, size_t index)
{
    return (uint8_t)((packed[index / 8u] >> (index % 8u)) & 1u);
}

static void bit_set(uint8_t *packed, size_t index, uint8_t value)
{
    uint8_t mask = (uint8_t)(1u << (index % 8u));
    if ((value & 1u) != 0u) {
        packed[index / 8u] |= mask;
    } else {
        packed[index / 8u] &= (uint8_t)~mask;
    }
}

uint16_t crdv_pfcs(const uint8_t *bytes, size_t length)
{
    uint16_t value = 0xffffu;
    if (bytes == NULL && length != 0u) {
        return 0u;
    }
    for (size_t index = 0; index < length; ++index) {
        value ^= bytes[index];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            value = (value & 1u) != 0u
                        ? (uint16_t)((value >> 1u) ^ 0x8408u)
                        : (uint16_t)(value >> 1u);
        }
    }
    return (uint16_t)~value;
}

crdv_result crdv_header_pack(const crdv_header_fields *fields,
                             uint8_t out[CRDV_HEADER_BYTES])
{
    uint16_t check;
    if (fields == NULL || out == NULL) {
        return CRDV_E_ARGUMENT;
    }
    memcpy(out, fields->flags, 3u);
    memcpy(out + 3u, fields->rpt2, 8u);
    memcpy(out + 11u, fields->rpt1, 8u);
    memcpy(out + 19u, fields->urcall, 8u);
    memcpy(out + 27u, fields->mycall, 8u);
    memcpy(out + 35u, fields->suffix, 4u);
    check = crdv_pfcs(out, 39u);
    out[39] = (uint8_t)(check & 0xffu);
    out[40] = (uint8_t)(check >> 8u);
    return CRDV_OK;
}

crdv_result crdv_header_unpack(const uint8_t in[CRDV_HEADER_BYTES],
                               crdv_header_fields *fields)
{
    uint16_t stored;
    if (in == NULL || fields == NULL) {
        return CRDV_E_ARGUMENT;
    }
    stored = (uint16_t)in[39] | (uint16_t)((uint16_t)in[40] << 8u);
    if (stored != crdv_pfcs(in, 39u)) {
        return CRDV_E_CHECK;
    }
    memcpy(fields->flags, in, 3u);
    memcpy(fields->rpt2, in + 3u, 8u);
    memcpy(fields->rpt1, in + 11u, 8u);
    memcpy(fields->urcall, in + 19u, 8u);
    memcpy(fields->mycall, in + 27u, 8u);
    memcpy(fields->suffix, in + 35u, 4u);
    return CRDV_OK;
}

static void scrambler_bits(uint8_t *bits, size_t length)
{
    uint8_t state = 0x7fu;
    for (size_t index = 0; index < length; ++index) {
        uint8_t feedback = (uint8_t)(((state >> 6u) ^ (state >> 3u)) & 1u);
        bits[index] ^= feedback;
        state = (uint8_t)(((uint32_t)state << 1u | (uint32_t)feedback) & 0x7fu);
    }
}

crdv_result crdv_header_protect(const uint8_t in[CRDV_HEADER_BYTES],
                                uint8_t out[CRDV_PROTECTED_PACKED_BYTES])
{
    uint8_t coded[CRDV_PROTECTED_BITS];
    uint8_t transmitted[CRDV_PROTECTED_BITS];
    uint8_t delay1 = 0u;
    uint8_t delay2 = 0u;
    size_t coded_index = 0u;
    size_t transmitted_index = 0u;

    if (in == NULL || out == NULL) {
        return CRDV_E_ARGUMENT;
    }
    for (size_t input_index = 0; input_index < 330u; ++input_index) {
        uint8_t input = input_index < 328u ? bit_get(in, input_index) : 0u;
        coded[coded_index++] = (uint8_t)(input ^ delay1 ^ delay2);
        coded[coded_index++] = (uint8_t)(input ^ delay2);
        delay2 = delay1;
        delay1 = input;
    }
    for (size_t row = 0; row < 24u; ++row) {
        for (size_t column = 0; column < 28u; ++column) {
            size_t source = row + 24u * column;
            if (source < CRDV_PROTECTED_BITS) {
                transmitted[transmitted_index++] = coded[source];
            }
        }
    }
    scrambler_bits(transmitted, CRDV_PROTECTED_BITS);
    memset(out, 0, CRDV_PROTECTED_PACKED_BYTES);
    for (size_t index = 0; index < CRDV_PROTECTED_BITS; ++index) {
        bit_set(out, index, transmitted[index]);
    }
    return CRDV_OK;
}

crdv_result crdv_header_recover(const uint8_t in[CRDV_PROTECTED_PACKED_BYTES],
                                uint8_t out[CRDV_HEADER_BYTES],
                                unsigned *path_metric)
{
    uint8_t transmitted[CRDV_PROTECTED_BITS];
    uint8_t coded[CRDV_PROTECTED_BITS];
    unsigned metrics[4] = {0u, UINT_MAX / 4u, UINT_MAX / 4u, UINT_MAX / 4u};
    uint8_t predecessor[330][4];
    uint8_t decision[330][4];
    uint8_t recovered[330];
    size_t transmitted_index = 0u;

    if (in == NULL || out == NULL) {
        return CRDV_E_ARGUMENT;
    }
    for (size_t index = 0; index < CRDV_PROTECTED_BITS; ++index) {
        transmitted[index] = bit_get(in, index);
    }
    scrambler_bits(transmitted, CRDV_PROTECTED_BITS);
    memset(coded, 0, sizeof(coded));
    for (size_t row = 0; row < 24u; ++row) {
        for (size_t column = 0; column < 28u; ++column) {
            size_t destination = row + 24u * column;
            if (destination < CRDV_PROTECTED_BITS) {
                coded[destination] = transmitted[transmitted_index++];
            }
        }
    }

    for (size_t step = 0; step < 330u; ++step) {
        unsigned next[4] = {UINT_MAX / 4u, UINT_MAX / 4u, UINT_MAX / 4u,
                            UINT_MAX / 4u};
        for (unsigned state = 0; state < 4u; ++state) {
            if (metrics[state] >= UINT_MAX / 8u) {
                continue;
            }
            for (unsigned input = 0; input < 2u; ++input) {
                unsigned delay1 = state & 1u;
                unsigned delay2 = (state >> 1u) & 1u;
                unsigned first = input ^ delay1 ^ delay2;
                unsigned second = input ^ delay2;
                unsigned distance = first ^ coded[step * 2u];
                unsigned next_state;
                unsigned candidate;
                distance += second ^ coded[step * 2u + 1u];
                next_state = input | (delay1 << 1u);
                candidate = metrics[state] + distance;
                if (candidate < next[next_state]) {
                    next[next_state] = candidate;
                    predecessor[step][next_state] = (uint8_t)state;
                    decision[step][next_state] = (uint8_t)input;
                }
            }
        }
        memcpy(metrics, next, sizeof(metrics));
    }

    {
        unsigned state = 0u;
        if (path_metric != NULL) {
            *path_metric = metrics[state];
        }
        for (size_t offset = 330u; offset > 0u; --offset) {
            size_t step = offset - 1u;
            recovered[step] = decision[step][state];
            state = predecessor[step][state];
        }
    }
    memset(out, 0, CRDV_HEADER_BYTES);
    for (size_t index = 0; index < 328u; ++index) {
        bit_set(out, index, recovered[index]);
    }
    {
        crdv_header_fields fields;
        return crdv_header_unpack(out, &fields);
    }
}

void crdv_slow_xor(uint8_t fragment[CRDV_SLOW_FRAGMENT_BYTES])
{
    static const uint8_t mask[3] = {0x70u, 0x4fu, 0x93u};
    if (fragment != NULL) {
        for (size_t index = 0; index < 3u; ++index) {
            fragment[index] ^= mask[index];
        }
    }
}

crdv_result crdv_message_block(const uint8_t message[CRDV_MESSAGE_BYTES],
                               unsigned block_number,
                               uint8_t first[CRDV_SLOW_FRAGMENT_BYTES],
                               uint8_t second[CRDV_SLOW_FRAGMENT_BYTES])
{
    uint8_t block[6];
    if (message == NULL || first == NULL || second == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (block_number < 1u || block_number > 4u) {
        return CRDV_E_RANGE;
    }
    block[0] = (uint8_t)(0x40u + (block_number - 1u));
    memcpy(block + 1u, message + (block_number - 1u) * 5u, 5u);
    memcpy(first, block, 3u);
    memcpy(second, block + 3u, 3u);
    crdv_slow_xor(first);
    crdv_slow_xor(second);
    return CRDV_OK;
}

void crdv_message_assembler_reset(crdv_message_assembler *assembler,
                                  uint32_t session)
{
    if (assembler != NULL) {
        memset(assembler, 0, sizeof(*assembler));
        assembler->session = session;
    }
}

crdv_result crdv_message_assembler_push(crdv_message_assembler *assembler,
                                        uint32_t session,
                                        const uint8_t six_bytes[6],
                                        uint8_t complete[CRDV_MESSAGE_BYTES],
                                        bool *new_message)
{
    unsigned slot;
    uint8_t mask;
    if (assembler == NULL || six_bytes == NULL || complete == NULL ||
        new_message == NULL) {
        return CRDV_E_ARGUMENT;
    }
    *new_message = false;
    if (assembler->session != session) {
        crdv_message_assembler_reset(assembler, session);
    }
    if (assembler->conflicted) {
        return CRDV_E_STATE;
    }
    if (six_bytes[0] < 0x40u || six_bytes[0] > 0x43u) {
        return CRDV_E_FORMAT;
    }
    slot = (unsigned)(six_bytes[0] - 0x40u);
    mask = (uint8_t)(1u << slot);
    if ((assembler->present_mask & mask) != 0u) {
        if (memcmp(assembler->slots[slot], six_bytes + 1u, 5u) != 0) {
            assembler->present_mask = 0u;
            assembler->published = false;
            assembler->conflicted = true;
            return CRDV_E_CHECK;
        }
        return CRDV_OK;
    }
    memcpy(assembler->slots[slot], six_bytes + 1u, 5u);
    assembler->present_mask |= mask;
    if (assembler->present_mask == 0x0fu && !assembler->published) {
        for (size_t index = 0; index < 4u; ++index) {
            memcpy(complete + index * 5u, assembler->slots[index], 5u);
        }
        assembler->published = true;
        *new_message = true;
    }
    return CRDV_OK;
}

crdv_result crdv_voice_frame_pack(const uint8_t ambe[CRDV_AMBE_BYTES],
                                  const uint8_t slow[CRDV_SLOW_FRAGMENT_BYTES],
                                  uint8_t bits[96])
{
    if (ambe == NULL || slow == NULL || bits == NULL) {
        return CRDV_E_ARGUMENT;
    }
    for (size_t index = 0; index < 72u; ++index) {
        bits[index] = bit_get(ambe, index);
    }
    for (size_t index = 0; index < 24u; ++index) {
        bits[72u + index] = bit_get(slow, index);
    }
    return CRDV_OK;
}

crdv_result crdv_transmission_prefix(const uint8_t header[CRDV_HEADER_BYTES],
                                     size_t bit_sync_count, uint8_t *bits,
                                     size_t capacity, size_t *written)
{
    static const char frame_sync[] = "111011001010000";
    uint8_t protected_bytes[CRDV_PROTECTED_PACKED_BYTES];
    size_t needed;
    if (header == NULL || bits == NULL || written == NULL) {
        return CRDV_E_ARGUMENT;
    }
    *written = 0u;
    if (bit_sync_count < 64u || (bit_sync_count & 1u) != 0u ||
        bit_sync_count > SIZE_MAX - 15u - CRDV_PROTECTED_BITS) {
        return CRDV_E_RANGE;
    }
    needed = bit_sync_count + 15u + CRDV_PROTECTED_BITS;
    if (capacity < needed) {
        return CRDV_E_CAPACITY;
    }
    if (crdv_header_protect(header, protected_bytes) != CRDV_OK) {
        return CRDV_E_FORMAT;
    }
    for (size_t index = 0; index < bit_sync_count; ++index) {
        bits[index] = (uint8_t)((index & 1u) == 0u ? 1u : 0u);
    }
    for (size_t index = 0; index < 15u; ++index) {
        bits[bit_sync_count + index] = (uint8_t)(frame_sync[index] == '1');
    }
    for (size_t index = 0; index < CRDV_PROTECTED_BITS; ++index) {
        bits[bit_sync_count + 15u + index] = bit_get(protected_bytes, index);
    }
    *written = needed;
    return CRDV_OK;
}

crdv_result crdv_header_repeat_block(const uint8_t header[CRDV_HEADER_BYTES],
                                     unsigned block_index, uint8_t first[3],
                                     uint8_t second[3])
{
    uint8_t block[6] = {0};
    if (header == NULL || first == NULL || second == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (block_index > 8u) {
        return CRDV_E_RANGE;
    }
    if (block_index < 8u) {
        block[0] = 0x55u;
        memcpy(block + 1u, header + (size_t)block_index * 5u, 5u);
    } else {
        block[0] = 0x51u;
        block[1] = header[40];
    }
    memcpy(first, block, 3u);
    memcpy(second, block + 3u, 3u);
    crdv_slow_xor(first);
    crdv_slow_xor(second);
    return CRDV_OK;
}

void crdv_header_repeat_reset(crdv_header_repeat_assembler *assembler,
                              uint32_t session)
{
    if (assembler != NULL) {
        memset(assembler, 0, sizeof(*assembler));
        assembler->session = session;
    }
}

crdv_result crdv_header_repeat_push(crdv_header_repeat_assembler *assembler,
                                    uint32_t session,
                                    const uint8_t six_bytes[6],
                                    crdv_header_fields *header,
                                    bool *new_header)
{
    size_t payload_length;
    if (assembler == NULL || six_bytes == NULL || header == NULL ||
        new_header == NULL) {
        return CRDV_E_ARGUMENT;
    }
    *new_header = false;
    if (assembler->session != session) {
        crdv_header_repeat_reset(assembler, session);
    }
    if (assembler->complete || assembler->rejected || six_bytes[0] < 0x51u ||
        six_bytes[0] > 0x55u) {
        return assembler->complete ? CRDV_OK
                                   : (assembler->rejected ? CRDV_E_STATE
                                                          : CRDV_E_FORMAT);
    }
    payload_length = (size_t)(six_bytes[0] & 0x0fu);
    if (payload_length > CRDV_HEADER_BYTES - assembler->used) {
        assembler->used = 0u;
        assembler->rejected = true;
        return CRDV_E_RANGE;
    }
    memcpy(assembler->bytes + assembler->used, six_bytes + 1u, payload_length);
    assembler->used += payload_length;
    if (assembler->used == CRDV_HEADER_BYTES) {
        crdv_result result = crdv_header_unpack(assembler->bytes, header);
        if (result != CRDV_OK) {
            assembler->used = 0u;
            assembler->rejected = true;
            return result;
        }
        assembler->complete = true;
        *new_header = true;
    }
    return CRDV_OK;
}

void crdv_last_frame_bits(uint8_t bits[48])
{
    static const char ending[] = "000100110101111";
    if (bits == NULL) {
        return;
    }
    for (size_t index = 0; index < 32u; ++index) {
        bits[index] = (uint8_t)((index % 2u) == 0u ? 1u : 0u);
    }
    for (size_t index = 0; index < 15u; ++index) {
        bits[32u + index] = (uint8_t)(ending[index] == '1' ? 1u : 0u);
    }
    bits[47] = 0u;
}

static bool last_frame_equal(const uint8_t *bits)
{
    uint8_t expected[48];
    crdv_last_frame_bits(expected);
    return memcmp(bits, expected, sizeof(expected)) == 0;
}

static bool frame_sync_equal(const uint8_t *bits)
{
    static const char sync[] = "111011001010000";
    uint8_t preamble_errors = 0u;
    for (size_t index = 0; index < 16u; ++index) {
        const uint8_t expected = (uint8_t)((index & 1u) == 0u ? 1u : 0u);
        if (bits[index] != expected) {
            preamble_errors++;
        }
    }
    if (preamble_errors > 2u) {
        return false;
    }
    for (size_t index = 0; index < 15u; ++index) {
        if (bits[16u + index] != (uint8_t)(sync[index] == '1' ? 1u : 0u)) {
            return false;
        }
    }
    return true;
}

static uint8_t data_sync_hamming(const uint8_t *bits)
{
    static const char sync[] = "101010101011010001101000";
    uint8_t distance = 0u;
    for (size_t index = 0; index < 24u; ++index) {
        if (bits[index] != (uint8_t)(sync[index] == '1' ? 1u : 0u)) {
            distance++;
        }
    }
    return distance;
}

void crdv_air_receiver_init(crdv_air_receiver *receiver,
                            const crdv_air_callbacks *callbacks)
{
    if (receiver != NULL) {
        memset(receiver, 0, sizeof(*receiver));
        receiver->best_sync_distance = CRDV_DATA_SYNC_BITS + 1u;
        if (callbacks != NULL) {
            receiver->callbacks = *callbacks;
        }
    }
}

crdv_result crdv_air_receiver_set_sync_policy(
    crdv_air_receiver *receiver, const crdv_receive_sync_policy *policy)
{
    if (receiver == NULL || policy == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (receiver->phase != CRDV_RX_SEARCH) {
        return CRDV_E_STATE;
    }
    if (policy->max_hamming_distance > CRDV_DATA_SYNC_BITS ||
        (!policy->sliding_reacquisition && policy->max_realign_bits != 0u) ||
        (policy->sliding_reacquisition &&
         (policy->max_realign_bits == 0u ||
          policy->max_realign_bits > CRDV_MAX_SYNC_REALIGN_BITS))) {
        return CRDV_E_RANGE;
    }
    receiver->sync_policy = *policy;
    receiver->best_sync_distance = CRDV_DATA_SYNC_BITS + 1u;
    return CRDV_OK;
}

crdv_result crdv_air_receiver_get_sync_counters(
    const crdv_air_receiver *receiver, crdv_sync_counters *counters)
{
    if (receiver == NULL || counters == NULL) {
        return CRDV_E_ARGUMENT;
    }
    *counters = receiver->sync_counters;
    return CRDV_OK;
}

void crdv_air_receiver_cancel(crdv_air_receiver *receiver)
{
    if (receiver != NULL) {
        crdv_air_callbacks callbacks = receiver->callbacks;
        crdv_receive_sync_policy policy = receiver->sync_policy;
        crdv_sync_counters counters = receiver->sync_counters;
        memset(receiver, 0, sizeof(*receiver));
        receiver->callbacks = callbacks;
        receiver->sync_policy = policy;
        receiver->sync_counters = counters;
        receiver->best_sync_distance = CRDV_DATA_SYNC_BITS + 1u;
    }
}

static void air_emit_event(crdv_air_receiver *receiver,
                           crdv_sync_event_kind kind, int offset,
                           uint8_t hamming_distance)
{
    crdv_sync_event event;
    event.kind = kind;
    event.frame_position = receiver->frame_position;
    event.bit_offset = (int8_t)offset;
    event.hamming_distance = hamming_distance;
    event.consecutive_misses = receiver->consecutive_sync_misses;
    if (receiver->callbacks.sync_event != NULL) {
        receiver->callbacks.sync_event(receiver->callbacks.context, &event);
    }
}

static void air_register_sync(crdv_air_receiver *receiver, uint8_t distance,
                              int offset)
{
    crdv_sync_event_kind kind;
    receiver->consecutive_sync_misses = 0u;
    if (offset < 0) {
        receiver->sync_counters.early_reacquisitions++;
        kind = CRDV_SYNC_REACQUIRED_EARLY;
    } else if (offset > 0) {
        receiver->sync_counters.late_reacquisitions++;
        kind = CRDV_SYNC_REACQUIRED_LATE;
    } else if (distance == 0u) {
        receiver->sync_counters.exact_syncs++;
        kind = CRDV_SYNC_EXACT;
    } else {
        receiver->sync_counters.tolerant_syncs++;
        kind = CRDV_SYNC_TOLERANT;
    }
    air_emit_event(receiver, kind, offset, distance);
}

static bool air_register_miss(crdv_air_receiver *receiver, uint8_t distance)
{
    receiver->sync_counters.expected_sync_misses++;
    if (receiver->consecutive_sync_misses < UINT8_MAX) {
        receiver->consecutive_sync_misses++;
    }
    air_emit_event(receiver, CRDV_SYNC_EXPECTED_MISS, 0, distance);
    if (receiver->sync_policy.consecutive_miss_limit != 0u &&
        receiver->consecutive_sync_misses >=
            receiver->sync_policy.consecutive_miss_limit) {
        receiver->sync_counters.lock_losses++;
        air_emit_event(receiver, CRDV_SYNC_LOCK_LOST, 0, distance);
        if (!receiver->ended && receiver->callbacks.end != NULL) {
            receiver->callbacks.end(receiver->callbacks.context);
        }
        receiver->ended = true;
        receiver->phase = CRDV_RX_SEARCH;
        receiver->window_fill = 0u;
        receiver->frame_fill = 0u;
        return true;
    }
    return false;
}

static void air_emit_voice(crdv_air_receiver *receiver, bool sync)
{
    uint8_t ambe[9] = {0};
    uint8_t slow[3] = {0};
    for (size_t index = 0; index < 72u; ++index) {
        bit_set(ambe, index, receiver->frame_bits[index]);
    }
    if (sync) {
        slow[0] = 0xaau;
        slow[1] = 0xb4u;
        slow[2] = 0x68u;
    } else {
        for (size_t index = 0; index < 24u; ++index) {
            bit_set(slow, index, receiver->frame_bits[72u + index]);
        }
    }
    if (receiver->callbacks.voice != NULL) {
        receiver->callbacks.voice(receiver->callbacks.context, ambe, slow, sync);
    }
    receiver->frame_position =
        sync ? 1u : (uint8_t)((receiver->frame_position + 1u) % 21u);
}

static void air_process_fixed_frame(crdv_air_receiver *receiver)
{
    uint8_t distance = data_sync_hamming(receiver->frame_bits + 72u);
    bool sync = distance <= receiver->sync_policy.max_hamming_distance;
    bool expected = receiver->frame_position == 0u;
    if (sync) {
        air_register_sync(receiver, distance, 0);
        air_emit_voice(receiver, true);
        return;
    }
    if (receiver->sync_policy.max_hamming_distance < CRDV_DATA_SYNC_BITS &&
        distance == receiver->sync_policy.max_hamming_distance + 1u) {
        receiver->sync_counters.rejected_candidates++;
    }
    if (expected && air_register_miss(receiver, distance)) {
        return;
    }
    air_emit_voice(receiver, false);
}

static void air_accept_reacquisition(crdv_air_receiver *receiver,
                                     uint8_t distance, int offset)
{
    air_register_sync(receiver, distance, offset);
    if (offset == 0) {
        air_emit_voice(receiver, true);
    } else {
        receiver->sync_counters.reacquired_frame_drops++;
        receiver->frame_position = 1u;
    }
    receiver->frame_fill = 0u;
}

static void air_finish_sliding_miss(crdv_air_receiver *receiver)
{
    size_t trailing = receiver->frame_fill - CRDV_VOICE_FRAME_BITS;
    if (air_register_miss(receiver, receiver->best_sync_distance)) {
        return;
    }
    air_emit_voice(receiver, false);
    memmove(receiver->frame_bits,
            receiver->frame_bits + CRDV_VOICE_FRAME_BITS, trailing);
    receiver->frame_fill = trailing;
    receiver->best_sync_distance = CRDV_DATA_SYNC_BITS + 1u;
}

static void air_accept_voice_bit(crdv_air_receiver *receiver, uint8_t bit)
{
    bool sliding_expected = receiver->sync_policy.sliding_reacquisition &&
                            receiver->frame_position == 0u;
    receiver->frame_bits[receiver->frame_fill++] = bit;
    if (receiver->frame_fill == 1u) {
        receiver->best_sync_distance = CRDV_DATA_SYNC_BITS + 1u;
    }
    if (receiver->frame_fill == 48u &&
        last_frame_equal(receiver->frame_bits)) {
        if (!receiver->ended && receiver->callbacks.end != NULL) {
            receiver->callbacks.end(receiver->callbacks.context);
        }
        receiver->ended = true;
        receiver->phase = CRDV_RX_SEARCH;
        receiver->window_fill = 0u;
        receiver->frame_fill = 0u;
        return;
    }
    if (sliding_expected) {
        size_t lower = CRDV_VOICE_FRAME_BITS -
                       (size_t)receiver->sync_policy.max_realign_bits;
        size_t upper = CRDV_VOICE_FRAME_BITS +
                       (size_t)receiver->sync_policy.max_realign_bits;
        if (receiver->frame_fill >= lower && receiver->frame_fill <= upper) {
            uint8_t distance = data_sync_hamming(
                receiver->frame_bits + receiver->frame_fill -
                CRDV_DATA_SYNC_BITS);
            int offset = (int)receiver->frame_fill -
                         (int)CRDV_VOICE_FRAME_BITS;
            if (distance < receiver->best_sync_distance) {
                receiver->best_sync_distance = distance;
            }
            if (distance <= receiver->sync_policy.max_hamming_distance) {
                air_accept_reacquisition(receiver, distance, offset);
                return;
            }
            if (receiver->sync_policy.max_hamming_distance <
                    CRDV_DATA_SYNC_BITS &&
                distance ==
                    receiver->sync_policy.max_hamming_distance + 1u) {
                receiver->sync_counters.rejected_candidates++;
            }
        }
        if (receiver->frame_fill == upper) {
            air_finish_sliding_miss(receiver);
        }
    } else if (receiver->frame_fill == CRDV_VOICE_FRAME_BITS) {
        air_process_fixed_frame(receiver);
        if (receiver->phase == CRDV_RX_VOICE) {
            receiver->frame_fill = 0u;
        }
    }
}

crdv_result crdv_air_receiver_push(crdv_air_receiver *receiver,
                                   const uint8_t *bits, size_t bit_count)
{
    if (receiver == NULL || (bits == NULL && bit_count != 0u)) {
        return CRDV_E_ARGUMENT;
    }
    for (size_t input = 0; input < bit_count; ++input) {
        uint8_t bit = (uint8_t)(bits[input] & 1u);
        if (receiver->phase == CRDV_RX_SEARCH) {
            if (receiver->window_fill < 31u) {
                receiver->window[receiver->window_fill++] = bit;
            } else {
                memmove(receiver->window, receiver->window + 1u, 30u);
                receiver->window[30] = bit;
            }
            if (receiver->window_fill == 31u && frame_sync_equal(receiver->window)) {
                receiver->phase = CRDV_RX_HEADER;
                receiver->protected_fill = 0u;
            }
        } else if (receiver->phase == CRDV_RX_HEADER) {
            receiver->protected_bits[receiver->protected_fill++] = bit;
            if (receiver->protected_fill == CRDV_PROTECTED_BITS) {
                uint8_t packed[CRDV_PROTECTED_PACKED_BYTES] = {0};
                uint8_t header[CRDV_HEADER_BYTES];
                crdv_header_fields fields;
                for (size_t index = 0; index < CRDV_PROTECTED_BITS; ++index) {
                    bit_set(packed, index, receiver->protected_bits[index]);
                }
                if (crdv_header_recover(packed, header, NULL) == CRDV_OK &&
                    crdv_header_unpack(header, &fields) == CRDV_OK) {
                    receiver->phase = CRDV_RX_VOICE;
                    receiver->frame_fill = 0u;
                    receiver->frame_position = 0u;
                    receiver->consecutive_sync_misses = 0u;
                    receiver->best_sync_distance = CRDV_DATA_SYNC_BITS + 1u;
                    receiver->ended = false;
                    if (receiver->callbacks.header != NULL) {
                        receiver->callbacks.header(receiver->callbacks.context,
                                                   &fields);
                    }
                } else {
                    receiver->phase = CRDV_RX_SEARCH;
                    receiver->window_fill = 0u;
                }
            }
        } else {
            air_accept_voice_bit(receiver, bit);
        }
    }
    return CRDV_OK;
}
