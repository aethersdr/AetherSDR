/*
 * AetherSDR integration state for the independently implemented crdv core.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AETHER_DSTAR_PROTOCOL_H_
#define AETHER_DSTAR_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crdv.h"

enum {
    AETHER_DSTAR_SAMPLES_PER_BIT = 5U,
    AETHER_DSTAR_SYMBOL_RATE = 4800U,
    AETHER_DSTAR_SAMPLE_RATE = 24000U,
    AETHER_DSTAR_MIN_PREAMBLE_BITS = 64U,
    AETHER_DSTAR_DEFAULT_PREAMBLE_BITS = 1200U,
    AETHER_DSTAR_MAX_PREAMBLE_BITS = 4800U
};

typedef struct aether_dstar_tx_snapshot {
    crdv_header_fields fields;
    uint8_t header[CRDV_HEADER_BYTES];
    uint8_t message[CRDV_MESSAGE_BYTES];
    size_t preamble_bits;
} aether_dstar_tx_snapshot;

typedef struct aether_dstar_rx_event {
    bool header;
    crdv_header_fields header_fields;
    bool voice;
    uint8_t ambe[CRDV_AMBE_BYTES];
    bool data_sync;
    bool sync_event;
    crdv_sync_event sync;
    bool message;
    char message_text[CRDV_MESSAGE_BYTES + 1U];
    bool end;
} aether_dstar_rx_event;

typedef struct aether_dstar_protocol {
    char mycall[9];
    char suffix[5];
    char urcall[9];
    char rpt1[9];
    char rpt2[9];
    char message[21];
    uint32_t slice;
    size_t preamble_bits;
    bool verbose;

    crdv_air_receiver receiver;
    crdv_message_assembler message_assembler;
    crdv_header_repeat_assembler header_assembler;
    uint32_t slow_session;
    uint8_t first_slow_fragment[CRDV_SLOW_FRAGMENT_BYTES];
    bool have_first_slow_fragment;
    aether_dstar_rx_event *active_event;
} aether_dstar_protocol;

void aether_dstar_protocol_init(aether_dstar_protocol *protocol);
void aether_dstar_protocol_reset_rx(aether_dstar_protocol *protocol);

bool aether_dstar_protocol_set_mycall(aether_dstar_protocol *protocol,
                                      const char *value);
bool aether_dstar_protocol_set_suffix(aether_dstar_protocol *protocol,
                                      const char *value);
bool aether_dstar_protocol_set_urcall(aether_dstar_protocol *protocol,
                                      const char *value);
bool aether_dstar_protocol_set_rpt1(aether_dstar_protocol *protocol,
                                    const char *value);
bool aether_dstar_protocol_set_rpt2(aether_dstar_protocol *protocol,
                                    const char *value);
bool aether_dstar_protocol_set_message(aether_dstar_protocol *protocol,
                                       const char *value);
crdv_result aether_dstar_protocol_configure(
    aether_dstar_protocol *protocol,
    const char *mycall,
    const char *suffix,
    const char *urcall,
    const char *rpt1,
    const char *rpt2,
    const char *message);

void aether_dstar_protocol_set_slice(aether_dstar_protocol *protocol,
                                     uint32_t slice);
void aether_dstar_protocol_set_preamble(aether_dstar_protocol *protocol,
                                        size_t bits);

crdv_result aether_dstar_protocol_tx_snapshot(
    const aether_dstar_protocol *protocol,
    aether_dstar_tx_snapshot *snapshot);
crdv_result aether_dstar_protocol_push_rx_bit(
    aether_dstar_protocol *protocol,
    uint8_t bit,
    aether_dstar_rx_event *event);
crdv_result aether_dstar_protocol_set_sync_policy(
    aether_dstar_protocol *protocol,
    const crdv_receive_sync_policy *policy);
crdv_result aether_dstar_protocol_sync_counters(
    const aether_dstar_protocol *protocol,
    crdv_sync_counters *counters);

#endif /* AETHER_DSTAR_PROTOCOL_H_ */
