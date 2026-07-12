/* End-to-end offline D-STAR TX lifecycle, framing, and VITA output test. */

#include "aether_dstar_protocol.h"
#include "digital_voice_tx_gate.h"
#include "dstar_transmit_state.h"
#include "dstar_tx_stream.h"
#include "traffic_cop.h"
#include "vita.h"
#include "vita_output.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

enum {
    TEST_QUEUE_CAPACITY = 32768U,
    TEST_PACKET_SAMPLES = 128U,
    TEST_STREAM_ID = 0x81000000U
};

static int failures = 0;
static uint32 sent_packets = 0U;
static uint32 last_packet_count = 0U;
static uint32 last_stream_id = 0U;

static void expect_true(const char * name, BOOL condition)
{
    printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", name);
    if (!condition) {
        failures++;
    }
}

uint32 tc_sendSmartSDRcommand(char * command, BOOL block, char ** response)
{
    (void)command;
    (void)block;
    (void)response;
    return 0U;
}

void tc_abort(void)
{
}

void thumbDV_dump(char * text, unsigned char * data, unsigned int length)
{
    (void)text;
    (void)data;
    (void)length;
}

BOOL hal_ListenerSend(const void * packet,
                      uint32 num_bytes,
                      uint32 ip_address,
                      uint16 udp_port)
{
    (void)ip_address;
    (void)udp_port;
    if (packet == NULL || num_bytes < 28U) {
        return FALSE;
    }
    const VitaIFData vita = (const VitaIFData)packet;
    last_stream_id = ntohl(vita->stream_id);
    last_packet_count =
        (ntohl(vita->header) & VITA_HEADER_PACKET_COUNT_MASK) >> 16U;
    sent_packets++;
    return TRUE;
}

static uint32 emit_one_packet(dstar_tx_stream * stream,
                              GMSK_MOD modulator,
                              Circular_Float_Buffer queue)
{
    float samples[TEST_PACKET_SAMPLES] = {0.0f};
    const uint32 read = dstar_tx_stream_readPacket(
        stream, modulator, queue, samples, TEST_PACKET_SAMPLES);
    if (read == 0U) {
        return 0U;
    }

    Complex complex_samples[TEST_PACKET_SAMPLES] = {{0.0f, 0.0f}};
    for (uint32 i = 0U; i < read; i++) {
        complex_samples[i].real = samples[i];
        complex_samples[i].imag = samples[i];
    }
    buffer_descriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.stream_id = TEST_STREAM_ID;
    descriptor.num_samples = TEST_PACKET_SAMPLES;
    descriptor.sample_size = sizeof(Complex);
    descriptor.buf_ptr = complex_samples;
    expect_true("VITA packet send succeeds", emit_waveform_output(&descriptor));
    return read;
}

int main(void)
{
    float queue_storage[TEST_QUEUE_CAPACITY] = {0.0f};
    circular_float_buffer queue = {
        .size = TEST_QUEUE_CAPACITY,
        .start = 0U,
        .end = 0U,
        .elems = queue_storage,
        .name = "tx-path"
    };
    dstar_tx_stream stream = DSTAR_TX_STREAM_INITIALIZER;
    dstar_transmit_state transmit = DSTAR_TRANSMIT_STATE_INITIALIZER;
    digital_voice_tx_gate gate = DIGITAL_VOICE_TX_GATE_INITIALIZER;
    aether_dstar_protocol protocol;
    aether_dstar_protocol_init(&protocol);
    aether_dstar_protocol_set_mycall(&protocol, "N0SPEC");
    aether_dstar_protocol_set_message(&protocol, "TX PATH TEST");
    aether_dstar_protocol_set_preamble(
        &protocol, AETHER_DSTAR_MIN_PREAMBLE_BITS);
    aether_dstar_tx_snapshot snapshot;
    expect_true("TX fixture produces a clean protocol snapshot",
                aether_dstar_protocol_tx_snapshot(&protocol, &snapshot)
                    == CRDV_OK);
    GMSK_MOD modulator = gmsk_createModulator();
    modulator->m_invert = TRUE;

    digital_voice_tx_gate_setModeSlice(&gate, TRUE, 1U);
    digital_voice_tx_gate_setTxSlice(&gate, TRUE, 1U);
    const digital_voice_tx_gate_action begin_action =
        digital_voice_tx_gate_observe(
            &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
            TRUE, DIGITAL_VOICE_TX_SOURCE_HARDWARE);
    expect_true("interlock begins eligible D-STAR TX",
                begin_action == DIGITAL_VOICE_TX_GATE_BEGIN);
    expect_true("atomic scheduler state enters ACTIVE",
                dstar_transmit_state_begin(&transmit));
    expect_true("framing stream begins",
                dstar_tx_stream_begin(
                    &stream, &snapshot, modulator, &queue));

    const uint32 header_samples = (AETHER_DSTAR_MIN_PREAMBLE_BITS
        + 15U + CRDV_PROTECTED_BITS) * AETHER_DSTAR_SAMPLES_PER_BIT;
    expect_true("header contains exact preamble, sync, and FEC samples",
                dstar_tx_stream_pendingSamples(&stream, &queue)
                    == header_samples);
    expect_true("fixed header is included in bounded sample telemetry",
                stream.queue_max_samples == header_samples);

    unsigned char delayed_ambe[6U][CRDV_AMBE_BYTES] = {{0}};
    for (uint32 i = 0U; i < 6U; i++) {
        delayed_ambe[i][0] = (unsigned char)(i + 1U);
        delayed_ambe[i][CRDV_AMBE_BYTES - 1U] =
            (unsigned char)(0xA0U + i);
        dstar_tx_stream_offerVoice(
            &stream, modulator, &queue,
            i, delayed_ambe[i], TRUE);
    }
    expect_true("header-era ThumbDV responses do not alter framing samples",
                dstar_tx_stream_pendingSamples(&stream, &queue)
                    == header_samples);
    expect_true("header-era speech does not inflate modulated sample depth",
                stream.queue_max_samples == header_samples);
    expect_true("all header-era AMBE frames remain queued in order",
                dstar_tx_stream_pendingVoiceFrames(&stream) == 6U
                    && stream.pre_roll_frames == 6U
                    && stream.queue_max_frames == 6U
                    && stream.ambe_queue[stream.ambe_queue_head].sequence == 0U
                    && memcmp(stream.ambe_queue[stream.ambe_queue_head].bytes,
                              delayed_ambe[0], CRDV_AMBE_BYTES) == 0);

    vita_output_Init("127.0.0.1");
    uint32 header_packets = 0U;
    while (stream.phase == DSTAR_TX_STREAM_HEADER
           && header_packets < 64U) {
        expect_true("header packet is available",
                    emit_one_packet(
                        &stream, modulator, &queue) > 0U);
        header_packets++;
    }
    expect_true("header transitions into voice with oldest speech first",
                stream.phase == DSTAR_TX_STREAM_VOICE
                    && stream.next_sequence == 1U
                    && dstar_tx_stream_pendingVoiceFrames(&stream) == 5U
                    && dstar_tx_stream_pendingSamples(&stream, &queue) < 608U);
    expect_true("pre-roll avoids a startup null frame",
                stream.null_frame_count == 0U
                    && stream.underflow_count == 0U);

    for (uint32 frame = 0U; frame < 4U; frame++) {
        unsigned char live_ambe[CRDV_AMBE_BYTES] = {0};
        live_ambe[0] = (unsigned char)(0x20U + frame);
        dstar_tx_stream_offerVoice(
            &stream, modulator, &queue,
            6U + frame, live_ambe, TRUE);
        for (uint32 packet = 0U; packet < 4U; packet++) {
            emit_one_packet(&stream, modulator, &queue);
        }
        expect_true("live voice queue remains bounded",
                    dstar_tx_stream_pendingSamples(&stream, &queue) < 768U);
    }

    const digital_voice_tx_gate_action end_action =
        digital_voice_tx_gate_observe(
            &gate, DIGITAL_VOICE_TX_EVENT_UNKEY_REQUESTED,
            FALSE, DIGITAL_VOICE_TX_SOURCE_UNKNOWN);
    expect_true("source-less UNKEY requests the D-STAR tail",
                end_action == DIGITAL_VOICE_TX_GATE_REQUEST_END);
    expect_true("atomic scheduler state starts draining",
                dstar_transmit_state_requestEnd(&transmit));
    expect_true("end pattern waits for queued speech",
                dstar_tx_stream_requestEnd(
                    &stream, modulator, &queue)
                    == DSTAR_TX_STREAM_END_DRAIN_PENDING);
    uint32 drain_packets = 0U;
    while (dstar_tx_stream_pendingVoiceFrames(&stream) > 0U
           && drain_packets < 64U) {
        emit_one_packet(&stream, modulator, &queue);
        drain_packets++;
    }
    expect_true("all queued speech drains before EOT",
                dstar_tx_stream_pendingVoiceFrames(&stream) == 0U
                    && drain_packets > 0U);
    expect_true("end pattern is queued once",
                dstar_tx_stream_requestEnd(
                    &stream, modulator, &queue)
                    == DSTAR_TX_STREAM_END_QUEUED);
    expect_true("atomic scheduler state enters ENDING after EOT is queued",
                dstar_transmit_state_markEndQueued(&transmit));
    const uint32 bounded_tail_samples =
        dstar_tx_stream_pendingSamples(&stream, &queue);
    expect_true("tail is bounded below 1024 samples",
                bounded_tail_samples > 0U
                    && bounded_tail_samples < 1024U);
    expect_true("duplicate end request is ignored",
                dstar_tx_stream_requestEnd(
                    &stream, modulator, &queue)
                    == DSTAR_TX_STREAM_END_NONE);

    uint32 tail_packets = 0U;
    while (!dstar_tx_stream_finished(&stream, &queue)
           && tail_packets < 8U) {
        emit_one_packet(&stream, modulator, &queue);
        tail_packets++;
    }
    expect_true("tail completes without another radio input buffer",
                dstar_tx_stream_finished(&stream, &queue));
    expect_true("bounded tail needs no more than eight VITA packets",
                tail_packets > 0U && tail_packets <= 8U);
    dstar_transmit_state_finishTail(&transmit);
    expect_true("scheduler returns to IDLE after tail completion",
                dstar_transmit_state_phase(&transmit)
                    == DSTAR_TRANSMIT_IDLE);
    expect_true("all VITA packets retain the TX input stream ID",
                last_stream_id == TEST_STREAM_ID);
    expect_true("VITA packet counts advance per stream",
                sent_packets > 1U
                    && last_packet_count == (sent_packets - 1U) % 16U);

    dstar_tx_stream_reset(&stream, &queue);
    expect_true("a fresh framing stream begins for overflow coverage",
                dstar_tx_stream_begin(
                    &stream, &snapshot, modulator, &queue));
    for (uint64 sequence = 0U;
         sequence < DSTAR_TX_STREAM_AMBE_QUEUE_CAPACITY + 1U;
         sequence++) {
        unsigned char frame[CRDV_AMBE_BYTES] = {0};
        frame[0] = (unsigned char)sequence;
        dstar_tx_stream_offerVoice(
            &stream, modulator, &queue,
            sequence, frame, TRUE);
    }
    expect_true("AMBE pre-roll remains bounded and drops the oldest frame",
                dstar_tx_stream_pendingVoiceFrames(&stream)
                        == DSTAR_TX_STREAM_AMBE_QUEUE_CAPACITY
                    && stream.overflow_count == 1U
                    && stream.next_sequence == 1U
                    && stream.ambe_queue[stream.ambe_queue_head].sequence == 1U);
    unsigned char duplicate_frame[CRDV_AMBE_BYTES] = {0};
    dstar_tx_stream_offerVoice(
        &stream, modulator, &queue,
        DSTAR_TX_STREAM_AMBE_QUEUE_CAPACITY,
        duplicate_frame, TRUE);
    expect_true("duplicate AMBE responses are rejected",
                stream.sequence_error_count == 1U
                    && dstar_tx_stream_pendingVoiceFrames(&stream)
                        == DSTAR_TX_STREAM_AMBE_QUEUE_CAPACITY);
    expect_true("hard cancellation discards every queued AMBE frame",
                dstar_tx_stream_discardPendingVoice(&stream)
                        == DSTAR_TX_STREAM_AMBE_QUEUE_CAPACITY
                    && dstar_tx_stream_pendingVoiceFrames(&stream) == 0U
                    && stream.discarded_frame_count
                        == DSTAR_TX_STREAM_AMBE_QUEUE_CAPACITY);

    dstar_tx_stream_reset(&stream, &queue);
    expect_true("a short-key framing stream begins",
                dstar_tx_stream_begin(
                    &stream, &snapshot, modulator, &queue));
    expect_true("new framing stream has not emitted its preamble",
                dstar_tx_stream_emittedHeaderSamples(&stream, &queue) == 0U
                    && !dstar_tx_stream_preambleComplete(&stream, &queue));
    unsigned char aborted_ambe[CRDV_AMBE_BYTES] = {0x55U};
    dstar_tx_stream_offerVoice(
        &stream, modulator, &queue,
        0U, aborted_ambe, TRUE);
    expect_true("unkey before the RF preamble completes aborts cleanly",
                dstar_tx_stream_abortHeader(&stream, &queue)
                    && stream.phase == DSTAR_TX_STREAM_IDLE
                    && dstar_tx_stream_pendingSamples(&stream, &queue) == 0U
                    && dstar_tx_stream_pendingVoiceFrames(&stream) == 0U);

    expect_true("a post-preamble framing stream begins",
                dstar_tx_stream_begin(
                    &stream, &snapshot, modulator, &queue));
    uint32 preamble_packets = 0U;
    while (!dstar_tx_stream_preambleComplete(&stream, &queue)
           && preamble_packets < 16U) {
        expect_true("preamble packet is available",
                    emit_one_packet(
                        &stream, modulator, &queue) > 0U);
        preamble_packets++;
    }
    expect_true("preamble completion is tracked before the protected header ends",
                dstar_tx_stream_preambleComplete(&stream, &queue)
                    && stream.phase == DSTAR_TX_STREAM_HEADER
                    && dstar_tx_stream_emittedHeaderSamples(&stream, &queue)
                        >= stream.header_preamble_samples);
    const uint32 remaining_header_samples =
        dstar_tx_stream_pendingSamples(&stream, &queue);
    expect_true("unkey after the RF preamble preserves the remaining header",
                dstar_tx_stream_requestEnd(
                    &stream, modulator, &queue)
                        == DSTAR_TX_STREAM_END_QUEUED
                    && stream.phase == DSTAR_TX_STREAM_ENDING
                    && dstar_tx_stream_pendingSamples(&stream, &queue)
                        > remaining_header_samples);

    gmsk_destroyModulator(modulator);

    printf("\n%s D-STAR offline TX path tests.\n",
           failures == 0 ? "All" : "Failed");
    return failures == 0 ? 0 : 1;
}
