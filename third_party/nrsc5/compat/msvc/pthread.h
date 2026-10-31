/* AetherSDR native Windows adapter; GPL-3.0-or-later.
 * Private, deliberately limited pthread subset used by pinned nrsc5. Pipe
 * mode uses only the FFTW planner mutex and never creates a decoder thread.
 * Preserve the retained upstream device/file entrypoints without introducing
 * a pthread DLL or exporting this compatibility layer to any other target.
 */
#pragma once

#ifndef _WIN32
#error "This adapter requires Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#include <errno.h>
#include <stdlib.h>

typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef HANDLE pthread_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const void *attributes)
{
    (void)attributes;
    InitializeSRWLock(mutex);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    AcquireSRWLockExclusive(mutex);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    ReleaseSRWLockExclusive(mutex);
    return 0;
}

static inline int pthread_cond_init(pthread_cond_t *condition, const void *attributes)
{
    (void)attributes;
    InitializeConditionVariable(condition);
    return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t *condition)
{
    WakeAllConditionVariable(condition);
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    return SleepConditionVariableSRW(condition, mutex, INFINITE, 0) ? 0 : EINVAL;
}

struct aether_nrsc5_thread_start
{
    void *(*function)(void *);
    void *argument;
};

static unsigned __stdcall aether_nrsc5_thread_entry(void *opaque)
{
    struct aether_nrsc5_thread_start start = *(struct aether_nrsc5_thread_start *)opaque;
    free(opaque);
    start.function(start.argument);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const void *attributes,
                                 void *(*function)(void *), void *argument)
{
    (void)attributes;
    struct aether_nrsc5_thread_start *start = malloc(sizeof(*start));
    if (!start) {
        return ENOMEM;
    }
    start->function = function;
    start->argument = argument;
    const uintptr_t handle = _beginthreadex(NULL, 0, aether_nrsc5_thread_entry, start, 0, NULL);
    if (!handle) {
        free(start);
        return EAGAIN;
    }
    *thread = (HANDLE)handle;
    return 0;
}

static inline int pthread_join(pthread_t thread, void **result)
{
    /* nrsc5 joins only with a null result and its worker returns NULL. */
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) {
        return EINVAL;
    }
    if (result) {
        *result = NULL;
    }
    return CloseHandle(thread) ? 0 : EINVAL;
}
