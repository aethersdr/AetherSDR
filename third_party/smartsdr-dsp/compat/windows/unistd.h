/*
 * Minimal unistd compatibility for the ThumbDV helper on Windows.
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
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef SIGPIPE
#define SIGPIPE 13
#endif

typedef SSIZE_T ssize_t;

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#if !defined(__MINGW32__)
static inline int clock_gettime(int clock_id, struct timespec* value)
{
    if (value == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (clock_id == CLOCK_REALTIME) {
        FILETIME file_time;
        ULARGE_INTEGER ticks;
        GetSystemTimePreciseAsFileTime(&file_time);
        ticks.LowPart = file_time.dwLowDateTime;
        ticks.HighPart = file_time.dwHighDateTime;
        const unsigned long long unix_ticks =
            ticks.QuadPart - 116444736000000000ULL;
        value->tv_sec = (time_t)(unix_ticks / 10000000ULL);
        value->tv_nsec = (long)((unix_ticks % 10000000ULL) * 100ULL);
        return 0;
    }
    if (clock_id == CLOCK_MONOTONIC) {
        LARGE_INTEGER counter;
        LARGE_INTEGER frequency;
        if (!QueryPerformanceCounter(&counter)
                || !QueryPerformanceFrequency(&frequency)
                || frequency.QuadPart <= 0) {
            errno = EIO;
            return -1;
        }
        value->tv_sec = (time_t)(counter.QuadPart / frequency.QuadPart);
        value->tv_nsec = (long)(
            (counter.QuadPart % frequency.QuadPart) * 1000000000LL
            / frequency.QuadPart);
        return 0;
    }
    errno = EINVAL;
    return -1;
}
#endif

#define strdup _strdup
#define read _read
#define write _write

static inline int usleep(unsigned int usec)
{
    Sleep((usec + 999U) / 1000U);
    return 0;
}

static inline int pause(void)
{
    Sleep(100);
    return 0;
}

#if !defined(__MINGW32__)
static inline int nanosleep(const struct timespec* req, struct timespec* rem)
{
    (void)rem;
    if (req == NULL) {
        return 0;
    }
    DWORD ms = (DWORD)(req->tv_sec * 1000 + (req->tv_nsec + 999999L) / 1000000L);
    Sleep(ms);
    return 0;
}
#endif

static inline int setenv(const char* name, const char* value, int overwrite)
{
    if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && getenv(name) != NULL) {
        return 0;
    }
    if (_putenv_s(name, value != NULL ? value : "") != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
