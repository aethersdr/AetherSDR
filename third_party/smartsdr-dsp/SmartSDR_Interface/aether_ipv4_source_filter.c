/*
 * Exact IPv4 source filter for radio-originated UDP data.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "aether_ipv4_source_filter.h"

#include <arpa/inet.h>
#include <string.h>

int aether_ipv4_source_filter_init(aether_ipv4_source_filter* filter,
                                   const char* address)
{
    if (filter == NULL) {
        return -1;
    }
    memset(filter, 0, sizeof(*filter));
    struct in_addr parsed;
    if (address == NULL || address[0] == '\0'
        || inet_pton(AF_INET, address, &parsed) != 1) {
        return -1;
    }
    filter->network_address = parsed.s_addr;
    filter->valid = 1;
    return 0;
}

int aether_ipv4_source_filter_accepts(
    const aether_ipv4_source_filter* filter,
    uint32_t network_address)
{
    return filter != NULL && filter->valid
        && filter->network_address == network_address;
}
