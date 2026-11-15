/*
 * Winsock arpa/inet.h compatibility for the ThumbDV helper.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include <sys/socket.h>

static inline int inet_aton(const char* cp, struct in_addr* inp)
{
    if (cp == NULL || inp == NULL) {
        errno = EINVAL;
        return 0;
    }
    return InetPtonA(AF_INET, cp, inp) == 1;
}
