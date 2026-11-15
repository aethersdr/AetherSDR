/*
 * AetherSDR integration state for the independently implemented crdv core.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "aether_dstar_protocol.h"

#include <string.h>

static bool copy_command_value(char *destination,
                               size_t capacity,
                               const char *source)
{
    char next[21];
    size_t length = 0U;

    if (destination == NULL || capacity == 0U || capacity > sizeof(next)) {
        return false;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return true;
    }

    while (source[length] != '\0') {
        const unsigned char value = (unsigned char)source[length];
        if (length + 1U >= capacity || value == '\r' || value == '\n'
            || value == '|' || (value < 0x20U && value != 0x7fU)
            || value > 0x7fU) {
            return false;
        }
        next[length] = value == 0x7fU ? ' ' : (char)value;
        length++;
    }
    while (length > 0U && next[length - 1U] == ' ') {
        length--;
    }
    next[length] = '\0';
    memcpy(destination, next, length + 1U);
    return true;
}

static bool padded_equals(const char *field,
                          size_t width,
                          const char *value)
{
    const size_t value_length = strlen(value);
    if (value_length > width || memcmp(field, value, value_length) != 0) {
        return false;
    }
    for (size_t index = value_length; index < width; index++) {
        if (field[index] != ' ') {
            return false;
        }
    }
    return true;
}

static void reset_slow_data(aether_dstar_protocol *protocol)
{
    protocol->slow_session++;
    if (protocol->slow_session == 0U) {
        protocol->slow_session = 1U;
    }
    crdv_message_assembler_reset(&protocol->message_assembler,
                                 protocol->slow_session);
    crdv_header_repeat_reset(&protocol->header_assembler,
                             protocol->slow_session);
    protocol->have_first_slow_fragment = false;
    memset(protocol->first_slow_fragment, 0,
           sizeof(protocol->first_slow_fragment));
}

static void receive_header(void *context,
                           const crdv_header_fields *header)
{
    aether_dstar_protocol *protocol = context;
    if (protocol == NULL || protocol->active_event == NULL || header == NULL) {
        return;
    }
    protocol->active_event->header = true;
    protocol->active_event->header_fields = *header;
    reset_slow_data(protocol);
}

static void process_slow_pair(aether_dstar_protocol *protocol,
                              const uint8_t bytes[6])
{
    uint8_t message[CRDV_MESSAGE_BYTES] = {0};
    bool new_message = false;
    crdv_header_fields header;
    bool new_header = false;

    if (crdv_message_assembler_push(&protocol->message_assembler,
                                    protocol->slow_session,
                                    bytes,
                                    message,
                                    &new_message) == CRDV_OK
        && new_message) {
        protocol->active_event->message = true;
        memcpy(protocol->active_event->message_text,
               message,
               sizeof(message));
        protocol->active_event->message_text[sizeof(message)] = '\0';
    }

    if (crdv_header_repeat_push(&protocol->header_assembler,
                                protocol->slow_session,
                                bytes,
                                &header,
                                &new_header) == CRDV_OK
        && new_header) {
        protocol->active_event->header = true;
        protocol->active_event->header_fields = header;
    }
}

static void receive_voice(void *context,
                          const uint8_t ambe[CRDV_AMBE_BYTES],
                          const uint8_t slow[CRDV_SLOW_FRAGMENT_BYTES],
                          bool data_sync)
{
    aether_dstar_protocol *protocol = context;
    uint8_t fragment[CRDV_SLOW_FRAGMENT_BYTES];

    if (protocol == NULL || protocol->active_event == NULL || ambe == NULL
        || slow == NULL) {
        return;
    }

    protocol->active_event->voice = true;
    protocol->active_event->data_sync = data_sync;
    memcpy(protocol->active_event->ambe, ambe, CRDV_AMBE_BYTES);

    if (data_sync) {
        reset_slow_data(protocol);
        return;
    }

    memcpy(fragment, slow, sizeof(fragment));
    crdv_slow_xor(fragment);
    if (!protocol->have_first_slow_fragment) {
        memcpy(protocol->first_slow_fragment, fragment, sizeof(fragment));
        protocol->have_first_slow_fragment = true;
        return;
    }

    uint8_t pair[6];
    memcpy(pair, protocol->first_slow_fragment, CRDV_SLOW_FRAGMENT_BYTES);
    memcpy(pair + CRDV_SLOW_FRAGMENT_BYTES, fragment,
           CRDV_SLOW_FRAGMENT_BYTES);
    protocol->have_first_slow_fragment = false;
    process_slow_pair(protocol, pair);
}

static void receive_end(void *context)
{
    aether_dstar_protocol *protocol = context;
    if (protocol != NULL && protocol->active_event != NULL) {
        protocol->active_event->end = true;
    }
}

static void receive_sync_event(void *context, const crdv_sync_event *event)
{
    aether_dstar_protocol *protocol = context;
    if (protocol == NULL || protocol->active_event == NULL || event == NULL) {
        return;
    }
    protocol->active_event->sync_event = true;
    protocol->active_event->sync = *event;
    if (event->kind == CRDV_SYNC_REACQUIRED_EARLY
        || event->kind == CRDV_SYNC_REACQUIRED_LATE) {
        reset_slow_data(protocol);
    }
}

void aether_dstar_protocol_init(aether_dstar_protocol *protocol)
{
    if (protocol == NULL) {
        return;
    }

    memset(protocol, 0, sizeof(*protocol));
    memcpy(protocol->urcall, "CQCQCQ", sizeof("CQCQCQ"));
    memcpy(protocol->rpt1, "DIRECT", sizeof("DIRECT"));
    memcpy(protocol->rpt2, "DIRECT", sizeof("DIRECT"));
    protocol->preamble_bits = AETHER_DSTAR_DEFAULT_PREAMBLE_BITS;

    crdv_air_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.header = receive_header;
    callbacks.voice = receive_voice;
    callbacks.end = receive_end;
    callbacks.context = protocol;
    callbacks.sync_event = receive_sync_event;
    crdv_air_receiver_init(&protocol->receiver, &callbacks);
    reset_slow_data(protocol);
}

void aether_dstar_protocol_reset_rx(aether_dstar_protocol *protocol)
{
    if (protocol == NULL) {
        return;
    }
    crdv_air_receiver_cancel(&protocol->receiver);
    protocol->active_event = NULL;
    reset_slow_data(protocol);
}

bool aether_dstar_protocol_set_mycall(aether_dstar_protocol *protocol,
                                      const char *value)
{
    return protocol != NULL
        && copy_command_value(protocol->mycall, sizeof(protocol->mycall), value);
}

bool aether_dstar_protocol_set_suffix(aether_dstar_protocol *protocol,
                                      const char *value)
{
    return protocol != NULL
        && copy_command_value(protocol->suffix, sizeof(protocol->suffix), value);
}

bool aether_dstar_protocol_set_urcall(aether_dstar_protocol *protocol,
                                      const char *value)
{
    return protocol != NULL
        && copy_command_value(protocol->urcall, sizeof(protocol->urcall), value);
}

bool aether_dstar_protocol_set_rpt1(aether_dstar_protocol *protocol,
                                    const char *value)
{
    return protocol != NULL
        && copy_command_value(protocol->rpt1, sizeof(protocol->rpt1), value);
}

bool aether_dstar_protocol_set_rpt2(aether_dstar_protocol *protocol,
                                    const char *value)
{
    return protocol != NULL
        && copy_command_value(protocol->rpt2, sizeof(protocol->rpt2), value);
}

bool aether_dstar_protocol_set_message(aether_dstar_protocol *protocol,
                                       const char *value)
{
    return protocol != NULL
        && copy_command_value(protocol->message, sizeof(protocol->message), value);
}

crdv_result aether_dstar_protocol_configure(
    aether_dstar_protocol *protocol,
    const char *mycall,
    const char *suffix,
    const char *urcall,
    const char *rpt1,
    const char *rpt2,
    const char *message)
{
    char next_mycall[9] = {0};
    char next_suffix[5] = {0};
    char next_urcall[9] = {0};
    char next_rpt1[9] = {0};
    char next_rpt2[9] = {0};
    char next_message[21] = {0};
    crdv_station_config normalized;

    if (protocol == NULL || mycall == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (!copy_command_value(next_mycall, sizeof(next_mycall), mycall)
        || !copy_command_value(next_suffix, sizeof(next_suffix),
                               suffix == NULL ? "" : suffix)
        || !copy_command_value(next_urcall, sizeof(next_urcall),
                               urcall == NULL ? "CQCQCQ" : urcall)
        || !copy_command_value(next_rpt1, sizeof(next_rpt1),
                               rpt1 == NULL ? "DIRECT" : rpt1)
        || !copy_command_value(next_rpt2, sizeof(next_rpt2),
                               rpt2 == NULL ? "DIRECT" : rpt2)
        || !copy_command_value(next_message, sizeof(next_message),
                               message == NULL ? "" : message)) {
        return CRDV_E_FORMAT;
    }

    const crdv_result result = crdv_config_normalize(
        next_mycall,
        next_suffix,
        next_urcall,
        next_rpt1,
        next_rpt2,
        next_message,
        &normalized);
    if (result != CRDV_OK) {
        return result;
    }

    memcpy(protocol->mycall, next_mycall, sizeof(next_mycall));
    memcpy(protocol->suffix, next_suffix, sizeof(next_suffix));
    memcpy(protocol->urcall, next_urcall, sizeof(next_urcall));
    memcpy(protocol->rpt1, next_rpt1, sizeof(next_rpt1));
    memcpy(protocol->rpt2, next_rpt2, sizeof(next_rpt2));
    memcpy(protocol->message, next_message, sizeof(next_message));
    return CRDV_OK;
}

void aether_dstar_protocol_set_slice(aether_dstar_protocol *protocol,
                                     uint32_t slice)
{
    if (protocol != NULL) {
        protocol->slice = slice;
    }
}

void aether_dstar_protocol_set_preamble(aether_dstar_protocol *protocol,
                                        size_t bits)
{
    if (protocol == NULL) {
        return;
    }
    if (bits < AETHER_DSTAR_MIN_PREAMBLE_BITS) {
        bits = AETHER_DSTAR_MIN_PREAMBLE_BITS;
    } else if (bits > AETHER_DSTAR_MAX_PREAMBLE_BITS) {
        bits = AETHER_DSTAR_MAX_PREAMBLE_BITS;
    }
    protocol->preamble_bits = bits & ~(size_t)1U;
}

crdv_result aether_dstar_protocol_tx_snapshot(
    const aether_dstar_protocol *protocol,
    aether_dstar_tx_snapshot *snapshot)
{
    crdv_station_config normalized;
    crdv_result result;

    if (protocol == NULL || snapshot == NULL) {
        return CRDV_E_ARGUMENT;
    }
    result = crdv_config_normalize(protocol->mycall,
                                   protocol->suffix,
                                   protocol->urcall,
                                   protocol->rpt1,
                                   protocol->rpt2,
                                   protocol->message,
                                   &normalized);
    if (result != CRDV_OK) {
        return result;
    }

    aether_dstar_tx_snapshot next;
    memset(&next, 0, sizeof(next));
    next.fields.flags[0] = padded_equals(normalized.rpt2, 8U, "DIRECT")
        ? 0U : 0x40U;
    memcpy(next.fields.rpt2, normalized.rpt2, sizeof(next.fields.rpt2));
    memcpy(next.fields.rpt1, normalized.rpt1, sizeof(next.fields.rpt1));
    memcpy(next.fields.urcall, normalized.urcall, sizeof(next.fields.urcall));
    memcpy(next.fields.mycall, normalized.mycall, sizeof(next.fields.mycall));
    memcpy(next.fields.suffix, normalized.suffix, sizeof(next.fields.suffix));
    memcpy(next.message, normalized.message, sizeof(next.message));
    next.preamble_bits = protocol->preamble_bits;

    result = crdv_header_pack(&next.fields, next.header);
    if (result != CRDV_OK) {
        return result;
    }
    *snapshot = next;
    return CRDV_OK;
}

crdv_result aether_dstar_protocol_push_rx_bit(
    aether_dstar_protocol *protocol,
    uint8_t bit,
    aether_dstar_rx_event *event)
{
    crdv_result result;

    if (protocol == NULL || event == NULL || bit > 1U
        || protocol->active_event != NULL) {
        return CRDV_E_ARGUMENT;
    }
    memset(event, 0, sizeof(*event));
    protocol->active_event = event;
    result = crdv_air_receiver_push(&protocol->receiver, &bit, 1U);
    protocol->active_event = NULL;
    return result;
}

crdv_result aether_dstar_protocol_set_sync_policy(
    aether_dstar_protocol *protocol,
    const crdv_receive_sync_policy *policy)
{
    if (protocol == NULL) {
        return CRDV_E_ARGUMENT;
    }
    return crdv_air_receiver_set_sync_policy(&protocol->receiver, policy);
}

crdv_result aether_dstar_protocol_sync_counters(
    const aether_dstar_protocol *protocol,
    crdv_sync_counters *counters)
{
    if (protocol == NULL) {
        return CRDV_E_ARGUMENT;
    }
    return crdv_air_receiver_get_sync_counters(&protocol->receiver, counters);
}
