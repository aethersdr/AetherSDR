/*
 * MultiFlex ownership guard for the slice controlled by this helper.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "digital_voice_slice_ownership.h"

#include <string.h>

void digital_voice_slice_ownership_set(
    digital_voice_slice_ownership* ownership,
    uint32 slice,
    BOOL expected_owner_valid,
    uint32 expected_owner)
{
    if (ownership == NULL) {
        return;
    }
    memset(ownership, 0, sizeof(*ownership));
    ownership->controlled = TRUE;
    ownership->slice = slice;
    ownership->expected_owner_valid = expected_owner_valid
        && expected_owner != 0U;
    ownership->expected_owner = expected_owner;
}

void digital_voice_slice_ownership_clear(
    digital_voice_slice_ownership* ownership)
{
    if (ownership != NULL) {
        memset(ownership, 0, sizeof(*ownership));
    }
}

BOOL digital_voice_slice_ownership_accepts(
    digital_voice_slice_ownership* ownership,
    uint32 slice,
    BOOL owner_present,
    uint32 owner,
    BOOL removed)
{
    if (ownership == NULL || !ownership->controlled
        || ownership->slice != slice) {
        return FALSE;
    }

    const BOOL usable_owner = owner_present && owner != 0U;
    if (usable_owner) {
        if (ownership->expected_owner_valid
            && ownership->expected_owner != owner) {
            return FALSE;
        }
        if (ownership->observed_owner_valid
            && ownership->observed_owner != owner) {
            return FALSE;
        }
        ownership->observed_owner_valid = TRUE;
        ownership->observed_owner = owner;
    }

    if (removed) {
        digital_voice_slice_ownership_clear(ownership);
    }
    return TRUE;
}
