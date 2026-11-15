/* Focused integration tests for the clean-room D-STAR protocol core. */

#include "aether_dstar_protocol.h"
#include "gmsk_modem.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed = 0;

static void report(const char *name, BOOL ok)
{
    printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        failed++;
    }
}

static aether_dstar_protocol configured_protocol(void)
{
    aether_dstar_protocol protocol;
    aether_dstar_protocol_init(&protocol);
    aether_dstar_protocol_set_mycall(&protocol, "N0SPEC");
    aether_dstar_protocol_set_suffix(&protocol, "A");
    aether_dstar_protocol_set_urcall(&protocol, "CQCQCQ");
    aether_dstar_protocol_set_rpt1(&protocol, "DIRECT");
    aether_dstar_protocol_set_rpt2(&protocol, "DIRECT");
    aether_dstar_protocol_set_message(&protocol, "ABCDEFGHIJKLMNOPQRST");
    return protocol;
}

static void test_configuration_snapshot(void)
{
    aether_dstar_protocol protocol = configured_protocol();
    aether_dstar_tx_snapshot snapshot;
    const crdv_result result = aether_dstar_protocol_tx_snapshot(
        &protocol, &snapshot);

    report("valid station configuration produces a TX snapshot",
           result == CRDV_OK);
    report("TX snapshot contains a valid protected-header source",
           crdv_pfcs(snapshot.header, 39U)
               == ((uint16_t)snapshot.header[39]
                   | (uint16_t)((uint16_t)snapshot.header[40] << 8U)));
    report("direct routing leaves D-STAR header flags clear",
           snapshot.fields.flags[0] == 0U
               && snapshot.fields.flags[1] == 0U
               && snapshot.fields.flags[2] == 0U);
    report("station fields are normalized and padded",
           memcmp(snapshot.fields.mycall, "N0SPEC  ", 8U) == 0
               && memcmp(snapshot.fields.suffix, "A   ", 4U) == 0);

    aether_dstar_protocol_set_rpt2(&protocol, "W1ABC B");
    report("repeater routing sets the compatibility repeater flag",
           aether_dstar_protocol_tx_snapshot(&protocol, &snapshot) == CRDV_OK
               && snapshot.fields.flags[0] == 0x40U);

    aether_dstar_protocol_set_mycall(&protocol, "INVALID");
    report("MYCALL without a digit is rejected before TX",
           aether_dstar_protocol_tx_snapshot(&protocol, &snapshot)
               == CRDV_E_FORMAT);

    aether_dstar_protocol_set_mycall(&protocol, "N0SPEC");
    report("command token spaces decode without accepting delimiters",
           aether_dstar_protocol_set_message(&protocol, "HELLO\x7fWORLD")
               && strcmp(protocol.message, "HELLO WORLD") == 0
               && !aether_dstar_protocol_set_message(&protocol, "BAD|VALUE"));

    report("complete station updates validate and commit atomically",
           aether_dstar_protocol_configure(
               &protocol,
               "K1ABC",
               "B",
               "CQCQCQ",
               "DIRECT",
               "DIRECT",
               "ATOMIC UPDATE") == CRDV_OK
               && strcmp(protocol.mycall, "K1ABC") == 0);
    report("a rejected station update preserves the previous configuration",
           aether_dstar_protocol_configure(
               &protocol,
               "NOCALL",
               "B",
               "CQCQCQ",
               "DIRECT",
               "DIRECT",
               "SHOULD NOT APPLY") == CRDV_E_FORMAT
               && strcmp(protocol.mycall, "K1ABC") == 0
               && strcmp(protocol.message, "ATOMIC UPDATE") == 0);
}

static void feed_bits(aether_dstar_protocol *protocol,
                      const uint8_t *bits,
                      size_t count,
                      unsigned *headers,
                      unsigned *voices,
                      unsigned *syncs,
                      unsigned *messages,
                      unsigned *ends,
                      char message[21])
{
    for (size_t index = 0U; index < count; index++) {
        aether_dstar_rx_event event;
        const crdv_result result = aether_dstar_protocol_push_rx_bit(
            protocol, bits[index], &event);
        if (result != CRDV_OK) {
            failed++;
            return;
        }
        *headers += event.header ? 1U : 0U;
        *voices += event.voice ? 1U : 0U;
        *syncs += event.data_sync ? 1U : 0U;
        *messages += event.message ? 1U : 0U;
        *ends += event.end ? 1U : 0U;
        if (event.message) {
            memcpy(message, event.message_text, 21U);
        }
    }
}

static void test_receive_session(void)
{
    static const uint8_t data_sync[24] = {
        1U, 0U, 1U, 0U, 1U, 0U, 1U, 0U,
        1U, 0U, 1U, 1U, 0U, 1U, 0U, 0U,
        0U, 1U, 1U, 0U, 1U, 0U, 0U, 0U
    };
    aether_dstar_protocol tx = configured_protocol();
    aether_dstar_protocol rx;
    aether_dstar_protocol_init(&rx);
    aether_dstar_tx_snapshot snapshot;
    uint8_t prefix[AETHER_DSTAR_MIN_PREAMBLE_BITS + 15U
                   + CRDV_PROTECTED_BITS];
    size_t prefix_count = 0U;
    unsigned headers = 0U;
    unsigned voices = 0U;
    unsigned syncs = 0U;
    unsigned messages = 0U;
    unsigned ends = 0U;
    char received_message[21] = {0};

    report("receive fixture snapshot is valid",
           aether_dstar_protocol_tx_snapshot(&tx, &snapshot) == CRDV_OK);
    report("receive fixture prefix is valid",
           crdv_transmission_prefix(snapshot.header,
                                    AETHER_DSTAR_MIN_PREAMBLE_BITS,
                                    prefix,
                                    sizeof(prefix),
                                    &prefix_count) == CRDV_OK);
    feed_bits(&rx, prefix, prefix_count, &headers, &voices, &syncs,
              &messages, &ends, received_message);

    uint8_t ambe[CRDV_AMBE_BYTES] = {0x01U};
    uint8_t slow[CRDV_SLOW_FRAGMENT_BYTES] = {0};
    uint8_t frame[96];
    crdv_voice_frame_pack(ambe, slow, frame);
    memcpy(frame + 72U, data_sync, sizeof(data_sync));
    feed_bits(&rx, frame, sizeof(frame), &headers, &voices, &syncs,
              &messages, &ends, received_message);

    for (unsigned block = 1U; block <= 4U; block++) {
        uint8_t first[CRDV_SLOW_FRAGMENT_BYTES];
        uint8_t second[CRDV_SLOW_FRAGMENT_BYTES];
        crdv_message_block(snapshot.message, block, first, second);
        crdv_voice_frame_pack(ambe, first, frame);
        feed_bits(&rx, frame, sizeof(frame), &headers, &voices, &syncs,
                  &messages, &ends, received_message);
        crdv_voice_frame_pack(ambe, second, frame);
        feed_bits(&rx, frame, sizeof(frame), &headers, &voices, &syncs,
                  &messages, &ends, received_message);
    }

    uint8_t tail[48];
    crdv_last_frame_bits(tail);
    feed_bits(&rx, tail, sizeof(tail), &headers, &voices, &syncs,
              &messages, &ends, received_message);

    report("clean receiver publishes one initial header", headers == 1U);
    report("clean receiver emits each AMBE frame exactly once", voices == 9U);
    report("clean receiver recognizes the standard data sync", syncs == 1U);
    report("slow-data message publishes once after all four blocks",
           messages == 1U
               && memcmp(received_message, "ABCDEFGHIJKLMNOPQRST", 20U) == 0);
    report("clean receiver reports exactly one last frame", ends == 1U);
}

static void test_gmsk_clock_recovery(void)
{
    enum { kBits = 12000U, kAlignment = 12U };
    BOOL *sent = calloc(kBits, sizeof(*sent));
    BOOL *recovered = calloc(kBits + 32U, sizeof(*recovered));
    GMSK_MOD modulator = gmsk_createModulator();
    GMSK_DEMOD demodulator = gmsk_createDemodulator();
    uint32_t lfsr = 0x1aceU;
    size_t recovered_count = 0U;

    report("GMSK test allocations succeed",
           sent != NULL && recovered != NULL
               && modulator != NULL && demodulator != NULL);
    if (sent == NULL || recovered == NULL || modulator == NULL
        || demodulator == NULL) {
        free(sent);
        free(recovered);
        return;
    }

    modulator->m_invert = TRUE;
    demodulator->m_invert = TRUE;
    for (size_t index = 0U; index < kBits; index++) {
        sent[index] = (lfsr & 1U) != 0U;
        const uint32_t feedback = ((lfsr >> 0U) ^ (lfsr >> 2U)
                                   ^ (lfsr >> 3U) ^ (lfsr >> 5U)) & 1U;
        lfsr = (lfsr >> 1U) | (feedback << 15U);
        float samples[GMSK_SAMPLES_PER_SYMBOL];
        gmsk_encode(modulator, sent[index], samples,
                    GMSK_SAMPLES_PER_SYMBOL);
        for (size_t sample = 0U; sample < GMSK_SAMPLES_PER_SYMBOL; sample++) {
            const enum DEMOD_STATE state = gmsk_decode(
                demodulator, samples[sample]);
            if (state != DEMOD_UNKNOWN && recovered_count < kBits + 32U) {
                recovered[recovered_count++] = state == DEMOD_TRUE;
            }
        }
    }

    size_t best_errors = kBits;
    for (size_t sent_offset = 0U; sent_offset <= kAlignment; sent_offset++) {
        for (size_t rx_offset = 0U; rx_offset <= kAlignment; rx_offset++) {
            const size_t comparable = (kBits - sent_offset)
                < (recovered_count - rx_offset)
                ? (kBits - sent_offset) : (recovered_count - rx_offset);
            size_t errors = 0U;
            for (size_t bit = 0U; bit < comparable; bit++) {
                errors += sent[sent_offset + bit]
                    != recovered[rx_offset + bit] ? 1U : 0U;
            }
            if (errors < best_errors) {
                best_errors = errors;
            }
        }
    }
    report("Flex GPLv3 symbol clock emits one decision per five samples",
           recovered_count >= kBits - 2U && recovered_count <= kBits + 2U);
    report("Flex GPLv3 GMSK modem remains bit-exact over 2.5 seconds",
           best_errors == 0U);

    gmsk_destroyDemodulator(demodulator);
    gmsk_destroyModulator(modulator);
    free(recovered);
    free(sent);
}

int main(void)
{
    test_configuration_snapshot();
    test_receive_session();
    test_gmsk_clock_recovery();
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
