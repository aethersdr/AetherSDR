/*
 * Digital-voice transmit eligibility and interlock state reducer.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DIGITAL_VOICE_TX_GATE_H_
#define DIGITAL_VOICE_TX_GATE_H_

#include "datatypes.h"

typedef enum digital_voice_tx_source
{
    DIGITAL_VOICE_TX_SOURCE_UNKNOWN = 0,
    DIGITAL_VOICE_TX_SOURCE_SOFTWARE,
    DIGITAL_VOICE_TX_SOURCE_HARDWARE,
    DIGITAL_VOICE_TX_SOURCE_TUNE
} digital_voice_tx_source;

typedef enum digital_voice_tx_event
{
    DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED = 0,
    DIGITAL_VOICE_TX_EVENT_TRANSMITTING,
    DIGITAL_VOICE_TX_EVENT_UNKEY_REQUESTED,
    DIGITAL_VOICE_TX_EVENT_READY,
    DIGITAL_VOICE_TX_EVENT_RECEIVE,
    DIGITAL_VOICE_TX_EVENT_NOT_READY,
    DIGITAL_VOICE_TX_EVENT_TX_FAULT,
    DIGITAL_VOICE_TX_EVENT_TIMEOUT,
    DIGITAL_VOICE_TX_EVENT_STUCK_INPUT
} digital_voice_tx_event;

typedef enum digital_voice_tx_gate_action
{
    DIGITAL_VOICE_TX_GATE_NONE = 0,
    DIGITAL_VOICE_TX_GATE_BEGIN,
    DIGITAL_VOICE_TX_GATE_REQUEST_END,
    DIGITAL_VOICE_TX_GATE_CANCEL
} digital_voice_tx_gate_action;

typedef enum digital_voice_tx_gate_phase
{
    DIGITAL_VOICE_TX_GATE_IDLE = 0,
    DIGITAL_VOICE_TX_GATE_ACTIVE,
    DIGITAL_VOICE_TX_GATE_ENDING
} digital_voice_tx_gate_phase;

typedef struct digital_voice_tx_gate
{
    BOOL mode_slice_valid;
    uint32 mode_slice;
    BOOL tx_slice_valid;
    uint32 tx_slice;
    BOOL authoritative_selection;
    BOOL expected_owner_valid;
    uint32 expected_owner;
    BOOL tx_owner_valid;
    uint32 tx_owner;
    BOOL carrier_requested;
    digital_voice_tx_source source;
    digital_voice_tx_gate_phase phase;
} digital_voice_tx_gate;

#define DIGITAL_VOICE_TX_GATE_INITIALIZER \
    { FALSE, 0U, FALSE, 0U, FALSE, FALSE, 0U, FALSE, 0U, FALSE, \
      DIGITAL_VOICE_TX_SOURCE_UNKNOWN, DIGITAL_VOICE_TX_GATE_IDLE }

digital_voice_tx_gate_action digital_voice_tx_gate_setModeSlice(
    digital_voice_tx_gate * gate,
    BOOL valid,
    uint32 slice);
digital_voice_tx_gate_action digital_voice_tx_gate_setTxSlice(
    digital_voice_tx_gate * gate,
    BOOL valid,
    uint32 slice);
digital_voice_tx_gate_action digital_voice_tx_gate_setAuthoritativeSelection(
    digital_voice_tx_gate * gate,
    BOOL valid,
    uint32 slice,
    BOOL mode_active);
digital_voice_tx_gate_action digital_voice_tx_gate_setExpectedOwner(
    digital_voice_tx_gate * gate,
    BOOL valid,
    uint32 owner);
digital_voice_tx_gate_action digital_voice_tx_gate_setTxOwner(
    digital_voice_tx_gate * gate,
    BOOL valid,
    uint32 owner);
digital_voice_tx_gate_action digital_voice_tx_gate_setSliceStatus(
    digital_voice_tx_gate * gate,
    uint32 slice,
    BOOL in_use_present,
    BOOL in_use,
    BOOL mode_present,
    BOOL mode_active,
    BOOL tx_present,
    BOOL tx);
digital_voice_tx_gate_action digital_voice_tx_gate_observe(
    digital_voice_tx_gate * gate,
    digital_voice_tx_event event,
    BOOL source_present,
    digital_voice_tx_source source);
BOOL digital_voice_tx_gate_isEligible(const digital_voice_tx_gate * gate);

#endif /* DIGITAL_VOICE_TX_GATE_H_ */
