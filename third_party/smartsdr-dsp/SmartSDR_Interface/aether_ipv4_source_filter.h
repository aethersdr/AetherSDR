/*
 * Exact IPv4 source filter for radio-originated UDP data.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AETHER_IPV4_SOURCE_FILTER_H_
#define AETHER_IPV4_SOURCE_FILTER_H_

#include <stdint.h>

typedef struct aether_ipv4_source_filter
{
    uint32_t network_address;
    int valid;
} aether_ipv4_source_filter;

int aether_ipv4_source_filter_init(aether_ipv4_source_filter* filter,
                                   const char* address);
int aether_ipv4_source_filter_accepts(
    const aether_ipv4_source_filter* filter,
    uint32_t network_address);

#endif /* AETHER_IPV4_SOURCE_FILTER_H_ */
