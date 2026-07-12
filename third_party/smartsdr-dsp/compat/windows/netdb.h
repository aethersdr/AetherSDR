/*
 * Winsock netdb.h compatibility for the ThumbDV helper.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include <sys/socket.h>

#ifndef gai_strerror
#define gai_strerror gai_strerrorA
#endif
