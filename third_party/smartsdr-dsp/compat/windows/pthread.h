/*
 * Minimal pthread compatibility implemented with Win32 primitives.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#if defined(__MINGW32__) || defined(__MINGW64__)

#include_next <pthread.h>

#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <errno.h>

#if defined(_MSC_VER) && !defined(__thread)
#define __thread __declspec(thread)
#endif

typedef HANDLE pthread_t;
typedef CRITICAL_SECTION pthread_rwlock_t;
typedef SRWLOCK pthread_mutex_t;
typedef void pthread_attr_t;
typedef void pthread_mutexattr_t;
typedef void pthread_rwlockattr_t;

struct sched_param {
    int sched_priority;
};

#ifndef SCHED_FIFO
#define SCHED_FIFO 1
#endif

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

typedef struct AetherPthreadStart {
    void* (*start)(void*);
    void* arg;
} AetherPthreadStart;

static DWORD WINAPI aether_pthread_trampoline(LPVOID param)
{
    AetherPthreadStart* start = (AetherPthreadStart*)param;
    void* (*fn)(void*) = start->start;
    void* arg = start->arg;
    HeapFree(GetProcessHeap(), 0, start);
    fn(arg);
    return 0;
}

static inline int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                                 void* (*start_routine)(void*), void* arg)
{
    (void)attr;
    if (thread == NULL || start_routine == NULL) {
        return EINVAL;
    }
    AetherPthreadStart* start =
        (AetherPthreadStart*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*start));
    if (start == NULL) {
        return ENOMEM;
    }
    start->start = start_routine;
    start->arg = arg;
    HANDLE handle = CreateThread(NULL, 0, aether_pthread_trampoline, start, 0, NULL);
    if (handle == NULL) {
        HeapFree(GetProcessHeap(), 0, start);
        return EAGAIN;
    }
    *thread = handle;
    return 0;
}

static inline int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr)
{
    (void)attr;
    InitializeSRWLock(mutex);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t* mutex)
{
    AcquireSRWLockExclusive(mutex);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t* mutex)
{
    ReleaseSRWLockExclusive(mutex);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t* mutex)
{
    (void)mutex;
    return 0;
}

static inline int pthread_rwlock_init(pthread_rwlock_t* lock, const pthread_rwlockattr_t* attr)
{
    (void)attr;
    InitializeCriticalSection(lock);
    return 0;
}

static inline int pthread_rwlock_rdlock(pthread_rwlock_t* lock)
{
    EnterCriticalSection(lock);
    return 0;
}

static inline int pthread_rwlock_wrlock(pthread_rwlock_t* lock)
{
    EnterCriticalSection(lock);
    return 0;
}

static inline int pthread_rwlock_unlock(pthread_rwlock_t* lock)
{
    LeaveCriticalSection(lock);
    return 0;
}

static inline int pthread_rwlock_destroy(pthread_rwlock_t* lock)
{
    DeleteCriticalSection(lock);
    return 0;
}

static inline int pthread_setschedparam(pthread_t thread, int policy,
                                        const struct sched_param* param)
{
    (void)thread;
    (void)policy;
    (void)param;
    return 0;
}

static inline int pthread_join(pthread_t thread, void** value_ptr)
{
    (void)value_ptr;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}

#endif
