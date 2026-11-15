/*
 * Bounded D-STAR transmit bitstream assembly.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DSTAR_TX_STREAM_H_
#define DSTAR_TX_STREAM_H_

#include "circular_buffer.h"
#include "datatypes.h"
#include "aether_dstar_protocol.h"
#include "gmsk_modem.h"

#define DSTAR_TX_STREAM_AMBE_QUEUE_CAPACITY 32U

typedef enum dstar_tx_stream_phase
{
    DSTAR_TX_STREAM_IDLE = 0,
    DSTAR_TX_STREAM_HEADER,
    DSTAR_TX_STREAM_VOICE,
    DSTAR_TX_STREAM_ENDING
} dstar_tx_stream_phase;

typedef enum dstar_tx_stream_end_result
{
    DSTAR_TX_STREAM_END_NONE = 0,
    DSTAR_TX_STREAM_END_DRAIN_PENDING,
    DSTAR_TX_STREAM_END_QUEUED
} dstar_tx_stream_end_result;

typedef struct dstar_tx_stream_ambe_frame
{
    unsigned char bytes[CRDV_AMBE_BYTES];
    uint64 sequence;
    BOOL valid;
} dstar_tx_stream_ambe_frame;

typedef struct dstar_tx_stream
{
    dstar_tx_stream_phase phase;
    uint32 frame_count;
    dstar_tx_stream_ambe_frame ambe_queue[
        DSTAR_TX_STREAM_AMBE_QUEUE_CAPACITY];
    uint32 ambe_queue_head;
    uint32 ambe_queue_count;
    uint64 next_sequence;
    uint32 header_total_samples;
    uint32 header_preamble_samples;
    uint32 pre_roll_frames;
    uint32 null_frame_count;
    uint32 underflow_count;
    uint32 overflow_count;
    uint32 sequence_error_count;
    uint32 discarded_frame_count;
    uint32 queue_max_frames;
    uint32 queue_max_samples;
    aether_dstar_tx_snapshot snapshot;
} dstar_tx_stream;

#define DSTAR_TX_STREAM_INITIALIZER {0}

void dstar_tx_stream_reset(dstar_tx_stream * stream,
                           Circular_Float_Buffer samples);
BOOL dstar_tx_stream_begin(dstar_tx_stream * stream,
                           const aether_dstar_tx_snapshot * snapshot,
                           GMSK_MOD modulator,
                           Circular_Float_Buffer samples);
void dstar_tx_stream_offerVoice(dstar_tx_stream * stream,
                                GMSK_MOD modulator,
                                Circular_Float_Buffer samples,
                                uint64 sequence,
                                const unsigned char * ambe,
                                BOOL valid);
dstar_tx_stream_end_result dstar_tx_stream_requestEnd(
    dstar_tx_stream * stream,
    GMSK_MOD modulator,
    Circular_Float_Buffer samples);
uint32 dstar_tx_stream_readPacket(dstar_tx_stream * stream,
                                  GMSK_MOD modulator,
                                  Circular_Float_Buffer samples,
                                  float * output,
                                  uint32 output_samples);
uint32 dstar_tx_stream_pendingSamples(const dstar_tx_stream * stream,
                                     Circular_Float_Buffer samples);
uint32 dstar_tx_stream_emittedHeaderSamples(
    const dstar_tx_stream * stream,
    Circular_Float_Buffer samples);
BOOL dstar_tx_stream_preambleComplete(
    const dstar_tx_stream * stream,
    Circular_Float_Buffer samples);
BOOL dstar_tx_stream_abortHeader(dstar_tx_stream * stream,
                                 Circular_Float_Buffer samples);
uint32 dstar_tx_stream_pendingVoiceFrames(const dstar_tx_stream * stream);
uint32 dstar_tx_stream_discardPendingVoice(dstar_tx_stream * stream);
BOOL dstar_tx_stream_finished(const dstar_tx_stream * stream,
                              Circular_Float_Buffer samples);

#endif /* DSTAR_TX_STREAM_H_ */
