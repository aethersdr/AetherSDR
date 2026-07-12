/*
 * Bounded D-STAR transmit bitstream assembly using the clean-room crdv core.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dstar_tx_stream.h"

#include <stddef.h>
#include <string.h>

/*
 * Compatibility silence frame independently exercised with ThumbDV hardware.
 * crdv intentionally does not make this C4 value a protocol default.
 */
static const uint8_t kNullAmbe[CRDV_AMBE_BYTES] = {
    0x9eU, 0x8dU, 0x32U, 0x88U, 0x26U, 0x1aU, 0x3fU, 0x61U, 0xe8U
};

static void dstar_tx_stream_recordQueue(dstar_tx_stream *stream,
                                        Circular_Float_Buffer samples)
{
    const uint32 queued = (uint32)cfbContains(samples);
    if (queued > stream->queue_max_samples) {
        stream->queue_max_samples = queued;
    }
}

static BOOL dstar_tx_stream_appendBits(dstar_tx_stream *stream,
                                       GMSK_MOD modulator,
                                       Circular_Float_Buffer samples,
                                       const uint8_t *bits,
                                       size_t bit_count)
{
    if (stream == NULL || modulator == NULL || samples == NULL
        || (bits == NULL && bit_count != 0U)
        || bit_count > SIZE_MAX / AETHER_DSTAR_SAMPLES_PER_BIT) {
        return FALSE;
    }

    const size_t needed = bit_count * AETHER_DSTAR_SAMPLES_PER_BIT;
    const size_t queued = (size_t)cfbContains(samples);
    const size_t capacity = samples->size > 0U ? samples->size - 1U : 0U;
    if (queued > capacity || needed > capacity - queued) {
        stream->overflow_count++;
        return FALSE;
    }

    for (size_t bit = 0U; bit < bit_count; bit++) {
        float encoded[AETHER_DSTAR_SAMPLES_PER_BIT];
        if (gmsk_encode(modulator,
                        bits[bit] != 0U,
                        encoded,
                        AETHER_DSTAR_SAMPLES_PER_BIT)
            != AETHER_DSTAR_SAMPLES_PER_BIT) {
            return FALSE;
        }
        for (size_t sample = 0U; sample < AETHER_DSTAR_SAMPLES_PER_BIT;
             sample++) {
            cbWriteFloat(samples, encoded[sample]);
        }
    }
    dstar_tx_stream_recordQueue(stream, samples);
    return TRUE;
}

static void dstar_tx_stream_slowData(const dstar_tx_stream *stream,
                                     uint8_t slow[CRDV_SLOW_FRAGMENT_BYTES],
                                     BOOL *data_sync)
{
    const uint32 position = stream->frame_count % 21U;
    memset(slow, 0, CRDV_SLOW_FRAGMENT_BYTES);
    *data_sync = position == 0U;
    if (*data_sync) {
        return;
    }

    const uint32 superframe = stream->frame_count / 21U;
    if ((superframe & 1U) == 0U && position <= 8U) {
        const unsigned block = (unsigned)((position - 1U) / 2U) + 1U;
        uint8_t first[CRDV_SLOW_FRAGMENT_BYTES];
        uint8_t second[CRDV_SLOW_FRAGMENT_BYTES];
        if (crdv_message_block(stream->snapshot.message,
                               block,
                               first,
                               second) == CRDV_OK) {
            memcpy(slow, (position & 1U) != 0U ? first : second,
                   CRDV_SLOW_FRAGMENT_BYTES);
            return;
        }
    } else if ((superframe & 1U) != 0U && position <= 18U) {
        const unsigned block = (unsigned)((position - 1U) / 2U);
        uint8_t first[CRDV_SLOW_FRAGMENT_BYTES];
        uint8_t second[CRDV_SLOW_FRAGMENT_BYTES];
        if (crdv_header_repeat_block(stream->snapshot.header,
                                     block,
                                     first,
                                     second) == CRDV_OK) {
            memcpy(slow, (position & 1U) != 0U ? first : second,
                   CRDV_SLOW_FRAGMENT_BYTES);
            return;
        }
    }

    /* An all-zero uninterpreted fragment, scrambled as required on air. */
    crdv_slow_xor(slow);
}

static BOOL dstar_tx_stream_appendVoice(dstar_tx_stream *stream,
                                        GMSK_MOD modulator,
                                        Circular_Float_Buffer samples,
                                        const unsigned char *ambe,
                                        BOOL valid)
{
    const unsigned char *frame = valid ? ambe : kNullAmbe;
    uint8_t slow[CRDV_SLOW_FRAGMENT_BYTES];
    uint8_t bits[96];
    BOOL data_sync = FALSE;

    dstar_tx_stream_slowData(stream, slow, &data_sync);
    if (crdv_voice_frame_pack(frame, slow, bits) != CRDV_OK) {
        return FALSE;
    }
    if (data_sync) {
        static const uint8_t kDataSync[24] = {
            1U, 0U, 1U, 0U, 1U, 0U, 1U, 0U,
            1U, 0U, 1U, 1U, 0U, 1U, 0U, 0U,
            0U, 1U, 1U, 0U, 1U, 0U, 0U, 0U
        };
        memcpy(bits + 72U, kDataSync, sizeof(kDataSync));
    }
    if (!dstar_tx_stream_appendBits(stream, modulator, samples, bits,
                                    sizeof(bits))) {
        return FALSE;
    }

    stream->frame_count++;
    if (!valid) {
        stream->null_frame_count++;
    }
    return TRUE;
}

static void dstar_tx_stream_dropHead(dstar_tx_stream *stream)
{
    if (stream->ambe_queue_count == 0U) {
        return;
    }
    stream->ambe_queue_head = (stream->ambe_queue_head + 1U)
        % DSTAR_TX_STREAM_AMBE_QUEUE_CAPACITY;
    stream->ambe_queue_count--;
}

static void dstar_tx_stream_appendNextVoice(
    dstar_tx_stream *stream,
    GMSK_MOD modulator,
    Circular_Float_Buffer samples)
{
    while (stream->ambe_queue_count > 0U) {
        const dstar_tx_stream_ambe_frame *frame =
            &stream->ambe_queue[stream->ambe_queue_head];
        if (frame->sequence >= stream->next_sequence) {
            break;
        }
        stream->sequence_error_count++;
        dstar_tx_stream_dropHead(stream);
    }

    if (stream->ambe_queue_count == 0U) {
        if (dstar_tx_stream_appendVoice(stream, modulator, samples,
                                        kNullAmbe, FALSE)) {
            stream->underflow_count++;
            stream->next_sequence++;
        }
        return;
    }

    const dstar_tx_stream_ambe_frame *frame =
        &stream->ambe_queue[stream->ambe_queue_head];
    if (frame->sequence > stream->next_sequence) {
        if (dstar_tx_stream_appendVoice(stream, modulator, samples,
                                        kNullAmbe, FALSE)) {
            stream->underflow_count++;
            stream->next_sequence++;
        }
        return;
    }

    if (dstar_tx_stream_appendVoice(stream, modulator, samples,
                                    frame->bytes, frame->valid)) {
        dstar_tx_stream_dropHead(stream);
        stream->next_sequence++;
    }
}

void dstar_tx_stream_reset(dstar_tx_stream *stream,
                           Circular_Float_Buffer samples)
{
    if (stream == NULL || samples == NULL) {
        return;
    }
    memset(stream, 0, sizeof(*stream));
    stream->phase = DSTAR_TX_STREAM_IDLE;
    zero_cfb(samples);
}

BOOL dstar_tx_stream_begin(dstar_tx_stream *stream,
                           const aether_dstar_tx_snapshot *snapshot,
                           GMSK_MOD modulator,
                           Circular_Float_Buffer samples)
{
    enum {
        kPrefixCapacity = AETHER_DSTAR_MAX_PREAMBLE_BITS + 15U
            + CRDV_PROTECTED_BITS
    };
    uint8_t prefix[kPrefixCapacity];
    size_t prefix_bits = 0U;

    if (stream == NULL || snapshot == NULL || modulator == NULL
        || samples == NULL || stream->phase != DSTAR_TX_STREAM_IDLE) {
        return FALSE;
    }

    dstar_tx_stream_reset(stream, samples);
    stream->snapshot = *snapshot;
    if (crdv_transmission_prefix(snapshot->header,
                                 snapshot->preamble_bits,
                                 prefix,
                                 sizeof(prefix),
                                 &prefix_bits) != CRDV_OK
        || !dstar_tx_stream_appendBits(stream, modulator, samples,
                                       prefix, prefix_bits)) {
        dstar_tx_stream_reset(stream, samples);
        return FALSE;
    }
    stream->header_total_samples = (uint32)cfbContains(samples);
    stream->header_preamble_samples =
        (uint32)(snapshot->preamble_bits * AETHER_DSTAR_SAMPLES_PER_BIT);
    stream->phase = DSTAR_TX_STREAM_HEADER;
    return TRUE;
}

void dstar_tx_stream_offerVoice(dstar_tx_stream *stream,
                                GMSK_MOD modulator,
                                Circular_Float_Buffer samples,
                                uint64 sequence,
                                const unsigned char *ambe,
                                BOOL valid)
{
    if (stream == NULL || modulator == NULL || samples == NULL) {
        return;
    }
    if (stream->phase != DSTAR_TX_STREAM_HEADER
        && stream->phase != DSTAR_TX_STREAM_VOICE) {
        return;
    }
    if (sequence < stream->next_sequence) {
        stream->sequence_error_count++;
        return;
    }
    if (stream->ambe_queue_count > 0U) {
        const uint32 tail_index = (stream->ambe_queue_head
            + stream->ambe_queue_count - 1U)
            % DSTAR_TX_STREAM_AMBE_QUEUE_CAPACITY;
        if (sequence <= stream->ambe_queue[tail_index].sequence) {
            stream->sequence_error_count++;
            return;
        }
    }

    if (stream->ambe_queue_count == DSTAR_TX_STREAM_AMBE_QUEUE_CAPACITY) {
        const uint64 dropped_sequence =
            stream->ambe_queue[stream->ambe_queue_head].sequence;
        dstar_tx_stream_dropHead(stream);
        stream->overflow_count++;
        if (stream->next_sequence <= dropped_sequence) {
            stream->next_sequence = dropped_sequence + 1U;
        }
    }

    const uint32 tail_index = (stream->ambe_queue_head
        + stream->ambe_queue_count) % DSTAR_TX_STREAM_AMBE_QUEUE_CAPACITY;
    dstar_tx_stream_ambe_frame *frame = &stream->ambe_queue[tail_index];
    frame->sequence = sequence;
    frame->valid = valid && ambe != NULL;
    memcpy(frame->bytes, frame->valid ? ambe : kNullAmbe,
           sizeof(frame->bytes));
    stream->ambe_queue_count++;
    if (stream->phase == DSTAR_TX_STREAM_HEADER) {
        stream->pre_roll_frames++;
    }
    if (stream->ambe_queue_count > stream->queue_max_frames) {
        stream->queue_max_frames = stream->ambe_queue_count;
    }
}

dstar_tx_stream_end_result dstar_tx_stream_requestEnd(
    dstar_tx_stream *stream,
    GMSK_MOD modulator,
    Circular_Float_Buffer samples)
{
    uint8_t tail[48];
    if (stream == NULL || modulator == NULL || samples == NULL) {
        return DSTAR_TX_STREAM_END_NONE;
    }
    if (stream->phase != DSTAR_TX_STREAM_HEADER
        && stream->phase != DSTAR_TX_STREAM_VOICE) {
        return DSTAR_TX_STREAM_END_NONE;
    }
    if (stream->ambe_queue_count > 0U) {
        return DSTAR_TX_STREAM_END_DRAIN_PENDING;
    }

    crdv_last_frame_bits(tail);
    if (!dstar_tx_stream_appendBits(stream, modulator, samples,
                                    tail, sizeof(tail))) {
        return DSTAR_TX_STREAM_END_NONE;
    }
    stream->phase = DSTAR_TX_STREAM_ENDING;
    return DSTAR_TX_STREAM_END_QUEUED;
}

static void dstar_tx_stream_prepareVoice(dstar_tx_stream *stream,
                                         GMSK_MOD modulator,
                                         Circular_Float_Buffer samples,
                                         uint32 required_samples)
{
    if (stream->phase == DSTAR_TX_STREAM_HEADER
        && (uint32)cfbContains(samples) < required_samples) {
        stream->phase = DSTAR_TX_STREAM_VOICE;
        dstar_tx_stream_appendNextVoice(stream, modulator, samples);
    }
    if (stream->phase == DSTAR_TX_STREAM_VOICE
        && (uint32)cfbContains(samples) < required_samples) {
        dstar_tx_stream_appendNextVoice(stream, modulator, samples);
    }
}

uint32 dstar_tx_stream_readPacket(dstar_tx_stream *stream,
                                  GMSK_MOD modulator,
                                  Circular_Float_Buffer samples,
                                  float *output,
                                  uint32 output_samples)
{
    if (stream == NULL || modulator == NULL || samples == NULL
        || output == NULL || output_samples == 0U
        || stream->phase == DSTAR_TX_STREAM_IDLE) {
        return 0U;
    }

    dstar_tx_stream_prepareVoice(stream, modulator, samples, output_samples);
    uint32 available = (uint32)cfbContains(samples);
    if (available > output_samples) {
        available = output_samples;
    }
    for (uint32 i = 0U; i < available; i++) {
        output[i] = cbReadFloat(samples);
    }
    return available;
}

uint32 dstar_tx_stream_pendingSamples(const dstar_tx_stream *stream,
                                      Circular_Float_Buffer samples)
{
    if (stream == NULL || samples == NULL
        || stream->phase == DSTAR_TX_STREAM_IDLE) {
        return 0U;
    }
    return (uint32)cfbContains(samples);
}

uint32 dstar_tx_stream_emittedHeaderSamples(
    const dstar_tx_stream *stream,
    Circular_Float_Buffer samples)
{
    if (stream == NULL || samples == NULL
        || stream->header_total_samples == 0U) {
        return 0U;
    }
    const uint32 pending = (uint32)cfbContains(samples);
    return pending < stream->header_total_samples
        ? stream->header_total_samples - pending : 0U;
}

BOOL dstar_tx_stream_preambleComplete(
    const dstar_tx_stream *stream,
    Circular_Float_Buffer samples)
{
    if (stream == NULL || samples == NULL) {
        return FALSE;
    }
    if (stream->phase != DSTAR_TX_STREAM_HEADER) {
        return TRUE;
    }
    return dstar_tx_stream_emittedHeaderSamples(stream, samples)
        >= stream->header_preamble_samples;
}

BOOL dstar_tx_stream_abortHeader(dstar_tx_stream *stream,
                                 Circular_Float_Buffer samples)
{
    if (stream == NULL || samples == NULL
        || stream->phase != DSTAR_TX_STREAM_HEADER) {
        return FALSE;
    }
    dstar_tx_stream_reset(stream, samples);
    return TRUE;
}

uint32 dstar_tx_stream_pendingVoiceFrames(const dstar_tx_stream *stream)
{
    return stream == NULL ? 0U : stream->ambe_queue_count;
}

uint32 dstar_tx_stream_discardPendingVoice(dstar_tx_stream *stream)
{
    if (stream == NULL) {
        return 0U;
    }
    const uint32 discarded = stream->ambe_queue_count;
    stream->ambe_queue_head = 0U;
    stream->ambe_queue_count = 0U;
    stream->discarded_frame_count += discarded;
    return discarded;
}

BOOL dstar_tx_stream_finished(const dstar_tx_stream *stream,
                              Circular_Float_Buffer samples)
{
    return stream != NULL && samples != NULL
        && stream->phase == DSTAR_TX_STREAM_ENDING
        && cfbIsEmpty(samples);
}
