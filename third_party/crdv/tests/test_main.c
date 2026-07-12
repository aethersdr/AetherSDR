/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "crdv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
static unsigned checks;

#define CHECK(expression)                                                        \
    do {                                                                         \
        ++checks;                                                                \
        if (!(expression)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,            \
                    #expression);                                                \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static uint8_t packed_bit(const uint8_t *bytes, size_t index)
{
    return (uint8_t)((bytes[index / 8u] >> (index % 8u)) & 1u);
}

static crdv_header_fields vector_fields(void)
{
    crdv_header_fields fields = {{0u, 0u, 0u}, {0}, {0}, {0}, {0}, {0}};
    memcpy(fields.rpt2, "DIRECT  ", 8u);
    memcpy(fields.rpt1, "DIRECT  ", 8u);
    memcpy(fields.urcall, "CQCQCQ  ", 8u);
    memcpy(fields.mycall, "N0SPEC  ", 8u);
    memcpy(fields.suffix, "A   ", 4u);
    return fields;
}

static void test_config(void)
{
    crdv_station_config config;
    CHECK(crdv_config_normalize("n0spec", NULL, NULL, NULL, NULL, NULL,
                                &config) == CRDV_OK);
    CHECK(memcmp(config.mycall, "N0SPEC  ", 8u) == 0);
    CHECK(memcmp(config.suffix, "    ", 4u) == 0);
    CHECK(memcmp(config.urcall, "CQCQCQ  ", 8u) == 0);
    CHECK(memcmp(config.rpt1, "DIRECT  ", 8u) == 0);
    CHECK(memcmp(config.message, "                    ", 20u) == 0);
    CHECK(crdv_config_normalize("ABC", "", NULL, NULL, NULL, NULL,
                                &config) == CRDV_E_FORMAT);
    CHECK(crdv_config_normalize("123", "", NULL, NULL, NULL, NULL,
                                &config) == CRDV_E_FORMAT);
    CHECK(crdv_config_normalize("A1", "", NULL, NULL, NULL, NULL,
                                &config) == CRDV_E_RANGE);
    CHECK(crdv_config_normalize("A1BCDEFGH", "", NULL, NULL, NULL, NULL,
                                &config) == CRDV_E_RANGE);
    CHECK(crdv_config_normalize("N0ABC", "ABCDE", NULL, NULL, NULL, NULL,
                                &config) == CRDV_E_RANGE);
    CHECK(crdv_config_normalize("N0ABC", "A-", NULL, NULL, NULL, NULL,
                                &config) == CRDV_E_FORMAT);
    CHECK(crdv_config_normalize("N0ABC", "", "/RPT A", "RPT 1", "D2",
                                "ASCII ~", &config) == CRDV_OK);
    CHECK(crdv_config_normalize("N0ABC", "", "//BAD", NULL, NULL, NULL,
                                &config) == CRDV_E_FORMAT);
    CHECK(crdv_config_normalize("N0ABC", "", "   ", NULL, NULL, NULL,
                                &config) == CRDV_E_FORMAT);
    CHECK(crdv_config_normalize("N0ABC", "", NULL, NULL, NULL, "bad|message",
                                &config) == CRDV_E_FORMAT);
}

static void test_header_vector(void)
{
    static const uint8_t expected_header[41] = {
        0x00, 0x00, 0x00, 0x44, 0x49, 0x52, 0x45, 0x43, 0x54, 0x20, 0x20,
        0x44, 0x49, 0x52, 0x45, 0x43, 0x54, 0x20, 0x20, 0x43, 0x51, 0x43,
        0x51, 0x43, 0x51, 0x20, 0x20, 0x4e, 0x30, 0x53, 0x50, 0x45, 0x43,
        0x20, 0x20, 0x41, 0x20, 0x20, 0x20, 0x05, 0xe9};
    static const uint8_t expected_protected[83] = {
        0x38, 0xcd, 0xeb, 0x4d, 0x40, 0xfc, 0x3a, 0xa8, 0xa0, 0x60, 0xab,
        0x1c, 0xbb, 0xa2, 0xb1, 0x5e, 0x1b, 0x65, 0x1b, 0xf6, 0xb0, 0x06,
        0xd9, 0x36, 0x9b, 0xab, 0xb2, 0x82, 0x13, 0xcc, 0x17, 0xfb, 0x5f,
        0x03, 0xed, 0x58, 0x2a, 0x88, 0x92, 0xd8, 0x03, 0xef, 0xf7, 0xa0,
        0x53, 0xc3, 0xea, 0x78, 0xeb, 0x96, 0x67, 0xd7, 0x1c, 0x8e, 0xdc,
        0xe4, 0x9d, 0x6f, 0xc5, 0x2e, 0xd0, 0xb7, 0x51, 0xab, 0x9e, 0x2c,
        0x2b, 0x07, 0x06, 0x53, 0xc9, 0x11, 0xd4, 0x6a, 0x44, 0xd0, 0x4a,
        0x77, 0x5f, 0xae, 0x49, 0x9e, 0x08};
    crdv_header_fields fields = vector_fields();
    crdv_header_fields decoded;
    uint8_t header[41];
    uint8_t protected_bytes[83];
    uint8_t recovered[41];
    unsigned metric = 99u;

    CHECK(crdv_header_pack(&fields, header) == CRDV_OK);
    CHECK(memcmp(header, expected_header, sizeof(header)) == 0);
    CHECK(crdv_pfcs(header, 39u) == 0xe905u);
    CHECK(crdv_header_protect(header, protected_bytes) == CRDV_OK);
    CHECK(memcmp(protected_bytes, expected_protected, sizeof(protected_bytes)) ==
          0);
    CHECK((protected_bytes[82] & 0xf0u) == 0u);
    CHECK(crdv_header_recover(protected_bytes, recovered, &metric) == CRDV_OK);
    CHECK(metric == 0u);
    CHECK(memcmp(recovered, header, sizeof(header)) == 0);
    CHECK(crdv_header_unpack(recovered, &decoded) == CRDV_OK);
    CHECK(memcmp(&decoded, &fields, sizeof(fields)) == 0);

    for (size_t index = 0; index < 39u * 8u; ++index) {
        uint8_t mutation[41];
        memcpy(mutation, header, sizeof(mutation));
        mutation[index / 8u] ^= (uint8_t)(1u << (index % 8u));
        CHECK(crdv_header_unpack(mutation, &decoded) == CRDV_E_CHECK);
        {
            uint16_t check = crdv_pfcs(mutation, 39u);
            mutation[39] = (uint8_t)(check & 0xffu);
            mutation[40] = (uint8_t)(check >> 8u);
            CHECK(crdv_header_unpack(mutation, &decoded) == CRDV_OK);
        }
    }

    for (size_t index = 0; index < 660u; index += 47u) {
        uint8_t mutation[83];
        memcpy(mutation, protected_bytes, sizeof(mutation));
        mutation[index / 8u] ^= (uint8_t)(1u << (index % 8u));
        CHECK(crdv_header_recover(mutation, recovered, &metric) == CRDV_OK);
        CHECK(memcmp(recovered, header, sizeof(header)) == 0);
        CHECK(metric == 1u);
    }
}

static void descramble_pair(const uint8_t first[3], const uint8_t second[3],
                            uint8_t block[6])
{
    memcpy(block, first, 3u);
    memcpy(block + 3u, second, 3u);
    crdv_slow_xor(block);
    crdv_slow_xor(block + 3u);
}

static void test_short_message_miniheaders(void)
{
    static const uint8_t message[] = "CLEANROOM DSTAR TEST";
    static const uint8_t expected[4][6] = {
        {0x30, 0x0c, 0xdf, 0x35, 0x0e, 0xdd},
        {0x31, 0x1d, 0xdc, 0x3f, 0x02, 0xb3},
        {0x32, 0x0b, 0xc0, 0x24, 0x0e, 0xc1},
        {0x33, 0x6f, 0xc7, 0x35, 0x1c, 0xc7}};
    crdv_message_assembler assembler;
    uint8_t complete[20];
    bool published;

    crdv_message_assembler_reset(&assembler, 10u);
    for (unsigned permutation = 0; permutation < 4u; ++permutation) {
        unsigned block_number = ((permutation * 3u) % 4u) + 1u;
        uint8_t first[3];
        uint8_t second[3];
        uint8_t block[6];
        CHECK(crdv_message_block(message, block_number, first, second) == CRDV_OK);
        CHECK(memcmp(first, expected[block_number - 1u], 3u) == 0);
        CHECK(memcmp(second, expected[block_number - 1u] + 3u, 3u) == 0);
        descramble_pair(first, second, block);
        CHECK(block[0] == (uint8_t)(0x40u + (block_number - 1u)));
        CHECK(crdv_message_assembler_push(&assembler, 10u, block, complete,
                                          &published) == CRDV_OK);
        CHECK(published == (permutation == 3u));
    }
    CHECK(memcmp(complete, message, CRDV_MESSAGE_BYTES) == 0);
    {
        uint8_t block[6] = {0x40, 'C', 'L', 'E', 'A', 'N'};
        CHECK(crdv_message_assembler_push(&assembler, 10u, block, complete,
                                          &published) == CRDV_OK);
        CHECK(!published);
        block[1] = 'X';
        CHECK(crdv_message_assembler_push(&assembler, 10u, block, complete,
                                          &published) == CRDV_E_CHECK);
        block[1] = 'C';
        CHECK(crdv_message_assembler_push(&assembler, 10u, block, complete,
                                          &published) == CRDV_E_STATE);
        CHECK(crdv_message_assembler_push(&assembler, 11u, block, complete,
                                          &published) == CRDV_OK);
    }
    {
        uint8_t invalid[6] = {0x44, 'B', 'L', 'O', 'C', 'K'};
        published = true;
        CHECK(crdv_message_assembler_push(&assembler, 12u, invalid, complete,
                                          &published) == CRDV_E_FORMAT);
        CHECK(!published);
    }
    CHECK(crdv_message_block(message, 0u, complete, complete + 3u) ==
          CRDV_E_RANGE);
    CHECK(crdv_message_block(message, 5u, complete, complete + 3u) ==
          CRDV_E_RANGE);
}

static void test_transmission_and_header_repeat(void)
{
    crdv_header_fields fields = vector_fields();
    crdv_header_fields repeated;
    crdv_header_repeat_assembler assembler;
    uint8_t header[41];
    uint8_t prefix[64u + 15u + 660u];
    size_t written;
    bool ready = false;

    CHECK(crdv_header_pack(&fields, header) == CRDV_OK);
    CHECK(crdv_transmission_prefix(header, 64u, prefix, sizeof(prefix),
                                   &written) == CRDV_OK);
    CHECK(written == sizeof(prefix));
    for (size_t index = 0; index < 64u; ++index) {
        CHECK(prefix[index] == (uint8_t)((index & 1u) == 0u));
    }
    CHECK(crdv_transmission_prefix(header, 63u, prefix, sizeof(prefix),
                                   &written) == CRDV_E_RANGE);
    crdv_header_repeat_reset(&assembler, 7u);
    for (unsigned block_index = 0u; block_index < 9u; ++block_index) {
        uint8_t first[3];
        uint8_t second[3];
        uint8_t block[6];
        CHECK(crdv_header_repeat_block(header, block_index, first, second) ==
              CRDV_OK);
        descramble_pair(first, second, block);
        CHECK(crdv_header_repeat_push(&assembler, 7u, block, &repeated, &ready) ==
              CRDV_OK);
        CHECK(ready == (block_index == 8u));
    }
    CHECK(memcmp(&fields, &repeated, sizeof(fields)) == 0);
}

typedef struct {
    unsigned count;
    uint8_t type;
    uint8_t fields[400];
    size_t length;
    bool parity;
} packet_capture;

static void capture_packet(void *context, const crdv_dv_packet_view *packet)
{
    packet_capture *capture = context;
    capture->count++;
    capture->type = (uint8_t)packet->type;
    capture->length = packet->field_length;
    capture->parity = packet->had_parity;
    if (packet->field_length <= sizeof(capture->fields)) {
        memcpy(capture->fields, packet->fields, packet->field_length);
    }
}

static void test_dv3000(void)
{
    static const uint8_t product_request[] = {0x61, 0x00, 0x03, 0x00,
                                               0x30, 0x2f, 0x1c};
    static const uint8_t reset_request[] = {0x61, 0x00, 0x03, 0x00,
                                             0x33, 0x2f, 0x1f};
    static const uint8_t disable_request[] = {0x61, 0x00, 0x04, 0x00,
                                               0x3f, 0x00, 0x2f, 0x14};
    static const uint8_t rate_request[] = {
        0x61, 0x00, 0x0d, 0x00, 0x0a, 0x01, 0x30, 0x07, 0x63,
        0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48};
    static const uint8_t decode_request[] = {
        0x61, 0x00, 0x0b, 0x01, 0x01, 0x48, 0x00, 0x11,
        0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    packet_capture capture;
    uint8_t built[400];
    size_t written;

    CHECK(crdv_dv_build_packet(CRDV_DV_CONTROL, product_request + 4u, 1u, true,
                               built, sizeof(built), &written) == CRDV_OK);
    CHECK(written == sizeof(product_request));
    CHECK(memcmp(built, product_request, written) == 0);
    CHECK(crdv_dv_build_packet(CRDV_DV_CONTROL, reset_request + 4u, 1u, true,
                               built, sizeof(built), &written) == CRDV_OK);
    CHECK(memcmp(built, reset_request, written) == 0);
    CHECK(crdv_dv_build_packet(CRDV_DV_CONTROL, disable_request + 4u, 2u, true,
                               built, sizeof(built), &written) == CRDV_OK);
    CHECK(memcmp(built, disable_request, written) == 0);
    CHECK(crdv_dv_build_packet(CRDV_DV_CONTROL, rate_request + 4u, 13u, false,
                               built, sizeof(built), &written) == CRDV_OK);
    CHECK(memcmp(built, rate_request, written) == 0);
    CHECK(crdv_dv_build_startup(CRDV_DV_START_DSTAR_RATE, built, sizeof(built),
                                &written) == CRDV_OK);
    CHECK(written == sizeof(rate_request));
    CHECK(memcmp(built, rate_request, written) == 0);
    CHECK(crdv_dv_build_decode(decode_request + 6u, false, built, sizeof(built),
                               &written) == CRDV_OK);
    CHECK(written == sizeof(decode_request));
    CHECK(memcmp(built, decode_request, written) == 0);

    for (size_t split = 0; split <= sizeof(product_request); ++split) {
        crdv_dv_parser parser;
        memset(&capture, 0, sizeof(capture));
        crdv_dv_parser_init(&parser, true, capture_packet, &capture);
        CHECK(crdv_dv_parser_feed(&parser, product_request, split) == CRDV_OK);
        CHECK(crdv_dv_parser_feed(&parser, product_request + split,
                                  sizeof(product_request) - split) == CRDV_OK);
        CHECK(capture.count == 1u);
        CHECK(capture.parity);
        CHECK(capture.type == CRDV_DV_CONTROL);
        CHECK(capture.length == 1u && capture.fields[0] == 0x30u);
    }
    {
        crdv_dv_parser parser;
        uint8_t corrupt[sizeof(product_request)];
        memcpy(corrupt, product_request, sizeof(corrupt));
        corrupt[sizeof(corrupt) - 1u] ^= 1u;
        memset(&capture, 0, sizeof(capture));
        crdv_dv_parser_init(&parser, true, capture_packet, &capture);
        for (size_t index = 0; index < sizeof(corrupt); ++index) {
            CHECK(crdv_dv_parser_feed(&parser, corrupt + index, 1u) == CRDV_OK);
        }
        CHECK(capture.count == 0u);
        CHECK(parser.rejected == 1u);
    }
    {
        int16_t pcm[160];
        for (size_t index = 0; index < 160u; ++index) {
            pcm[index] = (int16_t)((int)index * 257 - 20000);
        }
        CHECK(crdv_dv_build_encode(pcm, false, built, sizeof(built), &written) ==
              CRDV_OK);
        CHECK(written == 327u);
        CHECK(built[3] == 0x02u && built[4] == 0x40u && built[5] == 0x00u &&
              built[6] == 0xa0u);
        CHECK(built[7] == (uint8_t)((uint16_t)pcm[0] >> 8u));
    }
    {
        crdv_dv_transactions transactions;
        uint8_t channel_fields[11] = {0x01, 0x48};
        crdv_dv_packet_view view = {CRDV_DV_CHANNEL, channel_fields, 11u, false};
        crdv_dv_transactions_init(&transactions);
        CHECK(crdv_dv_transactions_submit(&transactions, CRDV_EXPECT_CHANNEL,
                                          0x01u, 100u) == CRDV_OK);
        CHECK(crdv_dv_transactions_accept(&transactions, &view, 99u) == CRDV_OK);
        CHECK(crdv_dv_transactions_submit(&transactions, CRDV_EXPECT_SPEECH,
                                          0x00u, 200u) == CRDV_OK);
        CHECK(crdv_dv_transactions_invalidate(&transactions) == 2u);
        CHECK(crdv_dv_transactions_accept(&transactions, &view, 150u) ==
              CRDV_E_STATE);
        CHECK(crdv_dv_transactions_submit(&transactions, CRDV_EXPECT_CHANNEL,
                                          0x01u, 200u) == CRDV_OK);
        CHECK(crdv_dv_transactions_expire(&transactions, 201u) == 1u);
        CHECK(transactions.timed_out == 1u);
    }
    {
        static const uint8_t product[] = {0x30u, 'A', 'M', 'B', 'E', '-',
                                          '3', '0', '0', '0', 'F', 0u};
        static const uint8_t wrong[] = {0x30u, 'N', 'O', 'T', 'A', 'M', 'B',
                                        'E', 0u};
        crdv_dv_packet_view good = {CRDV_DV_CONTROL, product, sizeof(product),
                                    true};
        crdv_dv_packet_view bad = {CRDV_DV_CONTROL, wrong, sizeof(wrong), true};
        CHECK(crdv_dv_product_is_ambe3000(&good) == CRDV_OK);
        CHECK(crdv_dv_product_is_ambe3000(&bad) == CRDV_E_CHECK);
    }
}

static void test_vita(void)
{
    crdv_vita_audio source;
    crdv_vita_audio parsed;
    uint8_t packet[1100];
    size_t written;
    memset(&source, 0, sizeof(source));
    source.count = 15u;
    source.stream_id = 0x12345678u;
    source.utc_seconds = 123456u;
    source.fractional_picoseconds = UINT64_C(999999000);
    for (size_t index = 0; index < 256u; ++index) {
        source.pairs[index] = (float)((double)index / 512.0 - 0.25);
    }
    CHECK(crdv_vita_build_audio(&source, packet, sizeof(packet), &written) ==
          CRDV_OK);
    CHECK(written == 1052u);
    CHECK(crdv_vita_parse_audio(packet, written, source.stream_id, &parsed) ==
          CRDV_OK);
    CHECK(memcmp(&source, &parsed, sizeof(source)) == 0);
    for (size_t length = 0; length < written; ++length) {
        CHECK(crdv_vita_parse_audio(packet, length, source.stream_id, &parsed) ==
              CRDV_E_FORMAT);
    }
    CHECK(crdv_vita_parse_audio(packet, written, 0x99999999u, &parsed) ==
          CRDV_E_STATE);
    packet[14] ^= 1u;
    CHECK(crdv_vita_parse_audio(packet, written, source.stream_id, &parsed) ==
          CRDV_E_FORMAT);
    packet[14] ^= 1u;
    packet[28] = 0x7fu;
    packet[29] = 0xc0u;
    packet[30] = 0x00u;
    packet[31] = 0x00u;
    CHECK(crdv_vita_parse_audio(packet, written, source.stream_id, &parsed) ==
          CRDV_E_FORMAT);

    {
        crdv_vita_counter counter = {0};
        crdv_vita_counter_push(&counter, 14u);
        crdv_vita_counter_push(&counter, 15u);
        crdv_vita_counter_push(&counter, 0u);
        CHECK(counter.gaps == 0u);
        crdv_vita_counter_push(&counter, 2u);
        CHECK(counter.gaps == 1u);
        crdv_vita_counter_push(&counter, 2u);
        CHECK(counter.duplicates == 1u);
        crdv_vita_counter_push(&counter, 1u);
        CHECK(counter.reordered == 1u);
    }
}

typedef struct {
    unsigned lines;
    char last[1001];
    size_t length;
} line_capture;

static void capture_line(void *context, const char *line, size_t length)
{
    line_capture *capture = context;
    capture->lines++;
    capture->length = length;
    memcpy(capture->last, line, length + 1u);
}

static void test_control(void)
{
    static const char response[] =
        "R42|00000000|tx_stream_in_id=0x1 rx_stream_in_id=0x2 "
        "tx_stream_out_id=0x3 rx_stream_out_id=0x4";
    static const char rx_metric[] =
        "AETHER_DV_METRIC v=2 mode=DSTR dir=RX rate_hz=24000.0 vita_gaps=0 "
        "source_blocks=188 turn_mean_us=12.5 turn_max_us=30 queue_max=2";
    static const char tx_metric[] =
        "AETHER_DV_METRIC v=3 mode=DSTR dir=TX rate_hz=24000 vita_gaps=0 "
        "null_frames=0 pcm_clips=0 pcm_invalid=0 send_failures=0 queue_max=1 "
        "tail_samples=240 tail_us=10000 preroll_frames=2 preroll_delay_ms=40 "
        "ambe_queue_max=2 ambe_underflows=0 ambe_overflows=0 "
        "ambe_sequence_errors=0 vocoder_submit_failures=0 vocoder_pending_max=1 "
        "drain_frames=2 drain_timeouts=0 drain_discarded_frames=0";
    crdv_control_response parsed;
    uint32_t streams[4] = {0};
    unsigned version;
    bool tx;

    CHECK(crdv_control_parse_response(response, strlen(response), &parsed) ==
          CRDV_OK);
    CHECK(parsed.sequence == 42u && parsed.status == 0u);
    CHECK(crdv_control_parse_create_streams(parsed.body, parsed.body_length,
                                            streams) == CRDV_OK);
    CHECK(streams[0] == 1u && streams[1] == 2u && streams[2] == 3u &&
          streams[3] == 4u);
    {
        crdv_control_transactions transactions;
        crdv_control_response success = {42u, 0u, NULL, 0u};
        crdv_control_response error = {43u, 1u, NULL, 0u};
        crdv_control_transactions_init(&transactions);
        CHECK(crdv_control_transactions_submit(&transactions, 42u, 100u) ==
              CRDV_OK);
        CHECK(crdv_control_transactions_submit(&transactions, 43u, 100u) ==
              CRDV_OK);
        CHECK(crdv_control_transactions_accept(&transactions, &error, 50u) ==
              CRDV_E_CHECK);
        CHECK(crdv_control_transactions_accept(&transactions, &success, 50u) ==
              CRDV_OK);
        CHECK(crdv_control_transactions_accept(&transactions, &success, 50u) ==
              CRDV_E_STATE);
        CHECK(crdv_control_transactions_submit(&transactions, 44u, 100u) ==
              CRDV_OK);
        CHECK(crdv_control_transactions_expire(&transactions, 101u) == 1u);
        CHECK(crdv_control_transactions_invalidate(&transactions) == 2u);
    }
    CHECK(crdv_metric_validate(rx_metric, strlen(rx_metric), &version, &tx) ==
          CRDV_OK);
    CHECK(version == 2u && !tx);
    CHECK(crdv_metric_validate(tx_metric, strlen(tx_metric), &version, &tx) ==
          CRDV_OK);
    CHECK(version == 3u && tx);
    {
        const char *bad[] = {
            "AETHER_DV_METRIC v=2 mode=DSTR dir=RX rate_hz=nan vita_gaps=0 source_blocks=1 turn_mean_us=1 turn_max_us=1 queue_max=1",
            "AETHER_DV_METRIC v=2 mode=DSTR dir=RX rate_hz=1 vita_gaps=-1 source_blocks=1 turn_mean_us=1 turn_max_us=1 queue_max=1",
            "AETHER_DV_METRIC v=2 mode=DSTR dir=RX rate_hz=1 vita_gaps=0 vita_gaps=0 source_blocks=1 turn_mean_us=1 turn_max_us=1 queue_max=1",
            "AETHER_DV_METRIC v=2 mode=DSTR dir=RX rate_hz=1 vita_gaps=18446744073709551616 source_blocks=1 turn_mean_us=1 turn_max_us=1 queue_max=1"};
        for (size_t index = 0; index < sizeof(bad) / sizeof(bad[0]); ++index) {
            CHECK(crdv_metric_validate(bad[index], strlen(bad[index]), &version,
                                       &tx) == CRDV_E_FORMAT);
        }
    }
    for (size_t split = 0; split <= strlen(response); ++split) {
        crdv_line_reader reader;
        line_capture capture = {0};
        char with_newline[sizeof(response) + 1u];
        memcpy(with_newline, response, sizeof(response) - 1u);
        with_newline[sizeof(response) - 1u] = '\n';
        crdv_line_reader_init(&reader, capture_line, &capture);
        CHECK(crdv_line_reader_feed(&reader, (const uint8_t *)with_newline,
                                    split) == CRDV_OK);
        CHECK(crdv_line_reader_feed(&reader,
                                    (const uint8_t *)with_newline + split,
                                    sizeof(response) - split) == CRDV_OK);
        CHECK(capture.lines == 1u);
        CHECK(capture.length == strlen(response));
        CHECK(memcmp(capture.last, response, strlen(response)) == 0);
    }
}

static void test_modem(void)
{
    uint8_t bits[40];
    uint8_t recovered[40];
    float taps[21];
    float samples[200];
    size_t written;
    double sum = 0.0;
    crdv_modulator_config config;

    CHECK(crdv_gaussian_tap_count(5u, 4u) == 21u);
    CHECK(crdv_gaussian_taps(0.5, 5u, 4u, taps, 21u) == CRDV_OK);
    for (size_t index = 0; index < 21u; ++index) {
        CHECK(isfinite(taps[index]));
        CHECK(taps[index] >= 0.0f);
        sum += taps[index];
    }
    CHECK(fabs(sum - 1.0) < 1e-6);
    for (size_t index = 0; index < 40u; ++index) {
        bits[index] = (uint8_t)(((index / 5u) & 1u) != 0u);
    }
    config.frequency_taps = taps;
    config.tap_count = 21u;
    config.samples_per_bit = 5u;
    config.gain = 2.0f;
    config.invert = false;
    CHECK(crdv_modulate_discriminator(bits, 40u, &config, samples, 200u,
                                      &written) == CRDV_OK);
    CHECK(written == 200u);
    for (size_t index = 0; index < written; ++index) {
        CHECK(isfinite(samples[index]));
        CHECK(fabsf(samples[index]) <= 0.980001f);
    }
    CHECK(crdv_demodulate_discriminator(samples, written, 5u, false, recovered,
                                        40u, &written) == CRDV_OK);
    CHECK(written == 40u);
    CHECK(memcmp(bits, recovered, 40u) == 0);
    samples[10] = NAN;
    CHECK(crdv_demodulate_discriminator(samples, 200u, 5u, false, recovered,
                                        40u, &written) == CRDV_E_FORMAT);
}

typedef struct {
    unsigned headers;
    unsigned voices;
    unsigned syncs;
    unsigned ends;
    unsigned event_count;
    unsigned event_kinds[6];
    crdv_sync_event events[128];
    uint8_t last_ambe[CRDV_AMBE_BYTES];
} air_capture;

static void air_header(void *context, const crdv_header_fields *header)
{
    air_capture *capture = context;
    capture->headers++;
    CHECK(memcmp(header->mycall, "N0SPEC  ", 8u) == 0);
}

static void air_voice(void *context, const uint8_t ambe[9],
                      const uint8_t slow[3], bool sync)
{
    air_capture *capture = context;
    capture->voices++;
    capture->syncs += sync ? 1u : 0u;
    memcpy(capture->last_ambe, ambe, CRDV_AMBE_BYTES);
    if (sync) {
        CHECK(slow[0] == 0xaau && slow[1] == 0xb4u && slow[2] == 0x68u);
    }
}

static void air_end(void *context)
{
    air_capture *capture = context;
    capture->ends++;
}

static void air_sync_event(void *context, const crdv_sync_event *event)
{
    air_capture *capture = context;
    CHECK(event != NULL);
    if (event == NULL) {
        return;
    }
    CHECK((unsigned)event->kind < 6u);
    if ((unsigned)event->kind < 6u) {
        capture->event_kinds[(unsigned)event->kind]++;
    }
    if (capture->event_count < 128u) {
        capture->events[capture->event_count] = *event;
    }
    capture->event_count++;
}

static crdv_air_callbacks air_callbacks_for(air_capture *capture)
{
    crdv_air_callbacks callbacks = {
        .header = air_header,
        .voice = air_voice,
        .end = air_end,
        .context = capture,
        .sync_event = air_sync_event,
    };
    return callbacks;
}

static void make_voice_frame(uint8_t marker, bool sync, unsigned errors,
                             uint8_t frame[CRDV_VOICE_FRAME_BITS])
{
    static const char data_sync[] = "101010101011010001101000";
    uint8_t ambe[CRDV_AMBE_BYTES] = {0};
    uint8_t slow[CRDV_SLOW_FRAGMENT_BYTES] = {0};
    ambe[0] = marker;
    CHECK(errors <= CRDV_DATA_SYNC_BITS);
    CHECK(crdv_voice_frame_pack(ambe, slow, frame) == CRDV_OK);
    if (sync) {
        for (size_t index = 0; index < CRDV_DATA_SYNC_BITS; ++index) {
            frame[72u + index] = (uint8_t)(data_sync[index] == '1');
        }
        for (size_t index = 0; index < errors; ++index) {
            frame[72u + index] ^= 1u;
        }
    }
}

static void prepare_voice_receiver(crdv_air_receiver *receiver,
                                   air_capture *capture,
                                   const crdv_receive_sync_policy *policy)
{
    crdv_air_callbacks callbacks = air_callbacks_for(capture);
    crdv_air_receiver_init(receiver, &callbacks);
    if (policy != NULL) {
        CHECK(crdv_air_receiver_set_sync_policy(receiver, policy) == CRDV_OK);
    }
    receiver->phase = CRDV_RX_VOICE;
    receiver->ended = false;
    receiver->frame_position = 0u;
}

static void test_air_receiver(void)
{
    static const char frame_sync[] = "111011001010000";
    crdv_header_fields fields = vector_fields();
    uint8_t header[41];
    uint8_t protected_bytes[83];
    uint8_t ambe[9] = {1u};
    uint8_t voice[96];
    uint8_t last[48];
    uint8_t stream[16u + 15u + 660u + 96u + 48u + 3u];
    size_t used = 0u;
    crdv_air_receiver receiver;
    air_capture capture = {0};
    crdv_air_callbacks callbacks = air_callbacks_for(&capture);

    stream[used++] = 0u;
    stream[used++] = 1u;
    stream[used++] = 0u;
    for (size_t index = 0; index < 16u; ++index) {
        stream[used++] = (uint8_t)((index & 1u) == 0u ? 1u : 0u);
    }
    for (size_t index = 0; index < 15u; ++index) {
        stream[used++] = (uint8_t)(frame_sync[index] == '1');
    }
    CHECK(crdv_header_pack(&fields, header) == CRDV_OK);
    CHECK(crdv_header_protect(header, protected_bytes) == CRDV_OK);
    for (size_t index = 0; index < 660u; ++index) {
        stream[used++] = packed_bit(protected_bytes, index);
    }
    make_voice_frame(ambe[0], true, 0u, voice);
    memcpy(stream + used, voice, sizeof(voice));
    used += sizeof(voice);
    crdv_last_frame_bits(last);
    memcpy(stream + used, last, sizeof(last));
    used += sizeof(last);
    crdv_air_receiver_init(&receiver, &callbacks);
    for (size_t split = 0; split < used; ++split) {
        CHECK(crdv_air_receiver_push(&receiver, stream + split, 1u) == CRDV_OK);
    }
    CHECK(capture.headers == 1u);
    CHECK(capture.voices == 1u);
    CHECK(capture.syncs == 1u);
    CHECK(capture.ends == 1u);
    CHECK(capture.last_ambe[0] == 0x01u);
    CHECK(capture.event_kinds[CRDV_SYNC_EXACT] == 1u);
    CHECK(receiver.sync_counters.exact_syncs == 1u);

    for (size_t errors = 0u; errors <= 3u; ++errors) {
        uint8_t acquisition[16u + 15u + 660u];
        size_t acquisition_used = 0u;
        for (size_t index = 0u; index < 16u; ++index) {
            acquisition[acquisition_used++] =
                (uint8_t)((index & 1u) == 0u ? 1u : 0u);
        }
        for (size_t index = 0u; index < errors; ++index) {
            acquisition[index] ^= 1u;
        }
        for (size_t index = 0u; index < 15u; ++index) {
            acquisition[acquisition_used++] =
                (uint8_t)(frame_sync[index] == '1');
        }
        for (size_t index = 0u; index < 660u; ++index) {
            acquisition[acquisition_used++] = packed_bit(protected_bytes, index);
        }

        memset(&capture, 0, sizeof(capture));
        crdv_air_receiver_init(&receiver, &callbacks);
        CHECK(crdv_air_receiver_push(&receiver, acquisition,
                                     acquisition_used) == CRDV_OK);
        CHECK(capture.headers == (errors <= 2u ? 1u : 0u));
    }
}

static void test_sync_policy_bounds(void)
{
    crdv_air_receiver receiver;
    crdv_sync_counters counters;
    crdv_receive_sync_policy policy = {0};
    crdv_air_receiver_init(&receiver, NULL);
    CHECK(crdv_air_receiver_set_sync_policy(NULL, &policy) == CRDV_E_ARGUMENT);
    CHECK(crdv_air_receiver_set_sync_policy(&receiver, NULL) == CRDV_E_ARGUMENT);
    CHECK(crdv_air_receiver_get_sync_counters(NULL, &counters) ==
          CRDV_E_ARGUMENT);
    CHECK(crdv_air_receiver_get_sync_counters(&receiver, NULL) ==
          CRDV_E_ARGUMENT);
    CHECK(crdv_air_receiver_set_sync_policy(&receiver, &policy) == CRDV_OK);
    CHECK(crdv_air_receiver_get_sync_counters(&receiver, &counters) == CRDV_OK);
    CHECK(memcmp(&counters, &(crdv_sync_counters){0}, sizeof(counters)) == 0);

    policy.max_hamming_distance = 25u;
    CHECK(crdv_air_receiver_set_sync_policy(&receiver, &policy) == CRDV_E_RANGE);
    policy.max_hamming_distance = 24u;
    policy.max_realign_bits = 1u;
    CHECK(crdv_air_receiver_set_sync_policy(&receiver, &policy) == CRDV_E_RANGE);
    policy.sliding_reacquisition = true;
    policy.max_realign_bits = 0u;
    CHECK(crdv_air_receiver_set_sync_policy(&receiver, &policy) == CRDV_E_RANGE);
    policy.max_realign_bits = 25u;
    CHECK(crdv_air_receiver_set_sync_policy(&receiver, &policy) == CRDV_E_RANGE);
    policy.max_realign_bits = CRDV_MAX_SYNC_REALIGN_BITS;
    policy.consecutive_miss_limit = UINT8_MAX;
    CHECK(crdv_air_receiver_set_sync_policy(&receiver, &policy) == CRDV_OK);
    receiver.phase = CRDV_RX_VOICE;
    CHECK(crdv_air_receiver_set_sync_policy(&receiver, &policy) == CRDV_E_STATE);
}

static void test_sync_hamming_boundaries(void)
{
    uint8_t frame[CRDV_VOICE_FRAME_BITS];
    for (unsigned distance = 0u; distance <= CRDV_DATA_SYNC_BITS; ++distance) {
        crdv_air_receiver receiver;
        air_capture capture = {0};
        crdv_receive_sync_policy policy = {
            .max_hamming_distance = (uint8_t)distance,
            .consecutive_miss_limit = 0u,
            .sliding_reacquisition = false,
            .max_realign_bits = 0u,
        };
        prepare_voice_receiver(&receiver, &capture, &policy);
        make_voice_frame(0x31u, true, distance, frame);
        CHECK(crdv_air_receiver_push(&receiver, frame, sizeof(frame)) == CRDV_OK);
        CHECK(capture.voices == 1u);
        CHECK(capture.syncs == 1u);
        CHECK(capture.event_count == 1u);
        CHECK(capture.events[0].hamming_distance == distance);
        CHECK(capture.events[0].kind ==
              (distance == 0u ? CRDV_SYNC_EXACT : CRDV_SYNC_TOLERANT));
        if (distance < CRDV_DATA_SYNC_BITS) {
            memset(&capture, 0, sizeof(capture));
            prepare_voice_receiver(&receiver, &capture, &policy);
            make_voice_frame(0x32u, true, distance + 1u, frame);
            CHECK(crdv_air_receiver_push(&receiver, frame, sizeof(frame)) ==
                  CRDV_OK);
            CHECK(capture.voices == 1u);
            CHECK(capture.syncs == 0u);
            CHECK(capture.event_kinds[CRDV_SYNC_EXPECTED_MISS] == 1u);
            CHECK(receiver.sync_counters.rejected_candidates == 1u);
            CHECK(receiver.sync_counters.expected_sync_misses == 1u);
        }
    }
}

static void test_sync_exhaustive_two_bit_policy(void)
{
    uint8_t frame[CRDV_VOICE_FRAME_BITS];
    crdv_receive_sync_policy policy = {0};

    for (unsigned first = 0u; first < CRDV_DATA_SYNC_BITS; ++first) {
        crdv_air_receiver receiver;
        air_capture capture = {0};
        prepare_voice_receiver(&receiver, &capture, &policy);
        make_voice_frame(0x71u, true, 0u, frame);
        frame[72u + first] ^= 1u;
        CHECK(crdv_air_receiver_push(&receiver, frame, sizeof(frame)) == CRDV_OK);
        CHECK(capture.syncs == 0u);
        CHECK(capture.event_kinds[CRDV_SYNC_EXPECTED_MISS] == 1u);
    }

    policy.max_hamming_distance = 1u;
    for (unsigned first = 0u; first < CRDV_DATA_SYNC_BITS; ++first) {
        crdv_air_receiver receiver;
        air_capture capture = {0};
        prepare_voice_receiver(&receiver, &capture, &policy);
        make_voice_frame(0x72u, true, 0u, frame);
        frame[72u + first] ^= 1u;
        CHECK(crdv_air_receiver_push(&receiver, frame, sizeof(frame)) == CRDV_OK);
        CHECK(capture.syncs == 1u);
        CHECK(capture.event_kinds[CRDV_SYNC_TOLERANT] == 1u);
    }
    for (unsigned first = 0u; first < CRDV_DATA_SYNC_BITS; ++first) {
        for (unsigned second = first + 1u; second < CRDV_DATA_SYNC_BITS;
             ++second) {
            crdv_air_receiver receiver;
            air_capture capture = {0};
            prepare_voice_receiver(&receiver, &capture, &policy);
            make_voice_frame(0x73u, true, 0u, frame);
            frame[72u + first] ^= 1u;
            frame[72u + second] ^= 1u;
            CHECK(crdv_air_receiver_push(&receiver, frame, sizeof(frame)) ==
                  CRDV_OK);
            CHECK(capture.syncs == 0u);
            CHECK(capture.event_kinds[CRDV_SYNC_EXPECTED_MISS] == 1u);
            CHECK(receiver.sync_counters.rejected_candidates == 1u);
        }
    }

    policy.max_hamming_distance = 2u;
    for (unsigned first = 0u; first < CRDV_DATA_SYNC_BITS; ++first) {
        for (unsigned second = first + 1u; second < CRDV_DATA_SYNC_BITS;
             ++second) {
            crdv_air_receiver receiver;
            air_capture capture = {0};
            prepare_voice_receiver(&receiver, &capture, &policy);
            make_voice_frame(0x74u, true, 0u, frame);
            frame[72u + first] ^= 1u;
            frame[72u + second] ^= 1u;
            CHECK(crdv_air_receiver_push(&receiver, frame, sizeof(frame)) ==
                  CRDV_OK);
            CHECK(capture.syncs == 1u);
            CHECK(capture.event_kinds[CRDV_SYNC_TOLERANT] == 1u);
        }
    }
    for (unsigned first = 0u; first < CRDV_DATA_SYNC_BITS; ++first) {
        for (unsigned second = first + 1u; second < CRDV_DATA_SYNC_BITS;
             ++second) {
            for (unsigned third = second + 1u; third < CRDV_DATA_SYNC_BITS;
                 ++third) {
                crdv_air_receiver receiver;
                air_capture capture = {0};
                prepare_voice_receiver(&receiver, &capture, &policy);
                make_voice_frame(0x75u, true, 0u, frame);
                frame[72u + first] ^= 1u;
                frame[72u + second] ^= 1u;
                frame[72u + third] ^= 1u;
                CHECK(crdv_air_receiver_push(&receiver, frame, sizeof(frame)) ==
                      CRDV_OK);
                CHECK(capture.syncs == 0u);
                CHECK(capture.event_kinds[CRDV_SYNC_EXPECTED_MISS] == 1u);
                CHECK(receiver.sync_counters.rejected_candidates == 1u);
            }
        }
    }
}

static void test_sync_sliding_reacquisition(void)
{
    uint8_t sync_frame[CRDV_VOICE_FRAME_BITS];
    uint8_t ordinary[CRDV_VOICE_FRAME_BITS];
    uint8_t shifted[CRDV_VOICE_FRAME_BITS + CRDV_MAX_SYNC_REALIGN_BITS];
    crdv_receive_sync_policy policy = {
        .max_hamming_distance = 0u,
        .consecutive_miss_limit = 0u,
        .sliding_reacquisition = true,
        .max_realign_bits = CRDV_MAX_SYNC_REALIGN_BITS,
    };
    make_voice_frame(0x40u, true, 0u, sync_frame);
    make_voice_frame(0xa5u, false, 0u, ordinary);

    for (unsigned shift = 1u; shift <= CRDV_MAX_SYNC_REALIGN_BITS; ++shift) {
        crdv_air_receiver receiver;
        air_capture capture = {0};
        prepare_voice_receiver(&receiver, &capture, &policy);
        CHECK(crdv_air_receiver_push(&receiver, sync_frame + shift,
                                     CRDV_VOICE_FRAME_BITS - shift) == CRDV_OK);
        CHECK(capture.event_kinds[CRDV_SYNC_REACQUIRED_EARLY] == 1u);
        CHECK(capture.events[0].bit_offset == -(int)shift);
        CHECK(capture.events[0].hamming_distance == 0u);
        CHECK(capture.voices == 0u);
        CHECK(receiver.sync_counters.reacquired_frame_drops == 1u);
        CHECK(receiver.frame_position == 1u && receiver.frame_fill == 0u);
        CHECK(crdv_air_receiver_push(&receiver, ordinary, sizeof(ordinary)) ==
              CRDV_OK);
        CHECK(capture.voices == 1u);
        CHECK(capture.last_ambe[0] == 0xa5u);

        memset(&capture, 0, sizeof(capture));
        prepare_voice_receiver(&receiver, &capture, &policy);
        memset(shifted, 0, shift);
        memcpy(shifted + shift, sync_frame, sizeof(sync_frame));
        CHECK(crdv_air_receiver_push(&receiver, shifted,
                                     CRDV_VOICE_FRAME_BITS + shift) == CRDV_OK);
        CHECK(capture.event_kinds[CRDV_SYNC_REACQUIRED_LATE] == 1u);
        CHECK(capture.events[0].bit_offset == (int)shift);
        CHECK(capture.events[0].hamming_distance == 0u);
        CHECK(capture.voices == 0u);
        CHECK(receiver.sync_counters.reacquired_frame_drops == 1u);
        CHECK(receiver.frame_position == 1u && receiver.frame_fill == 0u);
        CHECK(crdv_air_receiver_push(&receiver, ordinary, sizeof(ordinary)) ==
              CRDV_OK);
        CHECK(capture.voices == 1u);
        CHECK(capture.last_ambe[0] == 0xa5u);
    }

    {
        crdv_air_receiver receiver;
        air_capture capture = {0};
        crdv_receive_sync_policy tolerant = {
            .max_hamming_distance = 2u,
            .consecutive_miss_limit = 0u,
            .sliding_reacquisition = true,
            .max_realign_bits = 4u,
        };
        make_voice_frame(0x44u, true, 2u, sync_frame);
        prepare_voice_receiver(&receiver, &capture, &tolerant);
        CHECK(crdv_air_receiver_push(&receiver, sync_frame + 3u,
                                     CRDV_VOICE_FRAME_BITS - 3u) == CRDV_OK);
        CHECK(capture.event_kinds[CRDV_SYNC_REACQUIRED_EARLY] == 1u);
        CHECK(capture.events[0].bit_offset == -3);
        CHECK(capture.events[0].hamming_distance == 2u);
        CHECK(receiver.sync_counters.tolerant_syncs == 0u);
    }
}

static void test_sync_false_candidate_and_miss_limit(void)
{
    uint8_t bad_sync[CRDV_VOICE_FRAME_BITS];
    uint8_t ordinary[CRDV_VOICE_FRAME_BITS];
    uint8_t with_lookahead[CRDV_VOICE_FRAME_BITS + 2u];
    crdv_air_receiver receiver;
    air_capture capture = {0};
    crdv_receive_sync_policy sliding = {
        .max_hamming_distance = 1u,
        .consecutive_miss_limit = 0u,
        .sliding_reacquisition = true,
        .max_realign_bits = 2u,
    };
    make_voice_frame(0x51u, true, 2u, bad_sync);
    make_voice_frame(0x5au, false, 0u, ordinary);
    memcpy(with_lookahead, bad_sync, sizeof(bad_sync));
    memcpy(with_lookahead + sizeof(bad_sync), ordinary, 2u);
    prepare_voice_receiver(&receiver, &capture, &sliding);
    CHECK(crdv_air_receiver_push(&receiver, with_lookahead,
                                 sizeof(with_lookahead)) == CRDV_OK);
    CHECK(capture.event_kinds[CRDV_SYNC_EXPECTED_MISS] == 1u);
    CHECK(capture.event_kinds[CRDV_SYNC_REACQUIRED_EARLY] == 0u);
    CHECK(capture.event_kinds[CRDV_SYNC_REACQUIRED_LATE] == 0u);
    CHECK(receiver.sync_counters.rejected_candidates >= 1u);
    CHECK(capture.voices == 1u && capture.syncs == 0u);
    CHECK(receiver.frame_fill == 2u);
    CHECK(crdv_air_receiver_push(&receiver, ordinary + 2u,
                                 sizeof(ordinary) - 2u) == CRDV_OK);
    CHECK(capture.voices == 2u);
    CHECK(capture.last_ambe[0] == 0x5au);

    {
        crdv_receive_sync_policy loss = {
            .max_hamming_distance = 0u,
            .consecutive_miss_limit = 2u,
            .sliding_reacquisition = false,
            .max_realign_bits = 0u,
        };
        uint8_t zeros[200] = {0};
        memset(&capture, 0, sizeof(capture));
        prepare_voice_receiver(&receiver, &capture, &loss);
        make_voice_frame(0x61u, false, 0u, ordinary);
        CHECK(crdv_air_receiver_push(&receiver, ordinary, sizeof(ordinary)) ==
              CRDV_OK);
        CHECK(capture.event_kinds[CRDV_SYNC_EXPECTED_MISS] == 1u);
        CHECK(capture.ends == 0u && receiver.phase == CRDV_RX_VOICE);
        for (unsigned frame = 0u; frame < 20u; ++frame) {
            CHECK(crdv_air_receiver_push(&receiver, ordinary,
                                         sizeof(ordinary)) == CRDV_OK);
        }
        CHECK(receiver.frame_position == 0u);
        CHECK(crdv_air_receiver_push(&receiver, ordinary, sizeof(ordinary)) ==
              CRDV_OK);
        CHECK(capture.event_kinds[CRDV_SYNC_EXPECTED_MISS] == 2u);
        CHECK(capture.event_kinds[CRDV_SYNC_LOCK_LOST] == 1u);
        CHECK(capture.ends == 1u);
        CHECK(receiver.phase == CRDV_RX_SEARCH);
        CHECK(receiver.sync_counters.lock_losses == 1u);
        CHECK(capture.voices == 21u);
        CHECK(crdv_air_receiver_push(&receiver, zeros, sizeof(zeros)) == CRDV_OK);
        CHECK(capture.ends == 1u);
        CHECK(capture.event_kinds[CRDV_SYNC_LOCK_LOST] == 1u);
    }
}

static uint32_t random_state = UINT32_C(0x1a2b3c4d);
static uint32_t next_random(void)
{
    random_state ^= random_state << 13u;
    random_state ^= random_state >> 17u;
    random_state ^= random_state << 5u;
    return random_state;
}

static void test_hostile_inputs(void)
{
    crdv_dv_parser parser;
    crdv_line_reader reader;
    packet_capture packets = {0};
    line_capture lines = {0};
    uint8_t byte;
    uint8_t random_packet[1100];
    crdv_vita_audio audio;

    crdv_dv_parser_init(&parser, true, capture_packet, &packets);
    crdv_line_reader_init(&reader, capture_line, &lines);
    for (size_t index = 0; index < 100000u; ++index) {
        byte = (uint8_t)next_random();
        CHECK(crdv_dv_parser_feed(&parser, &byte, 1u) == CRDV_OK);
        CHECK(crdv_line_reader_feed(&reader, &byte, 1u) == CRDV_OK);
    }
    for (size_t trial = 0; trial < 1000u; ++trial) {
        size_t length = (size_t)(next_random() % sizeof(random_packet));
        for (size_t index = 0; index < length; ++index) {
            random_packet[index] = (uint8_t)next_random();
        }
        CHECK(crdv_vita_parse_audio(random_packet, length, next_random(), &audio) !=
              CRDV_OK);
    }
}

int main(void)
{
    test_config();
    test_header_vector();
    test_short_message_miniheaders();
    test_transmission_and_header_repeat();
    test_dv3000();
    test_vita();
    test_control();
    test_modem();
    test_air_receiver();
    test_sync_policy_bounds();
    test_sync_hamming_boundaries();
    test_sync_exhaustive_two_bit_policy();
    test_sync_sliding_reacquisition();
    test_sync_false_candidate_and_miss_limit();
    test_hostile_inputs();
    printf("crdv_tests: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
