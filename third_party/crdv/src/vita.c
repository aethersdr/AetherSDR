/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "crdv.h"

#include <math.h>
#include <string.h>

#define CRDV_VITA_HEADER_BYTES 28u
#define CRDV_VITA_PAYLOAD_BYTES (CRDV_VITA_AUDIO_PAIRS * 2u * 4u)
#define CRDV_VITA_PACKET_BYTES (CRDV_VITA_HEADER_BYTES + CRDV_VITA_PAYLOAD_BYTES)
#define CRDV_VITA_PACKET_WORDS (CRDV_VITA_PACKET_BYTES / 4u)

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] << 8u) | (uint16_t)bytes[1];
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

static uint64_t read_be64(const uint8_t *bytes)
{
    uint64_t high = read_be32(bytes);
    uint64_t low = read_be32(bytes + 4u);
    return (high << 32u) | low;
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)(value & 0xffu);
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)(value & 0xffu);
}

static void write_be64(uint8_t *bytes, uint64_t value)
{
    write_be32(bytes, (uint32_t)(value >> 32u));
    write_be32(bytes + 4u, (uint32_t)(value & 0xffffffffu));
}

crdv_result crdv_vita_parse_audio(const uint8_t *packet, size_t length,
                                  uint32_t expected_stream,
                                  crdv_vita_audio *out)
{
    crdv_vita_audio next;
    if (packet == NULL || out == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (length != CRDV_VITA_PACKET_BYTES || packet[0] != 0x18u ||
        (packet[1] & 0xf0u) != 0x60u ||
        read_be16(packet + 2u) != CRDV_VITA_PACKET_WORDS) {
        return CRDV_E_FORMAT;
    }
    if (read_be32(packet + 4u) != expected_stream) {
        return CRDV_E_STATE;
    }
    if (read_be32(packet + 8u) != 0x001c2du ||
        read_be16(packet + 12u) != 0x534cu ||
        read_be16(packet + 14u) != 0x03e3u) {
        return CRDV_E_FORMAT;
    }
    memset(&next, 0, sizeof(next));
    next.count = (uint8_t)(packet[1] & 0x0fu);
    next.stream_id = expected_stream;
    next.utc_seconds = read_be32(packet + 16u);
    next.fractional_picoseconds = read_be64(packet + 20u);
    for (size_t index = 0; index < CRDV_VITA_AUDIO_PAIRS * 2u; ++index) {
        uint32_t bits = read_be32(packet + CRDV_VITA_HEADER_BYTES + index * 4u);
        float value;
        memcpy(&value, &bits, sizeof(value));
        if (!isfinite(value)) {
            return CRDV_E_FORMAT;
        }
        next.pairs[index] = value;
    }
    *out = next;
    return CRDV_OK;
}

crdv_result crdv_vita_build_audio(const crdv_vita_audio *audio,
                                  uint8_t *packet, size_t capacity,
                                  size_t *written)
{
    if (audio == NULL || packet == NULL || written == NULL) {
        return CRDV_E_ARGUMENT;
    }
    *written = 0u;
    if (audio->count > 15u || capacity < CRDV_VITA_PACKET_BYTES) {
        return audio->count > 15u ? CRDV_E_RANGE : CRDV_E_CAPACITY;
    }
    memset(packet, 0, CRDV_VITA_PACKET_BYTES);
    packet[0] = 0x18u;
    packet[1] = (uint8_t)(0x60u | audio->count);
    write_be16(packet + 2u, (uint16_t)CRDV_VITA_PACKET_WORDS);
    write_be32(packet + 4u, audio->stream_id);
    write_be32(packet + 8u, 0x001c2du);
    write_be16(packet + 12u, 0x534cu);
    write_be16(packet + 14u, 0x03e3u);
    write_be32(packet + 16u, audio->utc_seconds);
    write_be64(packet + 20u, audio->fractional_picoseconds);
    for (size_t index = 0; index < CRDV_VITA_AUDIO_PAIRS * 2u; ++index) {
        uint32_t bits;
        if (!isfinite(audio->pairs[index])) {
            return CRDV_E_FORMAT;
        }
        memcpy(&bits, &audio->pairs[index], sizeof(bits));
        write_be32(packet + CRDV_VITA_HEADER_BYTES + index * 4u, bits);
    }
    *written = CRDV_VITA_PACKET_BYTES;
    return CRDV_OK;
}

void crdv_vita_counter_push(crdv_vita_counter *counter, uint8_t value)
{
    uint8_t distance;
    if (counter == NULL || value > 15u) {
        return;
    }
    if (!counter->seen) {
        counter->seen = true;
        counter->last = value;
        return;
    }
    distance = (uint8_t)((value - counter->last) & 0x0fu);
    if (distance == 0u) {
        counter->duplicates++;
    } else if (distance == 1u) {
        counter->last = value;
    } else if (distance <= 8u) {
        counter->gaps += (uint64_t)(distance - 1u);
        counter->last = value;
    } else {
        counter->reordered++;
    }
}
