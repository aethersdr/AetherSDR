/*
 * ThumbDV vocoder backend for AetherSDR's local D-STAR waveform helper.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "ftd2xx.h"

#ifdef __cplusplus
extern "C" {
#endif

int aether_vocoder_set_kind_from_name(const char* name);
const char* aether_vocoder_name(void);
int aether_vocoder_requires_serial(void);

void aether_vocoder_init(FT_HANDLE* serial_handle);
void aether_vocoder_flush_lists(void);

unsigned int aether_vocoder_encode(FT_HANDLE handle,
                                   short* speech_in,
                                   unsigned char* packet_out,
                                   unsigned int num_samples);
void aether_vocoder_decode(FT_HANDLE handle,
                           unsigned char* packet_in,
                           unsigned int bytes_in_packet);
int aether_vocoder_get_decode_list_buffering(void);
int aether_vocoder_unlink_audio(short* speech_out);

#ifdef __cplusplus
}
#endif
