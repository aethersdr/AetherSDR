/*
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dstar_transmit_state.h"

BOOL dstar_transmit_state_begin(DStarTransmitState state)
{
    pthread_mutex_lock(&state->lock);
    const BOOL changed = state->phase == DSTAR_TRANSMIT_IDLE;
    if (changed) {
        state->phase = DSTAR_TRANSMIT_ACTIVE;
        state->receive_reset_requested = FALSE;
    }
    pthread_mutex_unlock(&state->lock);
    return changed;
}

BOOL dstar_transmit_state_requestEnd(DStarTransmitState state)
{
    pthread_mutex_lock(&state->lock);
    const BOOL changed = state->phase == DSTAR_TRANSMIT_ACTIVE;
    if (changed) {
        state->phase = DSTAR_TRANSMIT_DRAINING;
    }
    pthread_mutex_unlock(&state->lock);
    return changed;
}

BOOL dstar_transmit_state_markEndQueued(DStarTransmitState state)
{
    pthread_mutex_lock(&state->lock);
    const BOOL changed = state->phase == DSTAR_TRANSMIT_DRAINING;
    if (changed) {
        state->phase = DSTAR_TRANSMIT_ENDING;
    }
    pthread_mutex_unlock(&state->lock);
    return changed;
}

BOOL dstar_transmit_state_cancel(DStarTransmitState state)
{
    pthread_mutex_lock(&state->lock);
    const BOOL changed = state->phase != DSTAR_TRANSMIT_IDLE;
    if (changed) {
        state->phase = DSTAR_TRANSMIT_IDLE;
        state->receive_reset_requested = TRUE;
    }
    pthread_mutex_unlock(&state->lock);
    return changed;
}

void dstar_transmit_state_requestReceiveReset(DStarTransmitState state)
{
    pthread_mutex_lock(&state->lock);
    state->receive_reset_requested = TRUE;
    pthread_mutex_unlock(&state->lock);
}

BOOL dstar_transmit_state_endRequested(DStarTransmitState state)
{
    pthread_mutex_lock(&state->lock);
    const BOOL requested = state->phase == DSTAR_TRANSMIT_DRAINING
        || state->phase == DSTAR_TRANSMIT_ENDING;
    pthread_mutex_unlock(&state->lock);
    return requested;
}

BOOL dstar_transmit_state_transmitOrTailEnabled(DStarTransmitState state)
{
    pthread_mutex_lock(&state->lock);
    const BOOL enabled = state->phase != DSTAR_TRANSMIT_IDLE;
    pthread_mutex_unlock(&state->lock);
    return enabled;
}

enum dstar_transmit_phase dstar_transmit_state_phase(DStarTransmitState state)
{
    pthread_mutex_lock(&state->lock);
    const enum dstar_transmit_phase phase = state->phase;
    pthread_mutex_unlock(&state->lock);
    return phase;
}

BOOL dstar_transmit_state_consumeReceiveReset(DStarTransmitState state)
{
    pthread_mutex_lock(&state->lock);
    const BOOL requested = state->receive_reset_requested;
    state->receive_reset_requested = FALSE;
    pthread_mutex_unlock(&state->lock);
    return requested;
}

void dstar_transmit_state_finishTail(DStarTransmitState state)
{
    pthread_mutex_lock(&state->lock);
    if (state->phase == DSTAR_TRANSMIT_ENDING) {
        state->phase = DSTAR_TRANSMIT_IDLE;
        state->receive_reset_requested = TRUE;
    }
    pthread_mutex_unlock(&state->lock);
}

void dstar_transmit_state_reset(DStarTransmitState state)
{
    pthread_mutex_lock(&state->lock);
    state->phase = DSTAR_TRANSMIT_IDLE;
    state->receive_reset_requested = FALSE;
    pthread_mutex_unlock(&state->lock);
}
