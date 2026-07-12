/*
 * Bounded newline-delimited TCP frame accumulator.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "aether_tcp_frame_buffer.h"

#include <string.h>

static void consume(aether_tcp_frame_buffer* buffer, size_t count)
{
    if (count >= buffer->length) {
        buffer->length = 0U;
        return;
    }
    memmove(buffer->storage,
            buffer->storage + count,
            buffer->length - count);
    buffer->length -= count;
}

static int is_delimiter(unsigned char value)
{
    return value == '\r' || value == '\n' || value == '\0';
}

void aether_tcp_frame_buffer_init(aether_tcp_frame_buffer* buffer,
                                  void* storage,
                                  size_t capacity)
{
    if (buffer == NULL) {
        return;
    }
    buffer->storage = (unsigned char*)storage;
    buffer->capacity = storage != NULL ? capacity : 0U;
    buffer->length = 0U;
}

int aether_tcp_frame_buffer_append(aether_tcp_frame_buffer* buffer,
                                   const void* data,
                                   size_t data_size)
{
    if (buffer == NULL || buffer->storage == NULL
        || (data == NULL && data_size != 0U)
        || buffer->length > buffer->capacity
        || data_size > buffer->capacity - buffer->length) {
        return -1;
    }
    if (data_size > 0U) {
        memcpy(buffer->storage + buffer->length, data, data_size);
        buffer->length += data_size;
    }
    return 0;
}

aether_tcp_frame_result aether_tcp_frame_buffer_next(
    aether_tcp_frame_buffer* buffer,
    char* destination,
    size_t destination_size,
    size_t* frame_size)
{
    if (frame_size != NULL) {
        *frame_size = 0U;
    }
    if (buffer == NULL || buffer->storage == NULL
        || destination == NULL || destination_size == 0U) {
        return AETHER_TCP_FRAME_OVERFLOW;
    }

    for (;;) {
        while (buffer->length > 0U && is_delimiter(buffer->storage[0])) {
            consume(buffer, 1U);
        }

        if (buffer->length == 0U) {
            return AETHER_TCP_FRAME_NEED_DATA;
        }

        /* Ignore complete three-byte Telnet negotiation sequences. */
        if (buffer->storage[0] == 0xffU) {
            if (buffer->length < 3U) {
                return buffer->length == buffer->capacity
                    ? AETHER_TCP_FRAME_OVERFLOW
                    : AETHER_TCP_FRAME_NEED_DATA;
            }
            consume(buffer, 3U);
            continue;
        }
        break;
    }

    if (buffer->storage[0] == 3U || buffer->storage[0] == 4U
        || buffer->storage[0] == 26U) {
        consume(buffer, 1U);
        return AETHER_TCP_FRAME_TERMINATE;
    }

    size_t length = 0U;
    while (length < buffer->length && !is_delimiter(buffer->storage[length])) {
        length++;
    }
    if (length == buffer->length) {
        return buffer->length == buffer->capacity
            ? AETHER_TCP_FRAME_OVERFLOW
            : AETHER_TCP_FRAME_NEED_DATA;
    }
    if (length + 1U > destination_size) {
        return AETHER_TCP_FRAME_OVERFLOW;
    }

    memcpy(destination, buffer->storage, length);
    destination[length] = '\0';
    consume(buffer, length + 1U);
    if (frame_size != NULL) {
        *frame_size = length;
    }
    return AETHER_TCP_FRAME_READY;
}
