/*
 * Compatibility shim for the Linux prctl thread-name call used by SmartSDR-DSP.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#define PR_SET_NAME 15
#ifdef __APPLE__
#include <pthread.h>
static inline int aether_prctl(int option, const char* name) {
    (void)option;
    if (name) { pthread_setname_np(name); }
    return 0;
}
#else
static inline int aether_prctl(int option, const char* name) { (void)option; (void)name; return 0; }
#endif
#define prctl(option, name) aether_prctl((option), (name))
