/*
 * Minimal POSIX semaphore compatibility implemented with Win32 semaphores.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <errno.h>
#include <time.h>

#ifndef EIO
#define EIO EINVAL
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif

typedef HANDLE sem_t;

static inline int sem_init(sem_t* sem, int pshared, unsigned int value)
{
    if (sem == NULL || pshared != 0) {
        errno = EINVAL;
        return -1;
    }
    *sem = CreateSemaphoreA(NULL, (LONG)value, 0x7fffffffL, NULL);
    if (*sem == NULL) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static inline int sem_destroy(sem_t* sem)
{
    if (sem == NULL || *sem == NULL) {
        errno = EINVAL;
        return -1;
    }
    CloseHandle(*sem);
    *sem = NULL;
    return 0;
}

static inline int sem_post(sem_t* sem)
{
    if (sem == NULL || *sem == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!ReleaseSemaphore(*sem, 1, NULL)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static inline int sem_wait(sem_t* sem)
{
    if (sem == NULL || *sem == NULL) {
        errno = EINVAL;
        return -1;
    }
    DWORD result = WaitForSingleObject(*sem, INFINITE);
    if (result != WAIT_OBJECT_0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static inline int sem_trywait(sem_t* sem)
{
    if (sem == NULL || *sem == NULL) {
        errno = EINVAL;
        return -1;
    }
    const DWORD result = WaitForSingleObject(*sem, 0);
    if (result == WAIT_OBJECT_0) {
        return 0;
    }
    errno = result == WAIT_TIMEOUT ? EAGAIN : EIO;
    return -1;
}

static inline int sem_timedwait(sem_t* sem, const struct timespec* absolute_timeout)
{
    if (sem == NULL || *sem == NULL || absolute_timeout == NULL) {
        errno = EINVAL;
        return -1;
    }

    FILETIME fileTime;
    ULARGE_INTEGER ticks;
    struct timespec now;
    GetSystemTimePreciseAsFileTime(&fileTime);
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    const unsigned long long unixTicks =
        ticks.QuadPart - 116444736000000000ULL;
    now.tv_sec = (time_t)(unixTicks / 10000000ULL);
    now.tv_nsec = (long)((unixTicks % 10000000ULL) * 100ULL);
    long long seconds =
        (long long)absolute_timeout->tv_sec - (long long)now.tv_sec;
    long long nanoseconds =
        (long long)absolute_timeout->tv_nsec - (long long)now.tv_nsec;
    if (nanoseconds < 0LL) {
        seconds--;
        nanoseconds += 1000000000LL;
    }
    long long timeout_ms = seconds * 1000LL
        + (nanoseconds + 999999LL) / 1000000LL;
    if (timeout_ms < 0LL) {
        timeout_ms = 0LL;
    }
    if (timeout_ms > (long long)INFINITE - 1LL) {
        timeout_ms = (long long)INFINITE - 1LL;
    }

    const DWORD result = WaitForSingleObject(*sem, (DWORD)timeout_ms);
    if (result == WAIT_OBJECT_0) {
        return 0;
    }
    errno = result == WAIT_TIMEOUT ? ETIMEDOUT : EIO;
    return -1;
}
