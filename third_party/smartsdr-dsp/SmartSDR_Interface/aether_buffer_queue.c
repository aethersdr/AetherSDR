/*
 * Bounded intrusive queue for waveform buffers.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "aether_buffer_queue.h"

#include <stdlib.h>
#include <string.h>

static BufferDescriptor unlink_head_locked(aether_buffer_queue* queue)
{
    if (queue->root == NULL || queue->root->next == queue->root) {
        return NULL;
    }
    BufferDescriptor descriptor = queue->root->next;
    descriptor->next->prev = descriptor->prev;
    descriptor->prev->next = descriptor->next;
    descriptor->next = NULL;
    descriptor->prev = NULL;
    if (queue->depth > 0U) {
        queue->depth--;
    }
    return descriptor;
}

BOOL aether_buffer_queue_init(aether_buffer_queue* queue, uint32 limit)
{
    if (queue == NULL || limit == 0U) {
        return FALSE;
    }
    memset(queue, 0, sizeof(*queue));
    if (pthread_rwlock_init(&queue->lock, NULL) != 0) {
        return FALSE;
    }
    queue->root = (BufferDescriptor)calloc(1U, sizeof(buffer_descriptor));
    if (queue->root == NULL) {
        pthread_rwlock_destroy(&queue->lock);
        return FALSE;
    }
    queue->root->next = queue->root;
    queue->root->prev = queue->root;
    queue->limit = limit;
    return TRUE;
}

void aether_buffer_queue_destroy(aether_buffer_queue* queue)
{
    if (queue == NULL || queue->root == NULL) {
        return;
    }
    for (;;) {
        BufferDescriptor descriptor = aether_buffer_queue_pop(queue);
        if (descriptor == NULL) {
            break;
        }
        hal_BufferRelease(&descriptor);
    }
    free(queue->root);
    queue->root = NULL;
    queue->depth = 0U;
    queue->limit = 0U;
    pthread_rwlock_destroy(&queue->lock);
}

BOOL aether_buffer_queue_push(aether_buffer_queue* queue,
                              BufferDescriptor descriptor,
                              BOOL* dropped_oldest)
{
    if (dropped_oldest != NULL) {
        *dropped_oldest = FALSE;
    }
    if (queue == NULL || queue->root == NULL || descriptor == NULL) {
        return FALSE;
    }

    BufferDescriptor dropped = NULL;
    pthread_rwlock_wrlock(&queue->lock);
    if (queue->depth >= queue->limit) {
        dropped = unlink_head_locked(queue);
        if (dropped != NULL) {
            queue->dropped++;
        }
    }
    const BOOL was_empty = queue->depth == 0U;
    descriptor->next = queue->root;
    descriptor->prev = queue->root->prev;
    queue->root->prev->next = descriptor;
    queue->root->prev = descriptor;
    queue->depth++;
    pthread_rwlock_unlock(&queue->lock);

    if (dropped != NULL) {
        if (dropped_oldest != NULL) {
            *dropped_oldest = TRUE;
        }
        hal_BufferRelease(&dropped);
    }
    return was_empty;
}

BufferDescriptor aether_buffer_queue_pop(aether_buffer_queue* queue)
{
    if (queue == NULL || queue->root == NULL) {
        return NULL;
    }
    pthread_rwlock_wrlock(&queue->lock);
    BufferDescriptor descriptor = unlink_head_locked(queue);
    pthread_rwlock_unlock(&queue->lock);
    return descriptor;
}

uint32 aether_buffer_queue_depth(aether_buffer_queue* queue)
{
    if (queue == NULL || queue->root == NULL) {
        return 0U;
    }
    pthread_rwlock_rdlock(&queue->lock);
    const uint32 depth = queue->depth;
    pthread_rwlock_unlock(&queue->lock);
    return depth;
}

uint64 aether_buffer_queue_dropped(aether_buffer_queue* queue)
{
    if (queue == NULL || queue->root == NULL) {
        return 0U;
    }
    pthread_rwlock_rdlock(&queue->lock);
    const uint64 dropped = queue->dropped;
    pthread_rwlock_unlock(&queue->lock);
    return dropped;
}
