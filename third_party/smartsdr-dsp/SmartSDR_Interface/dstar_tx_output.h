/*
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DSTAR_TX_OUTPUT_H_
#define DSTAR_TX_OUTPUT_H_

#include "datatypes.h"

float dstar_tx_output_scaleSample(float sample, float gain);
short dstar_tx_output_pcm16(float sample, BOOL * clipped, BOOL * invalid);
uint32 dstar_tx_output_packetIntervalUsec(uint32 samples, uint32 sample_rate);
BOOL dstar_tx_output_isFinite(float value);

#endif
