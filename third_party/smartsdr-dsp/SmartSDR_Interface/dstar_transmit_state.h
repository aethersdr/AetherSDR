/*
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DSTAR_TRANSMIT_STATE_H_
#define DSTAR_TRANSMIT_STATE_H_

#include <pthread.h>

#include "datatypes.h"

typedef struct _dstar_transmit_state {
    pthread_mutex_t lock;
    enum dstar_transmit_phase {
        DSTAR_TRANSMIT_IDLE = 0,
        DSTAR_TRANSMIT_ACTIVE,
        DSTAR_TRANSMIT_DRAINING,
        DSTAR_TRANSMIT_ENDING
    } phase;
    BOOL receive_reset_requested;
} dstar_transmit_state;

#define DSTAR_TRANSMIT_STATE_INITIALIZER \
    { PTHREAD_MUTEX_INITIALIZER, DSTAR_TRANSMIT_IDLE, FALSE }

typedef dstar_transmit_state * DStarTransmitState;

BOOL dstar_transmit_state_begin(DStarTransmitState state);
BOOL dstar_transmit_state_requestEnd(DStarTransmitState state);
BOOL dstar_transmit_state_markEndQueued(DStarTransmitState state);
BOOL dstar_transmit_state_cancel(DStarTransmitState state);
void dstar_transmit_state_requestReceiveReset(DStarTransmitState state);
BOOL dstar_transmit_state_endRequested(DStarTransmitState state);
BOOL dstar_transmit_state_transmitOrTailEnabled(DStarTransmitState state);
enum dstar_transmit_phase dstar_transmit_state_phase(DStarTransmitState state);
BOOL dstar_transmit_state_consumeReceiveReset(DStarTransmitState state);
void dstar_transmit_state_finishTail(DStarTransmitState state);
void dstar_transmit_state_reset(DStarTransmitState state);

#endif
