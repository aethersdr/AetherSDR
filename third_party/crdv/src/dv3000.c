/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "crdv.h"

#include <string.h>

static bool packet_type_valid(uint8_t value)
{
    return value <= (uint8_t)CRDV_DV_SPEECH;
}

static uint8_t parity_value(const uint8_t *packet, size_t through)
{
    uint8_t value = 0u;
    for (size_t index = 1u; index <= through; ++index) {
        value ^= packet[index];
    }
    return value;
}

void crdv_dv_parser_init(crdv_dv_parser *parser, bool parity_required,
                         crdv_dv_packet_callback callback, void *context)
{
    if (parser != NULL) {
        memset(parser, 0, sizeof(*parser));
        parser->parity_required = parity_required;
        parser->callback = callback;
        parser->context = context;
    }
}

void crdv_dv_parser_set_parity(crdv_dv_parser *parser, bool required)
{
    if (parser != NULL) {
        parser->parity_required = required;
        parser->used = 0u;
        parser->expected = 0u;
    }
}

static void parser_reject(crdv_dv_parser *parser)
{
    parser->rejected++;
    parser->used = 0u;
    parser->expected = 0u;
}

static void parser_finish(crdv_dv_parser *parser)
{
    size_t payload = parser->expected - 4u;
    bool has_parity = false;
    crdv_dv_packet_view view;

    if (parser->parity_required && payload >= 2u &&
        parser->bytes[parser->expected - 2u] == 0x2fu &&
        parity_value(parser->bytes, parser->expected - 2u) ==
            parser->bytes[parser->expected - 1u]) {
        has_parity = true;
    }
    if (parser->parity_required && !has_parity) {
        parser_reject(parser);
        return;
    }
    view.type = (crdv_dv_packet_type)parser->bytes[3];
    view.fields = parser->bytes + 4u;
    view.field_length = payload - (has_parity ? 2u : 0u);
    view.had_parity = has_parity;
    if (view.field_length == 0u) {
        parser_reject(parser);
        return;
    }
    if (parser->callback != NULL) {
        parser->callback(parser->context, &view);
    }
    parser->used = 0u;
    parser->expected = 0u;
}

crdv_result crdv_dv_parser_feed(crdv_dv_parser *parser, const uint8_t *bytes,
                                size_t length)
{
    if (parser == NULL || (bytes == NULL && length != 0u)) {
        return CRDV_E_ARGUMENT;
    }
    for (size_t input = 0; input < length; ++input) {
        uint8_t byte = bytes[input];
        if (parser->used == 0u) {
            if (byte != 0x61u) {
                parser->rejected++;
                continue;
            }
            parser->bytes[parser->used++] = byte;
            continue;
        }
        if (parser->used >= CRDV_DV3000_MAX_PACKET) {
            parser_reject(parser);
            if (byte == 0x61u) {
                parser->bytes[parser->used++] = byte;
            }
            continue;
        }
        parser->bytes[parser->used++] = byte;
        if (parser->used == 3u) {
            size_t field_length = ((size_t)parser->bytes[1] << 8u) |
                                  (size_t)parser->bytes[2];
            if (field_length == 0u || field_length > CRDV_DV3000_MAX_PACKET - 4u) {
                parser_reject(parser);
                continue;
            }
            parser->expected = field_length + 4u;
        } else if (parser->used == 4u && !packet_type_valid(parser->bytes[3])) {
            parser_reject(parser);
        }
        if (parser->expected != 0u && parser->used == parser->expected) {
            parser_finish(parser);
        }
    }
    return CRDV_OK;
}

crdv_result crdv_dv_build_packet(crdv_dv_packet_type type,
                                 const uint8_t *fields, size_t field_length,
                                 bool parity, uint8_t *out, size_t capacity,
                                 size_t *written)
{
    size_t payload;
    size_t total;
    if ((fields == NULL && field_length != 0u) || out == NULL || written == NULL) {
        return CRDV_E_ARGUMENT;
    }
    *written = 0u;
    if (!packet_type_valid((uint8_t)type) || field_length == 0u) {
        return CRDV_E_RANGE;
    }
    if (field_length > 0xffffu - (parity ? 2u : 0u)) {
        return CRDV_E_RANGE;
    }
    payload = field_length + (parity ? 2u : 0u);
    total = payload + 4u;
    if (total > CRDV_DV3000_MAX_PACKET || capacity < total) {
        return CRDV_E_CAPACITY;
    }
    out[0] = 0x61u;
    out[1] = (uint8_t)(payload >> 8u);
    out[2] = (uint8_t)(payload & 0xffu);
    out[3] = (uint8_t)type;
    memcpy(out + 4u, fields, field_length);
    if (parity) {
        out[4u + field_length] = 0x2fu;
        out[5u + field_length] = parity_value(out, 4u + field_length);
    }
    *written = total;
    return CRDV_OK;
}

crdv_result crdv_dv_build_encode(const int16_t pcm[CRDV_PCM_SAMPLES],
                                 bool parity, uint8_t *out, size_t capacity,
                                 size_t *written)
{
    uint8_t fields[323];
    if (pcm == NULL) {
        return CRDV_E_ARGUMENT;
    }
    fields[0] = 0x40u;
    fields[1] = 0x00u;
    fields[2] = 0xa0u;
    for (size_t index = 0; index < CRDV_PCM_SAMPLES; ++index) {
        uint16_t sample = (uint16_t)pcm[index];
        fields[3u + index * 2u] = (uint8_t)(sample >> 8u);
        fields[4u + index * 2u] = (uint8_t)(sample & 0xffu);
    }
    return crdv_dv_build_packet(CRDV_DV_SPEECH, fields, sizeof(fields), parity,
                                out, capacity, written);
}

crdv_result crdv_dv_build_decode(const uint8_t ambe[CRDV_AMBE_BYTES],
                                 bool parity, uint8_t *out, size_t capacity,
                                 size_t *written)
{
    uint8_t fields[11];
    if (ambe == NULL) {
        return CRDV_E_ARGUMENT;
    }
    fields[0] = 0x01u;
    fields[1] = 0x48u;
    memcpy(fields + 2u, ambe, CRDV_AMBE_BYTES);
    return crdv_dv_build_packet(CRDV_DV_CHANNEL, fields, sizeof(fields), parity,
                                out, capacity, written);
}

crdv_result crdv_dv_build_startup(crdv_dv_startup_step step, uint8_t *out,
                                  size_t capacity, size_t *written)
{
    static const uint8_t fields[][13] = {
        {0x33u},
        {0x30u},
        {0x31u},
        {0x3fu, 0x00u},
        {0x37u},
        {0x0bu, 0x07u},
        {0x4bu, 0x00u, 0x00u},
        {0x32u, 0x00u},
        {0x15u, 0x00u, 0x00u},
        {0x0au, 0x01u, 0x30u, 0x07u, 0x63u, 0x40u, 0x00u,
         0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x48u}};
    static const uint8_t lengths[] = {1u, 1u, 1u, 2u, 1u,
                                      2u, 3u, 2u, 3u, 13u};
    bool parity;
    if (step < CRDV_DV_START_RESET || step > CRDV_DV_START_DSTAR_RATE) {
        return CRDV_E_RANGE;
    }
    parity = step <= CRDV_DV_START_DISABLE_PARITY;
    return crdv_dv_build_packet(CRDV_DV_CONTROL, fields[(unsigned)step],
                                lengths[(unsigned)step], parity, out, capacity,
                                written);
}

crdv_result crdv_dv_product_is_ambe3000(const crdv_dv_packet_view *packet)
{
    size_t nul = 0u;
    bool found_ambe = false;
    bool found_3000 = false;
    if (packet == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (packet->type != CRDV_DV_CONTROL || packet->field_length < 3u ||
        packet->fields[0] != 0x30u) {
        return CRDV_E_FORMAT;
    }
    for (nul = 1u; nul < packet->field_length; ++nul) {
        uint8_t value = packet->fields[nul];
        if (value == 0u) {
            break;
        }
        if (value < 0x20u || value > 0x7eu) {
            return CRDV_E_FORMAT;
        }
    }
    if (nul == packet->field_length) {
        return CRDV_E_FORMAT;
    }
    for (size_t index = 1u; index + 3u < nul; ++index) {
        if (memcmp(packet->fields + index, "AMBE", 4u) == 0) {
            found_ambe = true;
        }
        if (memcmp(packet->fields + index, "3000", 4u) == 0) {
            found_3000 = true;
        }
    }
    return found_ambe && found_3000 ? CRDV_OK : CRDV_E_CHECK;
}

crdv_result crdv_dv_read_channel(const crdv_dv_packet_view *packet,
                                 uint8_t ambe[CRDV_AMBE_BYTES])
{
    if (packet == NULL || ambe == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (packet->type != CRDV_DV_CHANNEL || packet->field_length != 11u ||
        packet->fields[0] != 0x01u || packet->fields[1] != 0x48u) {
        return CRDV_E_FORMAT;
    }
    memcpy(ambe, packet->fields + 2u, CRDV_AMBE_BYTES);
    return CRDV_OK;
}

crdv_result crdv_dv_read_speech(const crdv_dv_packet_view *packet,
                                int16_t pcm[CRDV_PCM_SAMPLES])
{
    if (packet == NULL || pcm == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (packet->type != CRDV_DV_SPEECH || packet->field_length != 323u ||
        packet->fields[0] != 0x00u || packet->fields[1] != 0x00u ||
        packet->fields[2] != 0xa0u) {
        return CRDV_E_FORMAT;
    }
    for (size_t index = 0; index < CRDV_PCM_SAMPLES; ++index) {
        uint16_t sample = (uint16_t)((uint16_t)packet->fields[3u + index * 2u]
                                     << 8u) |
                          (uint16_t)packet->fields[4u + index * 2u];
        pcm[index] = (int16_t)sample;
    }
    return CRDV_OK;
}

void crdv_dv_transactions_init(crdv_dv_transactions *transactions)
{
    if (transactions != NULL) {
        memset(transactions, 0, sizeof(*transactions));
        transactions->generation = 1u;
    }
}

uint32_t crdv_dv_transactions_invalidate(crdv_dv_transactions *transactions)
{
    if (transactions == NULL) {
        return 0u;
    }
    transactions->discarded += transactions->count;
    memset(transactions->pending, 0, sizeof(transactions->pending));
    transactions->head = 0u;
    transactions->count = 0u;
    transactions->generation++;
    if (transactions->generation == 0u) {
        transactions->generation = 1u;
    }
    return transactions->generation;
}

crdv_result crdv_dv_transactions_submit(crdv_dv_transactions *transactions,
                                        crdv_dv_expectation expected,
                                        uint8_t field, uint64_t deadline_ms)
{
    size_t index;
    if (transactions == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (expected < CRDV_EXPECT_CHANNEL || expected > CRDV_EXPECT_CONTROL) {
        return CRDV_E_RANGE;
    }
    if (transactions->count == CRDV_DV_PENDING_CAPACITY) {
        return CRDV_E_CAPACITY;
    }
    index = (transactions->head + transactions->count) %
            CRDV_DV_PENDING_CAPACITY;
    transactions->pending[index].generation = transactions->generation;
    transactions->pending[index].deadline_ms = deadline_ms;
    transactions->pending[index].expected = expected;
    transactions->pending[index].field = field;
    transactions->pending[index].occupied = true;
    transactions->count++;
    return CRDV_OK;
}

static bool expectation_matches(crdv_dv_expectation expected,
                                crdv_dv_packet_type actual)
{
    return (expected == CRDV_EXPECT_CHANNEL && actual == CRDV_DV_CHANNEL) ||
           (expected == CRDV_EXPECT_SPEECH && actual == CRDV_DV_SPEECH) ||
           (expected == CRDV_EXPECT_CONTROL && actual == CRDV_DV_CONTROL);
}

static void pending_pop(crdv_dv_transactions *transactions)
{
    transactions->pending[transactions->head].occupied = false;
    transactions->head = (transactions->head + 1u) % CRDV_DV_PENDING_CAPACITY;
    transactions->count--;
}

crdv_result crdv_dv_transactions_accept(crdv_dv_transactions *transactions,
                                        const crdv_dv_packet_view *packet,
                                        uint64_t now_ms)
{
    crdv_dv_pending *pending;
    bool matches;
    if (transactions == NULL || packet == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (transactions->count == 0u) {
        transactions->discarded++;
        return CRDV_E_STATE;
    }
    pending = &transactions->pending[transactions->head];
    if (pending->generation != transactions->generation) {
        transactions->discarded++;
        pending_pop(transactions);
        return CRDV_E_STATE;
    }
    if (now_ms > pending->deadline_ms) {
        transactions->timed_out++;
        pending_pop(transactions);
        return CRDV_E_TIMEOUT;
    }
    matches = expectation_matches(pending->expected, packet->type) &&
              packet->field_length > 0u && packet->fields[0] == pending->field;
    pending_pop(transactions);
    if (!matches) {
        transactions->discarded++;
        return CRDV_E_FORMAT;
    }
    return CRDV_OK;
}

size_t crdv_dv_transactions_expire(crdv_dv_transactions *transactions,
                                   uint64_t now_ms)
{
    size_t expired = 0u;
    if (transactions == NULL) {
        return 0u;
    }
    while (transactions->count != 0u &&
           now_ms > transactions->pending[transactions->head].deadline_ms) {
        pending_pop(transactions);
        transactions->timed_out++;
        expired++;
    }
    return expired;
}
