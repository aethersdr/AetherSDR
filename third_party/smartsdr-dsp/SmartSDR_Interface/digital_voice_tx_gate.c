/*
 * Digital-voice transmit eligibility and interlock state reducer.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "digital_voice_tx_gate.h"

#include <stddef.h>

BOOL digital_voice_tx_gate_isEligible(const digital_voice_tx_gate * gate)
{
    if (gate == NULL
        || !gate->mode_slice_valid
        || !gate->tx_slice_valid
        || gate->mode_slice != gate->tx_slice
        || gate->source == DIGITAL_VOICE_TX_SOURCE_TUNE) {
        return FALSE;
    }
    if (gate->expected_owner_valid
        && gate->source != DIGITAL_VOICE_TX_SOURCE_HARDWARE) {
        return gate->source == DIGITAL_VOICE_TX_SOURCE_SOFTWARE
            && gate->tx_owner_valid
            && gate->tx_owner == gate->expected_owner;
    }
    return TRUE;
}

static digital_voice_tx_gate_action digital_voice_tx_gate_recheck(
    digital_voice_tx_gate * gate)
{
    if (gate == NULL) {
        return DIGITAL_VOICE_TX_GATE_NONE;
    }
    if (gate->phase != DIGITAL_VOICE_TX_GATE_IDLE
            && !digital_voice_tx_gate_isEligible(gate)) {
        gate->phase = DIGITAL_VOICE_TX_GATE_IDLE;
        return DIGITAL_VOICE_TX_GATE_CANCEL;
    }
    if (gate->phase == DIGITAL_VOICE_TX_GATE_IDLE
            && gate->carrier_requested
            /* Fail closed on an unclassified source: keying requires an
             * explicit SOFTWARE or HARDWARE source. isEligible() stays a
             * source-independent slice/mode predicate; this is the actual
             * key-down decision, so a caller that fires PTT/TRANSMITTING
             * without setting a source (source_present == FALSE, or after a
             * READY/RECEIVE/fault reset to UNKNOWN) can never begin TX. */
            && (gate->source == DIGITAL_VOICE_TX_SOURCE_SOFTWARE
                || gate->source == DIGITAL_VOICE_TX_SOURCE_HARDWARE)
            && digital_voice_tx_gate_isEligible(gate)) {
        gate->phase = DIGITAL_VOICE_TX_GATE_ACTIVE;
        return DIGITAL_VOICE_TX_GATE_BEGIN;
    }
    return DIGITAL_VOICE_TX_GATE_NONE;
}

digital_voice_tx_gate_action digital_voice_tx_gate_setModeSlice(
    digital_voice_tx_gate * gate,
    BOOL valid,
    uint32 slice)
{
    if (gate == NULL) {
        return DIGITAL_VOICE_TX_GATE_NONE;
    }
    gate->mode_slice_valid = valid;
    gate->mode_slice = slice;
    return digital_voice_tx_gate_recheck(gate);
}

digital_voice_tx_gate_action digital_voice_tx_gate_setTxSlice(
    digital_voice_tx_gate * gate,
    BOOL valid,
    uint32 slice)
{
    if (gate == NULL) {
        return DIGITAL_VOICE_TX_GATE_NONE;
    }
    gate->tx_slice_valid = valid;
    gate->tx_slice = slice;
    return digital_voice_tx_gate_recheck(gate);
}

digital_voice_tx_gate_action digital_voice_tx_gate_setAuthoritativeSelection(
    digital_voice_tx_gate * gate,
    BOOL valid,
    uint32 slice,
    BOOL mode_active)
{
    if (gate == NULL) {
        return DIGITAL_VOICE_TX_GATE_NONE;
    }

    const BOOL selection_changed = gate->tx_slice_valid != valid
        || (valid && gate->tx_slice != slice);
    gate->authoritative_selection = TRUE;
    gate->tx_slice_valid = valid;
    gate->tx_slice = slice;
    gate->mode_slice_valid = valid && mode_active;
    gate->mode_slice = slice;

    if (selection_changed && gate->phase != DIGITAL_VOICE_TX_GATE_IDLE) {
        gate->phase = DIGITAL_VOICE_TX_GATE_IDLE;
        return DIGITAL_VOICE_TX_GATE_CANCEL;
    }
    return digital_voice_tx_gate_recheck(gate);
}

digital_voice_tx_gate_action digital_voice_tx_gate_setExpectedOwner(
    digital_voice_tx_gate * gate,
    BOOL valid,
    uint32 owner)
{
    if (gate == NULL) {
        return DIGITAL_VOICE_TX_GATE_NONE;
    }
    gate->expected_owner_valid = valid && owner != 0U;
    gate->expected_owner = owner;
    return digital_voice_tx_gate_recheck(gate);
}

digital_voice_tx_gate_action digital_voice_tx_gate_setTxOwner(
    digital_voice_tx_gate * gate,
    BOOL valid,
    uint32 owner)
{
    if (gate == NULL) {
        return DIGITAL_VOICE_TX_GATE_NONE;
    }
    gate->tx_owner_valid = valid && owner != 0U;
    gate->tx_owner = owner;
    return digital_voice_tx_gate_recheck(gate);
}

digital_voice_tx_gate_action digital_voice_tx_gate_setSliceStatus(
    digital_voice_tx_gate * gate,
    uint32 slice,
    BOOL in_use_present,
    BOOL in_use,
    BOOL mode_present,
    BOOL mode_active,
    BOOL tx_present,
    BOOL tx)
{
    if (gate == NULL) {
        return DIGITAL_VOICE_TX_GATE_NONE;
    }

    const BOOL removed = in_use_present && !in_use;
    if (removed) {
        if (gate->mode_slice_valid && gate->mode_slice == slice) {
            gate->mode_slice_valid = FALSE;
        }
        if (gate->tx_slice_valid && gate->tx_slice == slice) {
            gate->tx_slice_valid = FALSE;
        }
        return digital_voice_tx_gate_recheck(gate);
    }

    if (gate->authoritative_selection) {
        if (gate->tx_slice_valid && gate->tx_slice == slice && mode_present) {
            gate->mode_slice_valid = mode_active;
            gate->mode_slice = slice;
        }
        return digital_voice_tx_gate_recheck(gate);
    }

    if (mode_present) {
        if (mode_active) {
            gate->mode_slice_valid = TRUE;
            gate->mode_slice = slice;
        } else if (gate->mode_slice_valid && gate->mode_slice == slice) {
            gate->mode_slice_valid = FALSE;
        }
    }
    if (tx_present) {
        if (tx) {
            gate->tx_slice_valid = TRUE;
            gate->tx_slice = slice;
        } else if (gate->tx_slice_valid && gate->tx_slice == slice) {
            gate->tx_slice_valid = FALSE;
        }
    }
    return digital_voice_tx_gate_recheck(gate);
}

digital_voice_tx_gate_action digital_voice_tx_gate_observe(
    digital_voice_tx_gate * gate,
    digital_voice_tx_event event,
    BOOL source_present,
    digital_voice_tx_source source)
{
    if (gate == NULL) {
        return DIGITAL_VOICE_TX_GATE_NONE;
    }
    if (source_present) {
        gate->source = source;
    }

    switch (event) {
    case DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED:
    case DIGITAL_VOICE_TX_EVENT_TRANSMITTING:
        gate->carrier_requested = TRUE;
        return digital_voice_tx_gate_recheck(gate);

    case DIGITAL_VOICE_TX_EVENT_UNKEY_REQUESTED:
        gate->carrier_requested = FALSE;
        if (gate->phase == DIGITAL_VOICE_TX_GATE_ACTIVE) {
            gate->phase = DIGITAL_VOICE_TX_GATE_ENDING;
            return DIGITAL_VOICE_TX_GATE_REQUEST_END;
        }
        return DIGITAL_VOICE_TX_GATE_NONE;

    case DIGITAL_VOICE_TX_EVENT_READY:
    case DIGITAL_VOICE_TX_EVENT_RECEIVE:
    case DIGITAL_VOICE_TX_EVENT_NOT_READY:
    case DIGITAL_VOICE_TX_EVENT_TX_FAULT:
    case DIGITAL_VOICE_TX_EVENT_TIMEOUT:
    case DIGITAL_VOICE_TX_EVENT_STUCK_INPUT:
        gate->carrier_requested = FALSE;
        gate->source = DIGITAL_VOICE_TX_SOURCE_UNKNOWN;
        gate->tx_owner_valid = FALSE;
        gate->tx_owner = 0U;
        if (gate->phase != DIGITAL_VOICE_TX_GATE_IDLE) {
            gate->phase = DIGITAL_VOICE_TX_GATE_IDLE;
            return DIGITAL_VOICE_TX_GATE_CANCEL;
        }
        return DIGITAL_VOICE_TX_GATE_NONE;
    }

    return DIGITAL_VOICE_TX_GATE_NONE;
}
