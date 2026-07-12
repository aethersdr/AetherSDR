/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CRDV_H
#define CRDV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRDV_HEADER_BYTES 41u
#define CRDV_PROTECTED_BITS 660u
#define CRDV_PROTECTED_PACKED_BYTES 83u
#define CRDV_AMBE_BYTES 9u
#define CRDV_PCM_SAMPLES 160u
#define CRDV_SLOW_FRAGMENT_BYTES 3u
#define CRDV_MESSAGE_BYTES 20u
#define CRDV_DV3000_MAX_PACKET 2048u
#define CRDV_CONTROL_MAX_LINE 1000u
#define CRDV_VITA_AUDIO_PAIRS 128u
#define CRDV_DATA_SYNC_BITS 24u
#define CRDV_VOICE_FRAME_BITS 96u
#define CRDV_MAX_SYNC_REALIGN_BITS 24u

typedef enum {
    CRDV_OK = 0,
    CRDV_E_ARGUMENT = -1,
    CRDV_E_RANGE = -2,
    CRDV_E_FORMAT = -3,
    CRDV_E_CHECK = -4,
    CRDV_E_CAPACITY = -5,
    CRDV_E_STATE = -6,
    CRDV_E_TIMEOUT = -7
} crdv_result;

typedef struct {
    uint8_t flags[3];
    char rpt2[8];
    char rpt1[8];
    char urcall[8];
    char mycall[8];
    char suffix[4];
} crdv_header_fields;

typedef struct {
    char mycall[9];
    char suffix[5];
    char urcall[9];
    char rpt1[9];
    char rpt2[9];
    char message[21];
} crdv_station_config;

crdv_result crdv_config_normalize(const char *mycall, const char *suffix,
                                  const char *urcall, const char *rpt1,
                                  const char *rpt2, const char *message,
                                  crdv_station_config *out);

uint16_t crdv_pfcs(const uint8_t *bytes, size_t length);
crdv_result crdv_header_pack(const crdv_header_fields *fields,
                             uint8_t out[CRDV_HEADER_BYTES]);
crdv_result crdv_header_unpack(const uint8_t in[CRDV_HEADER_BYTES],
                               crdv_header_fields *fields);
crdv_result crdv_header_protect(const uint8_t in[CRDV_HEADER_BYTES],
                                uint8_t out[CRDV_PROTECTED_PACKED_BYTES]);
crdv_result crdv_header_recover(const uint8_t in[CRDV_PROTECTED_PACKED_BYTES],
                                uint8_t out[CRDV_HEADER_BYTES],
                                unsigned *path_metric);

void crdv_slow_xor(uint8_t fragment[CRDV_SLOW_FRAGMENT_BYTES]);
/* block_number is one-based (1-4); wire mini-headers are 0x40-0x43. */
crdv_result crdv_message_block(const uint8_t message[CRDV_MESSAGE_BYTES],
                               unsigned block_number,
                               uint8_t first[CRDV_SLOW_FRAGMENT_BYTES],
                               uint8_t second[CRDV_SLOW_FRAGMENT_BYTES]);

typedef struct {
    uint8_t slots[4][5];
    uint8_t present_mask;
    uint32_t session;
    bool published;
    bool conflicted;
} crdv_message_assembler;

void crdv_message_assembler_reset(crdv_message_assembler *assembler,
                                  uint32_t session);
crdv_result crdv_message_assembler_push(crdv_message_assembler *assembler,
                                        uint32_t session,
                                        const uint8_t six_bytes[6],
                                        uint8_t complete[CRDV_MESSAGE_BYTES],
                                        bool *new_message);

crdv_result crdv_voice_frame_pack(const uint8_t ambe[CRDV_AMBE_BYTES],
                                  const uint8_t slow[CRDV_SLOW_FRAGMENT_BYTES],
                                  uint8_t bits[96]);
crdv_result crdv_transmission_prefix(const uint8_t header[CRDV_HEADER_BYTES],
                                     size_t bit_sync_count, uint8_t *bits,
                                     size_t capacity, size_t *written);
void crdv_last_frame_bits(uint8_t bits[48]);

crdv_result crdv_header_repeat_block(const uint8_t header[CRDV_HEADER_BYTES],
                                     unsigned block_index,
                                     uint8_t first[CRDV_SLOW_FRAGMENT_BYTES],
                                     uint8_t second[CRDV_SLOW_FRAGMENT_BYTES]);

typedef struct {
    uint8_t bytes[CRDV_HEADER_BYTES];
    size_t used;
    uint32_t session;
    bool complete;
    bool rejected;
} crdv_header_repeat_assembler;
void crdv_header_repeat_reset(crdv_header_repeat_assembler *assembler,
                              uint32_t session);
crdv_result crdv_header_repeat_push(crdv_header_repeat_assembler *assembler,
                                    uint32_t session,
                                    const uint8_t six_bytes[6],
                                    crdv_header_fields *header,
                                    bool *new_header);

typedef enum {
    CRDV_RX_SEARCH = 0,
    CRDV_RX_HEADER,
    CRDV_RX_VOICE
} crdv_rx_phase;

typedef struct {
    uint8_t max_hamming_distance;
    uint8_t consecutive_miss_limit;
    bool sliding_reacquisition;
    uint8_t max_realign_bits;
} crdv_receive_sync_policy;

typedef enum {
    CRDV_SYNC_EXACT = 0,
    CRDV_SYNC_TOLERANT,
    CRDV_SYNC_REACQUIRED_EARLY,
    CRDV_SYNC_REACQUIRED_LATE,
    CRDV_SYNC_EXPECTED_MISS,
    CRDV_SYNC_LOCK_LOST
} crdv_sync_event_kind;

typedef struct {
    crdv_sync_event_kind kind;
    uint8_t frame_position;
    int8_t bit_offset;
    uint8_t hamming_distance;
    uint8_t consecutive_misses;
} crdv_sync_event;

typedef struct {
    uint64_t exact_syncs;
    uint64_t tolerant_syncs;
    uint64_t early_reacquisitions;
    uint64_t late_reacquisitions;
    uint64_t rejected_candidates;
    uint64_t expected_sync_misses;
    uint64_t lock_losses;
    uint64_t reacquired_frame_drops;
} crdv_sync_counters;

typedef struct {
    void (*header)(void *context, const crdv_header_fields *header);
    void (*voice)(void *context, const uint8_t ambe[CRDV_AMBE_BYTES],
                  const uint8_t slow[CRDV_SLOW_FRAGMENT_BYTES], bool data_sync);
    void (*end)(void *context);
    void *context;
    void (*sync_event)(void *context, const crdv_sync_event *event);
} crdv_air_callbacks;

typedef struct {
    crdv_rx_phase phase;
    crdv_air_callbacks callbacks;
    uint8_t window[31];
    size_t window_fill;
    uint8_t protected_bits[CRDV_PROTECTED_BITS];
    size_t protected_fill;
    uint8_t frame_bits[CRDV_VOICE_FRAME_BITS + CRDV_MAX_SYNC_REALIGN_BITS];
    size_t frame_fill;
    uint8_t frame_position;
    uint8_t consecutive_sync_misses;
    uint8_t best_sync_distance;
    crdv_receive_sync_policy sync_policy;
    crdv_sync_counters sync_counters;
    bool ended;
} crdv_air_receiver;

void crdv_air_receiver_init(crdv_air_receiver *receiver,
                            const crdv_air_callbacks *callbacks);
crdv_result crdv_air_receiver_set_sync_policy(
    crdv_air_receiver *receiver, const crdv_receive_sync_policy *policy);
crdv_result crdv_air_receiver_get_sync_counters(
    const crdv_air_receiver *receiver, crdv_sync_counters *counters);
crdv_result crdv_air_receiver_push(crdv_air_receiver *receiver,
                                   const uint8_t *bits, size_t bit_count);
void crdv_air_receiver_cancel(crdv_air_receiver *receiver);

typedef struct {
    const float *frequency_taps;
    size_t tap_count;
    unsigned samples_per_bit;
    float gain;
    bool invert;
} crdv_modulator_config;

size_t crdv_gaussian_tap_count(unsigned samples_per_bit, unsigned symbol_span);
crdv_result crdv_gaussian_taps(double bt, unsigned samples_per_bit,
                               unsigned symbol_span, float *out, size_t capacity);
crdv_result crdv_modulate_discriminator(const uint8_t *bits, size_t bit_count,
                                        const crdv_modulator_config *config,
                                        float *samples, size_t capacity,
                                        size_t *written);
crdv_result crdv_demodulate_discriminator(const float *samples,
                                          size_t sample_count,
                                          unsigned samples_per_bit,
                                          bool invert, uint8_t *bits,
                                          size_t capacity, size_t *written);

typedef enum {
    CRDV_DV_CONTROL = 0,
    CRDV_DV_CHANNEL = 1,
    CRDV_DV_SPEECH = 2
} crdv_dv_packet_type;

typedef struct {
    crdv_dv_packet_type type;
    const uint8_t *fields;
    size_t field_length;
    bool had_parity;
} crdv_dv_packet_view;

typedef void (*crdv_dv_packet_callback)(void *context,
                                        const crdv_dv_packet_view *packet);

typedef struct {
    uint8_t bytes[CRDV_DV3000_MAX_PACKET];
    size_t used;
    size_t expected;
    bool parity_required;
    uint64_t rejected;
    crdv_dv_packet_callback callback;
    void *context;
} crdv_dv_parser;

void crdv_dv_parser_init(crdv_dv_parser *parser, bool parity_required,
                         crdv_dv_packet_callback callback, void *context);
void crdv_dv_parser_set_parity(crdv_dv_parser *parser, bool required);
crdv_result crdv_dv_parser_feed(crdv_dv_parser *parser, const uint8_t *bytes,
                                size_t length);
crdv_result crdv_dv_build_packet(crdv_dv_packet_type type,
                                 const uint8_t *fields, size_t field_length,
                                 bool parity, uint8_t *out, size_t capacity,
                                 size_t *written);
crdv_result crdv_dv_build_encode(const int16_t pcm[CRDV_PCM_SAMPLES],
                                 bool parity, uint8_t *out, size_t capacity,
                                 size_t *written);
crdv_result crdv_dv_build_decode(const uint8_t ambe[CRDV_AMBE_BYTES],
                                 bool parity, uint8_t *out, size_t capacity,
                                 size_t *written);
typedef enum {
    CRDV_DV_START_RESET = 0,
    CRDV_DV_START_PRODUCT,
    CRDV_DV_START_VERSION,
    CRDV_DV_START_DISABLE_PARITY,
    CRDV_DV_START_READ_CONFIG,
    CRDV_DV_START_CODEC,
    CRDV_DV_START_GAIN,
    CRDV_DV_START_COMPANDING,
    CRDV_DV_START_CHANNEL_FORMAT,
    CRDV_DV_START_DSTAR_RATE
} crdv_dv_startup_step;
crdv_result crdv_dv_build_startup(crdv_dv_startup_step step, uint8_t *out,
                                  size_t capacity, size_t *written);
crdv_result crdv_dv_product_is_ambe3000(const crdv_dv_packet_view *packet);
crdv_result crdv_dv_read_channel(const crdv_dv_packet_view *packet,
                                 uint8_t ambe[CRDV_AMBE_BYTES]);
crdv_result crdv_dv_read_speech(const crdv_dv_packet_view *packet,
                                int16_t pcm[CRDV_PCM_SAMPLES]);

#define CRDV_DV_PENDING_CAPACITY 16u
typedef enum {
    CRDV_EXPECT_CHANNEL = 1,
    CRDV_EXPECT_SPEECH = 2,
    CRDV_EXPECT_CONTROL = 3
} crdv_dv_expectation;

typedef struct {
    uint32_t generation;
    uint64_t deadline_ms;
    crdv_dv_expectation expected;
    uint8_t field;
    bool occupied;
} crdv_dv_pending;

typedef struct {
    crdv_dv_pending pending[CRDV_DV_PENDING_CAPACITY];
    size_t head;
    size_t count;
    uint32_t generation;
    uint64_t discarded;
    uint64_t timed_out;
} crdv_dv_transactions;

void crdv_dv_transactions_init(crdv_dv_transactions *transactions);
uint32_t crdv_dv_transactions_invalidate(crdv_dv_transactions *transactions);
crdv_result crdv_dv_transactions_submit(crdv_dv_transactions *transactions,
                                        crdv_dv_expectation expected,
                                        uint8_t field, uint64_t deadline_ms);
crdv_result crdv_dv_transactions_accept(crdv_dv_transactions *transactions,
                                        const crdv_dv_packet_view *packet,
                                        uint64_t now_ms);
size_t crdv_dv_transactions_expire(crdv_dv_transactions *transactions,
                                   uint64_t now_ms);

typedef struct {
    uint8_t count;
    uint32_t stream_id;
    uint32_t utc_seconds;
    uint64_t fractional_picoseconds;
    float pairs[CRDV_VITA_AUDIO_PAIRS * 2u];
} crdv_vita_audio;

crdv_result crdv_vita_parse_audio(const uint8_t *packet, size_t length,
                                  uint32_t expected_stream,
                                  crdv_vita_audio *out);
crdv_result crdv_vita_build_audio(const crdv_vita_audio *audio,
                                  uint8_t *packet, size_t capacity,
                                  size_t *written);

typedef struct {
    bool seen;
    uint8_t last;
    uint64_t gaps;
    uint64_t duplicates;
    uint64_t reordered;
} crdv_vita_counter;
void crdv_vita_counter_push(crdv_vita_counter *counter, uint8_t value);

typedef void (*crdv_line_callback)(void *context, const char *line,
                                   size_t length);
typedef struct {
    char line[CRDV_CONTROL_MAX_LINE + 1u];
    size_t used;
    bool dropping;
    uint64_t rejected;
    crdv_line_callback callback;
    void *context;
} crdv_line_reader;
void crdv_line_reader_init(crdv_line_reader *reader, crdv_line_callback callback,
                           void *context);
crdv_result crdv_line_reader_feed(crdv_line_reader *reader,
                                  const uint8_t *bytes, size_t length);

typedef struct {
    uint32_t sequence;
    uint32_t status;
    const char *body;
    size_t body_length;
} crdv_control_response;
crdv_result crdv_control_parse_response(const char *line, size_t length,
                                        crdv_control_response *out);
crdv_result crdv_control_parse_create_streams(const char *body, size_t length,
                                              uint32_t streams[4]);
crdv_result crdv_metric_validate(const char *line, size_t length,
                                 unsigned *version, bool *is_tx);

#define CRDV_CONTROL_PENDING_CAPACITY 16u
typedef struct {
    uint32_t sequence;
    uint32_t generation;
    uint64_t deadline_ms;
    bool occupied;
} crdv_control_pending;
typedef struct {
    crdv_control_pending slots[CRDV_CONTROL_PENDING_CAPACITY];
    uint32_t generation;
    size_t count;
    uint64_t discarded;
    uint64_t timed_out;
} crdv_control_transactions;
void crdv_control_transactions_init(crdv_control_transactions *transactions);
uint32_t crdv_control_transactions_invalidate(
    crdv_control_transactions *transactions);
crdv_result crdv_control_transactions_submit(
    crdv_control_transactions *transactions, uint32_t sequence,
    uint64_t deadline_ms);
crdv_result crdv_control_transactions_accept(
    crdv_control_transactions *transactions,
    const crdv_control_response *response, uint64_t now_ms);
size_t crdv_control_transactions_expire(crdv_control_transactions *transactions,
                                        uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
