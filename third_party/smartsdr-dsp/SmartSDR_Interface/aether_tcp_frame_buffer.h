/*
 * Bounded newline-delimited TCP frame accumulator.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AETHER_TCP_FRAME_BUFFER_H_
#define AETHER_TCP_FRAME_BUFFER_H_

#include <stddef.h>

typedef enum aether_tcp_frame_result
{
    AETHER_TCP_FRAME_NEED_DATA = 0,
    AETHER_TCP_FRAME_READY,
    AETHER_TCP_FRAME_TERMINATE,
    AETHER_TCP_FRAME_OVERFLOW
} aether_tcp_frame_result;

typedef struct aether_tcp_frame_buffer
{
    unsigned char* storage;
    size_t capacity;
    size_t length;
} aether_tcp_frame_buffer;

void aether_tcp_frame_buffer_init(aether_tcp_frame_buffer* buffer,
                                  void* storage,
                                  size_t capacity);
int aether_tcp_frame_buffer_append(aether_tcp_frame_buffer* buffer,
                                   const void* data,
                                   size_t data_size);
aether_tcp_frame_result aether_tcp_frame_buffer_next(
    aether_tcp_frame_buffer* buffer,
    char* destination,
    size_t destination_size,
    size_t* frame_size);

#endif /* AETHER_TCP_FRAME_BUFFER_H_ */
