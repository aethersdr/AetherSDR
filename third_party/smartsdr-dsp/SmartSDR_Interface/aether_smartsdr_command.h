/*
 * SmartSDR command framing and status-value encoding.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AETHER_SMARTSDR_COMMAND_H_
#define AETHER_SMARTSDR_COMMAND_H_

#include <stddef.h>
#include <stdint.h>

/* Returns the complete frame length, or -1 when the command is invalid. */
int aether_smartsdr_frame_command(char* destination,
                                  size_t destination_size,
                                  uint32_t sequence,
                                  const char* command);

/*
 * Parses one unsigned 32-bit integer and returns the first unconsumed byte.
 * Leading signs are rejected explicitly so behavior is identical when
 * unsigned long is either 32 or 64 bits.
 */
int aether_smartsdr_parse_uint32(const char* text,
                                 int base,
                                 uint32_t* value,
                                 const char** end);

/*
 * Encodes an untrusted fixed-width field for a SmartSDR status value.
 * Spaces use the protocol's DEL placeholder; delimiters, controls, and
 * non-ASCII bytes become '?'. The result is always NUL terminated when the
 * destination has capacity.
 */
size_t aether_smartsdr_encode_status_value(char* destination,
                                           size_t destination_size,
                                           const void* source,
                                           size_t source_size);

#endif /* AETHER_SMARTSDR_COMMAND_H_ */
