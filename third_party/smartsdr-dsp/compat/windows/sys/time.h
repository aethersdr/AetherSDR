/*
 * Minimal gettimeofday compatibility for the ThumbDV helper on Windows.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <time.h>

static inline int gettimeofday(struct timeval* tv, void* tz)
{
    (void)tz;
    if (tv == NULL) {
        return -1;
    }
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    unsigned long long us = (uli.QuadPart - 116444736000000000ULL) / 10ULL;
    tv->tv_sec = (long)(us / 1000000ULL);
    tv->tv_usec = (long)(us % 1000000ULL);
    return 0;
}
