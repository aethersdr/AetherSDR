/*
 * MultiFlex ownership guard for the slice controlled by this helper.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DIGITAL_VOICE_SLICE_OWNERSHIP_H_
#define DIGITAL_VOICE_SLICE_OWNERSHIP_H_

#include "datatypes.h"

typedef struct digital_voice_slice_ownership
{
    BOOL controlled;
    uint32 slice;
    BOOL expected_owner_valid;
    uint32 expected_owner;
    BOOL observed_owner_valid;
    uint32 observed_owner;
} digital_voice_slice_ownership;

#define DIGITAL_VOICE_SLICE_OWNERSHIP_INITIALIZER \
    { FALSE, 0U, FALSE, 0U, FALSE, 0U }

void digital_voice_slice_ownership_set(
    digital_voice_slice_ownership* ownership,
    uint32 slice,
    BOOL expected_owner_valid,
    uint32 expected_owner);
void digital_voice_slice_ownership_clear(
    digital_voice_slice_ownership* ownership);
BOOL digital_voice_slice_ownership_accepts(
    digital_voice_slice_ownership* ownership,
    uint32 slice,
    BOOL owner_present,
    uint32 owner,
    BOOL removed);

#endif /* DIGITAL_VOICE_SLICE_OWNERSHIP_H_ */
