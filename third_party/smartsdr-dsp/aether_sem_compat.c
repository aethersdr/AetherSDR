/*
 * Minimal unnamed POSIX semaphore compatibility for macOS.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef __APPLE__

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>

typedef struct AetherSemaphore {
    int inUse;
    unsigned int value;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} AetherSemaphore;

static pthread_mutex_t registryMutex = PTHREAD_MUTEX_INITIALIZER;
static AetherSemaphore registry[128];

static AetherSemaphore* lookupSemaphore(sem_t* sem)
{
    if (sem == NULL || *sem <= 0 || *sem > (int)(sizeof(registry) / sizeof(registry[0]))) {
        errno = EINVAL;
        return NULL;
    }

    AetherSemaphore* entry = &registry[*sem - 1];
    if (!entry->inUse) {
        errno = EINVAL;
        return NULL;
    }
    return entry;
}

int sem_init(sem_t* sem, int pshared, unsigned int value)
{
    if (sem == NULL || pshared != 0) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&registryMutex);
    for (int i = 0; i < (int)(sizeof(registry) / sizeof(registry[0])); ++i) {
        if (registry[i].inUse) {
            continue;
        }

        registry[i].inUse = 1;
        registry[i].value = value;
        pthread_mutex_init(&registry[i].mutex, NULL);
        pthread_cond_init(&registry[i].cond, NULL);
        *sem = i + 1;
        pthread_mutex_unlock(&registryMutex);
        return 0;
    }
    pthread_mutex_unlock(&registryMutex);

    errno = ENOSPC;
    return -1;
}

int sem_destroy(sem_t* sem)
{
    pthread_mutex_lock(&registryMutex);
    AetherSemaphore* entry = lookupSemaphore(sem);
    if (entry == NULL) {
        pthread_mutex_unlock(&registryMutex);
        return -1;
    }

    pthread_mutex_destroy(&entry->mutex);
    pthread_cond_destroy(&entry->cond);
    entry->inUse = 0;
    *sem = 0;
    pthread_mutex_unlock(&registryMutex);
    return 0;
}

int sem_post(sem_t* sem)
{
    AetherSemaphore* entry = lookupSemaphore(sem);
    if (entry == NULL) {
        return -1;
    }

    pthread_mutex_lock(&entry->mutex);
    ++entry->value;
    pthread_cond_signal(&entry->cond);
    pthread_mutex_unlock(&entry->mutex);
    return 0;
}

int sem_wait(sem_t* sem)
{
    AetherSemaphore* entry = lookupSemaphore(sem);
    if (entry == NULL) {
        return -1;
    }

    pthread_mutex_lock(&entry->mutex);
    while (entry->value == 0) {
        pthread_cond_wait(&entry->cond, &entry->mutex);
    }
    --entry->value;
    pthread_mutex_unlock(&entry->mutex);
    return 0;
}

int sem_trywait(sem_t* sem)
{
    AetherSemaphore* entry = lookupSemaphore(sem);
    if (entry == NULL) {
        return -1;
    }

    pthread_mutex_lock(&entry->mutex);
    if (entry->value == 0) {
        pthread_mutex_unlock(&entry->mutex);
        errno = EAGAIN;
        return -1;
    }
    --entry->value;
    pthread_mutex_unlock(&entry->mutex);
    return 0;
}

int sem_timedwait(sem_t* sem, const struct timespec* absolute_timeout)
{
    if (absolute_timeout == NULL) {
        errno = EINVAL;
        return -1;
    }

    AetherSemaphore* entry = lookupSemaphore(sem);
    if (entry == NULL) {
        return -1;
    }

    pthread_mutex_lock(&entry->mutex);
    while (entry->value == 0) {
        const int result = pthread_cond_timedwait(
            &entry->cond, &entry->mutex, absolute_timeout);
        if (result != 0) {
            pthread_mutex_unlock(&entry->mutex);
            errno = result;
            return -1;
        }
    }
    --entry->value;
    pthread_mutex_unlock(&entry->mutex);
    return 0;
}

#endif
