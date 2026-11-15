/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "crdv.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void crdv_line_reader_init(crdv_line_reader *reader, crdv_line_callback callback,
                           void *context)
{
    if (reader != NULL) {
        memset(reader, 0, sizeof(*reader));
        reader->callback = callback;
        reader->context = context;
    }
}

crdv_result crdv_line_reader_feed(crdv_line_reader *reader,
                                  const uint8_t *bytes, size_t length)
{
    if (reader == NULL || (bytes == NULL && length != 0u)) {
        return CRDV_E_ARGUMENT;
    }
    for (size_t index = 0; index < length; ++index) {
        uint8_t byte = bytes[index];
        if (byte == (uint8_t)'\n') {
            if (reader->dropping) {
                reader->dropping = false;
                reader->used = 0u;
                continue;
            }
            if (reader->used != 0u && reader->line[reader->used - 1u] == '\r') {
                reader->used--;
            }
            reader->line[reader->used] = '\0';
            if (reader->callback != NULL) {
                reader->callback(reader->context, reader->line, reader->used);
            }
            reader->used = 0u;
        } else if (!reader->dropping) {
            if (reader->used == CRDV_CONTROL_MAX_LINE) {
                reader->rejected++;
                reader->dropping = true;
                reader->used = 0u;
            } else {
                reader->line[reader->used++] = (char)byte;
            }
        }
    }
    return CRDV_OK;
}

static bool parse_unsigned_decimal(const char *begin, const char *end,
                                   uint32_t *value)
{
    uint32_t current = 0u;
    if (begin == end) {
        return false;
    }
    for (const char *cursor = begin; cursor < end; ++cursor) {
        uint32_t digit;
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        digit = (uint32_t)(*cursor - '0');
        if (current > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        current = current * 10u + digit;
    }
    *value = current;
    return true;
}

static bool parse_unsigned_hex(const char *begin, const char *end,
                               uint32_t *value)
{
    uint32_t current = 0u;
    if (begin == end) {
        return false;
    }
    for (const char *cursor = begin; cursor < end; ++cursor) {
        uint32_t digit;
        if (*cursor >= '0' && *cursor <= '9') {
            digit = (uint32_t)(*cursor - '0');
        } else if (*cursor >= 'a' && *cursor <= 'f') {
            digit = (uint32_t)(*cursor - 'a' + 10);
        } else if (*cursor >= 'A' && *cursor <= 'F') {
            digit = (uint32_t)(*cursor - 'A' + 10);
        } else {
            return false;
        }
        if (current > (UINT32_MAX - digit) / 16u) {
            return false;
        }
        current = current * 16u + digit;
    }
    *value = current;
    return true;
}

crdv_result crdv_control_parse_response(const char *line, size_t length,
                                        crdv_control_response *out)
{
    const char *first;
    const char *second;
    crdv_control_response next;
    if (line == NULL || out == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (length < 4u || line[0] != 'R') {
        return CRDV_E_FORMAT;
    }
    first = memchr(line + 1u, '|', length - 1u);
    if (first == NULL) {
        return CRDV_E_FORMAT;
    }
    second = memchr(first + 1u, '|', (size_t)(line + length - first - 1u));
    if (second == NULL ||
        !parse_unsigned_decimal(line + 1u, first, &next.sequence) ||
        !parse_unsigned_hex(first + 1u, second, &next.status)) {
        return CRDV_E_FORMAT;
    }
    next.body = second + 1u;
    next.body_length = (size_t)(line + length - next.body);
    *out = next;
    return CRDV_OK;
}

static bool parse_u32_auto(const char *text, uint32_t *value)
{
    const char *begin = text;
    const char *end = text + strlen(text);
    if (end - begin > 2 && begin[0] == '0' &&
        (begin[1] == 'x' || begin[1] == 'X')) {
        return parse_unsigned_hex(begin + 2, end, value);
    }
    return parse_unsigned_decimal(begin, end, value);
}

static char *next_space_token(char **cursor)
{
    char *start;
    if (cursor == NULL || *cursor == NULL) {
        return NULL;
    }
    while (**cursor == ' ') {
        ++*cursor;
    }
    if (**cursor == '\0') {
        *cursor = NULL;
        return NULL;
    }
    start = *cursor;
    while (**cursor != '\0' && **cursor != ' ') {
        ++*cursor;
    }
    if (**cursor == ' ') {
        **cursor = '\0';
        ++*cursor;
    } else {
        *cursor = NULL;
    }
    return start;
}

crdv_result crdv_control_parse_create_streams(const char *body, size_t length,
                                              uint32_t streams[4])
{
    static const char *const names[4] = {
        "tx_stream_in_id", "rx_stream_in_id", "tx_stream_out_id",
        "rx_stream_out_id"};
    char copy[CRDV_CONTROL_MAX_LINE + 1u];
    bool seen[4] = {false, false, false, false};
    char *token;
    char *cursor;

    if (body == NULL || streams == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (length > CRDV_CONTROL_MAX_LINE) {
        return CRDV_E_RANGE;
    }
    memcpy(copy, body, length);
    copy[length] = '\0';
    cursor = copy;
    token = next_space_token(&cursor);
    while (token != NULL) {
        char *equals = strchr(token, '=');
        if (equals != NULL) {
            *equals = '\0';
            for (size_t index = 0; index < 4u; ++index) {
                if (strcmp(token, names[index]) == 0) {
                    if (seen[index] || !parse_u32_auto(equals + 1u, &streams[index]) ||
                        streams[index] == 0u) {
                        return CRDV_E_FORMAT;
                    }
                    seen[index] = true;
                }
            }
        }
        token = next_space_token(&cursor);
    }
    for (size_t index = 0; index < 4u; ++index) {
        if (!seen[index]) {
            return CRDV_E_FORMAT;
        }
        for (size_t prior = 0; prior < index; ++prior) {
            if (streams[index] == streams[prior]) {
                return CRDV_E_CHECK;
            }
        }
    }
    return CRDV_OK;
}

static bool valid_u64(const char *value)
{
    uint64_t current = 0u;
    if (*value == '\0') {
        return false;
    }
    while (*value != '\0') {
        uint64_t digit;
        if (*value < '0' || *value > '9') {
            return false;
        }
        digit = (uint64_t)(*value - '0');
        if (current > (UINT64_MAX - digit) / 10u) {
            return false;
        }
        current = current * 10u + digit;
        ++value;
    }
    return true;
}

static bool valid_decimal(const char *value)
{
    char *end;
    double number;
    if (*value == '\0' || *value == '-') {
        return false;
    }
    errno = 0;
    number = strtod(value, &end);
    return errno == 0 && *end == '\0' && isfinite(number) && number >= 0.0;
}

static int key_index(const char *key, const char *const *keys, size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(key, keys[index]) == 0) {
            return (int)index;
        }
    }
    return -1;
}

crdv_result crdv_metric_validate(const char *line, size_t length,
                                 unsigned *version, bool *is_tx)
{
    static const char *const rx_keys[] = {
        "v",          "mode",       "dir",          "rate_hz",
        "vita_gaps",  "source_blocks", "turn_mean_us", "turn_max_us",
        "queue_max"};
    static const char *const tx_keys[] = {
        "v", "mode", "dir", "rate_hz", "vita_gaps", "null_frames",
        "pcm_clips", "pcm_invalid", "send_failures", "queue_max",
        "tail_samples", "tail_us", "preroll_frames", "preroll_delay_ms",
        "ambe_queue_max", "ambe_underflows", "ambe_overflows",
        "ambe_sequence_errors", "vocoder_submit_failures",
        "vocoder_pending_max", "drain_frames", "drain_timeouts",
        "drain_discarded_frames"};
    const char prefix[] = "AETHER_DV_METRIC ";
    char copy[CRDV_CONTROL_MAX_LINE + 1u];
    char *tokens[24];
    size_t token_count = 0u;
    char *token;
    char *cursor;
    bool tx;
    const char *const *keys;
    size_t key_count;
    uint64_t seen = 0u;

    if (line == NULL || version == NULL || is_tx == NULL) {
        return CRDV_E_ARGUMENT;
    }
    if (length > CRDV_CONTROL_MAX_LINE || length < sizeof(prefix) - 1u ||
        memcmp(line, prefix, sizeof(prefix) - 1u) != 0) {
        return CRDV_E_FORMAT;
    }
    memcpy(copy, line + sizeof(prefix) - 1u,
           length - (sizeof(prefix) - 1u));
    copy[length - (sizeof(prefix) - 1u)] = '\0';
    cursor = copy;
    token = next_space_token(&cursor);
    while (token != NULL) {
        if (token_count == 24u) {
            return CRDV_E_FORMAT;
        }
        tokens[token_count++] = token;
        token = next_space_token(&cursor);
    }
    tx = false;
    for (size_t index = 0; index < token_count; ++index) {
        if (strcmp(tokens[index], "dir=TX") == 0) {
            tx = true;
        }
    }
    keys = tx ? tx_keys : rx_keys;
    key_count = tx ? sizeof(tx_keys) / sizeof(tx_keys[0])
                   : sizeof(rx_keys) / sizeof(rx_keys[0]);
    if (token_count != key_count) {
        return CRDV_E_FORMAT;
    }
    for (size_t token_index = 0; token_index < token_count; ++token_index) {
        char *equals = strchr(tokens[token_index], '=');
        int index;
        const char *value;
        bool special;
        if (equals == NULL || equals == tokens[token_index] || equals[1] == '\0') {
            return CRDV_E_FORMAT;
        }
        *equals = '\0';
        value = equals + 1u;
        index = key_index(tokens[token_index], keys, key_count);
        if (index < 0 || (seen & (UINT64_C(1) << (unsigned)index)) != 0u) {
            return CRDV_E_FORMAT;
        }
        seen |= UINT64_C(1) << (unsigned)index;
        special = strcmp(tokens[token_index], "v") == 0 ||
                  strcmp(tokens[token_index], "mode") == 0 ||
                  strcmp(tokens[token_index], "dir") == 0 ||
                  strcmp(tokens[token_index], "rate_hz") == 0 ||
                  strcmp(tokens[token_index], "turn_mean_us") == 0;
        if (strcmp(tokens[token_index], "v") == 0) {
            if (strcmp(value, tx ? "3" : "2") != 0) {
                return CRDV_E_FORMAT;
            }
        } else if (strcmp(tokens[token_index], "mode") == 0) {
            if (strcmp(value, "DSTR") != 0) {
                return CRDV_E_FORMAT;
            }
        } else if (strcmp(tokens[token_index], "dir") == 0) {
            if (strcmp(value, tx ? "TX" : "RX") != 0) {
                return CRDV_E_FORMAT;
            }
        } else if (strcmp(tokens[token_index], "rate_hz") == 0 ||
                   strcmp(tokens[token_index], "turn_mean_us") == 0) {
            if (!valid_decimal(value)) {
                return CRDV_E_FORMAT;
            }
        } else if (!special && !valid_u64(value)) {
            return CRDV_E_FORMAT;
        }
    }
    if (seen != ((UINT64_C(1) << key_count) - 1u)) {
        return CRDV_E_FORMAT;
    }
    *version = tx ? 3u : 2u;
    *is_tx = tx;
    return CRDV_OK;
}

void crdv_control_transactions_init(crdv_control_transactions *transactions)
{
    if (transactions != NULL) {
        memset(transactions, 0, sizeof(*transactions));
        transactions->generation = 1u;
    }
}

uint32_t crdv_control_transactions_invalidate(
    crdv_control_transactions *transactions)
{
    if (transactions == NULL) {
        return 0u;
    }
    transactions->discarded += transactions->count;
    memset(transactions->slots, 0, sizeof(transactions->slots));
    transactions->count = 0u;
    transactions->generation++;
    if (transactions->generation == 0u) {
        transactions->generation = 1u;
    }
    return transactions->generation;
}

crdv_result crdv_control_transactions_submit(
    crdv_control_transactions *transactions, uint32_t sequence,
    uint64_t deadline_ms)
{
    size_t free_slot = CRDV_CONTROL_PENDING_CAPACITY;
    if (transactions == NULL) {
        return CRDV_E_ARGUMENT;
    }
    for (size_t index = 0; index < CRDV_CONTROL_PENDING_CAPACITY; ++index) {
        if (transactions->slots[index].occupied &&
            transactions->slots[index].sequence == sequence) {
            return CRDV_E_STATE;
        }
        if (!transactions->slots[index].occupied &&
            free_slot == CRDV_CONTROL_PENDING_CAPACITY) {
            free_slot = index;
        }
    }
    if (free_slot == CRDV_CONTROL_PENDING_CAPACITY) {
        return CRDV_E_CAPACITY;
    }
    transactions->slots[free_slot].sequence = sequence;
    transactions->slots[free_slot].generation = transactions->generation;
    transactions->slots[free_slot].deadline_ms = deadline_ms;
    transactions->slots[free_slot].occupied = true;
    transactions->count++;
    return CRDV_OK;
}

crdv_result crdv_control_transactions_accept(
    crdv_control_transactions *transactions,
    const crdv_control_response *response, uint64_t now_ms)
{
    if (transactions == NULL || response == NULL) {
        return CRDV_E_ARGUMENT;
    }
    for (size_t index = 0; index < CRDV_CONTROL_PENDING_CAPACITY; ++index) {
        crdv_control_pending *pending = &transactions->slots[index];
        if (pending->occupied && pending->sequence == response->sequence) {
            pending->occupied = false;
            transactions->count--;
            if (pending->generation != transactions->generation) {
                transactions->discarded++;
                return CRDV_E_STATE;
            }
            if (now_ms > pending->deadline_ms) {
                transactions->timed_out++;
                return CRDV_E_TIMEOUT;
            }
            if (response->status != 0u) {
                return CRDV_E_CHECK;
            }
            return CRDV_OK;
        }
    }
    transactions->discarded++;
    return CRDV_E_STATE;
}

size_t crdv_control_transactions_expire(crdv_control_transactions *transactions,
                                        uint64_t now_ms)
{
    size_t expired = 0u;
    if (transactions == NULL) {
        return 0u;
    }
    for (size_t index = 0; index < CRDV_CONTROL_PENDING_CAPACITY; ++index) {
        crdv_control_pending *pending = &transactions->slots[index];
        if (pending->occupied && now_ms > pending->deadline_ms) {
            pending->occupied = false;
            transactions->count--;
            transactions->timed_out++;
            expired++;
        }
    }
    return expired;
}
