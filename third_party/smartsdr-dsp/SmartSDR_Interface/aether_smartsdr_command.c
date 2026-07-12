/*
 * SmartSDR command framing and status-value encoding.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "aether_smartsdr_command.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int aether_smartsdr_frame_command(char* destination,
                                  size_t destination_size,
                                  uint32_t sequence,
                                  const char* command)
{
    if (destination == NULL || destination_size == 0U
        || command == NULL || command[0] == '\0'
        || strpbrk(command, "\r\n") != NULL) {
        return -1;
    }

    const int length = snprintf(destination,
                                destination_size,
                                "C%u|%s\n",
                                sequence,
                                command);
    if (length < 0 || (size_t)length >= destination_size) {
        destination[0] = '\0';
        return -1;
    }
    return length;
}

int aether_smartsdr_parse_uint32(const char* text,
                                 int base,
                                 uint32_t* value,
                                 const char** end)
{
    if (text == NULL || value == NULL || text[0] == '\0'
        || isspace((unsigned char)text[0])
        || text[0] == '+' || text[0] == '-') {
        return -1;
    }

    errno = 0;
    char* parsed_end = NULL;
    const unsigned long parsed = strtoul(text, &parsed_end, base);
    if (errno != 0 || parsed_end == text || parsed > UINT32_MAX) {
        return -1;
    }

    *value = (uint32_t)parsed;
    if (end != NULL) {
        *end = parsed_end;
    }
    return 0;
}

size_t aether_smartsdr_encode_status_value(char* destination,
                                           size_t destination_size,
                                           const void* source,
                                           size_t source_size)
{
    if (destination == NULL || destination_size == 0U) {
        return 0U;
    }

    const unsigned char* bytes = (const unsigned char*)source;
    size_t written = 0U;
    while (bytes != NULL && written < source_size
           && written + 1U < destination_size) {
        const unsigned char value = bytes[written];
        if (value == 0x20U) {
            destination[written] = (char)0x7fU;
        } else if (value >= 0x21U && value <= 0x7eU
                   && value != (unsigned char)'|'
                   && value != (unsigned char)'=') {
            destination[written] = (char)value;
        } else {
            destination[written] = '?';
        }
        written++;
    }
    destination[written] = '\0';
    return written;
}
