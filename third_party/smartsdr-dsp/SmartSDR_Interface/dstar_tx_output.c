/*
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dstar_tx_output.h"

#include <limits.h>
#include <stddef.h>

#if defined(_WIN32)
#include <float.h>
#else
#include <math.h>
#endif

#define DSTAR_TX_OUTPUT_LIMIT 0.98f

BOOL dstar_tx_output_isFinite(float value)
{
#if defined(_WIN32)
    return _finite((double)value) != 0;
#else
    return isfinite(value);
#endif
}

float dstar_tx_output_scaleSample(float sample, float gain)
{
    if (!dstar_tx_output_isFinite(sample) || !dstar_tx_output_isFinite(gain)) {
        return 0.0f;
    }

    const float scaled = sample * gain;
    if (!dstar_tx_output_isFinite(scaled)) {
        return 0.0f;
    }
    if (scaled > DSTAR_TX_OUTPUT_LIMIT) {
        return DSTAR_TX_OUTPUT_LIMIT;
    }
    if (scaled < -DSTAR_TX_OUTPUT_LIMIT) {
        return -DSTAR_TX_OUTPUT_LIMIT;
    }
    return scaled;
}

short dstar_tx_output_pcm16(float sample, BOOL * clipped, BOOL * invalid)
{
    if (clipped != NULL) {
        *clipped = FALSE;
    }
    if (invalid != NULL) {
        *invalid = FALSE;
    }
    if (!dstar_tx_output_isFinite(sample)) {
        if (invalid != NULL) {
            *invalid = TRUE;
        }
        return 0;
    }
    if (sample >= 1.0f) {
        if (clipped != NULL && sample > 1.0f) {
            *clipped = TRUE;
        }
        return SHRT_MAX;
    }
    if (sample <= -1.0f) {
        if (clipped != NULL && sample < -1.0f) {
            *clipped = TRUE;
        }
        return SHRT_MIN;
    }
    return (short)(sample * (float)SHRT_MAX);
}

uint32 dstar_tx_output_packetIntervalUsec(uint32 samples, uint32 sample_rate)
{
    if (sample_rate == 0U) {
        return 0U;
    }
    return (uint32)(((uint64)samples * 1000000U) / sample_rate);
}
