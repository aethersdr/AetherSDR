/*
 * Bounded intrusive queue for waveform buffers.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AETHER_BUFFER_QUEUE_H_
#define AETHER_BUFFER_QUEUE_H_

#include <pthread.h>

#include "datatypes.h"
#include "hal_buffer.h"

typedef struct aether_buffer_queue
{
    pthread_rwlock_t lock;
    BufferDescriptor root;
    uint32 depth;
    uint32 limit;
    uint64 dropped;
} aether_buffer_queue;

BOOL aether_buffer_queue_init(aether_buffer_queue* queue, uint32 limit);
void aether_buffer_queue_destroy(aether_buffer_queue* queue);
BOOL aether_buffer_queue_push(aether_buffer_queue* queue,
                              BufferDescriptor descriptor,
                              BOOL* dropped_oldest);
BufferDescriptor aether_buffer_queue_pop(aether_buffer_queue* queue);
uint32 aether_buffer_queue_depth(aether_buffer_queue* queue);
uint64 aether_buffer_queue_dropped(aether_buffer_queue* queue);

#endif /* AETHER_BUFFER_QUEUE_H_ */
