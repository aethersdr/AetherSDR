/*
 * Digital-voice waveform registration and stream ownership.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "digital_voice_mode_registry.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "aether_smartsdr_command.h"
#include "crdv.h"
#include "hal_listener.h"
#include "smartsdr_dsp_api.h"
#include "traffic_cop.h"

typedef struct digital_voice_mode_descriptor
{
    const char* mode;
    const char* underlying_mode;
    const char* waveform_name;
    const char* version;
} digital_voice_mode_descriptor;

static const digital_voice_mode_descriptor supported_modes[] = {
    { "DSTR", "DFM", "AetherDStar", "1.2.0" }
};

static const digital_voice_mode_descriptor* active_mode = NULL;
static digital_voice_stream_map active_streams;
enum { DIGITAL_VOICE_MAX_COMMAND_SIZE = 1024 };

static char* duplicate_string(const char* value)
{
    const size_t length = strlen(value);
    char* copy = (char*)malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, value, length + 1U);
    }
    return copy;
}

static BOOL parse_response(const char* response,
                           uint32* code,
                           const char** message)
{
    if (response == NULL || code == NULL || message == NULL) {
        return FALSE;
    }

    const char* end = NULL;
    if (aether_smartsdr_parse_uint32(response, 16, code, &end) != 0) {
        return FALSE;
    }
    if (*end != '\0' && *end != '|') {
        return FALSE;
    }

    *message = *end == '|' ? end + 1 : "";
    return TRUE;
}

BOOL digital_voice_mode_registry_apply_create_response(const char* message)
{
    digital_voice_stream_map parsed;
    uint32_t streams[4] = {0U, 0U, 0U, 0U};
    memset(&parsed, 0, sizeof(parsed));
    if (message == NULL
            || crdv_control_parse_create_streams(
                   message, strlen(message), streams) != CRDV_OK) {
        return FALSE;
    }
    parsed.tx_stream_in_id = streams[0];
    parsed.rx_stream_in_id = streams[1];
    parsed.tx_stream_out_id = streams[2];
    parsed.rx_stream_out_id = streams[3];
    parsed.valid = TRUE;
    active_streams = parsed;
    return TRUE;
}

static uint32 send_verified(BOOL require_success,
                            char** response_message,
                            const char* format,
                            ...)
{
    char command[DIGITAL_VOICE_MAX_COMMAND_SIZE];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(command, sizeof(command), format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= sizeof(command)) {
        output("AETHER_DV_ERROR registration command_too_long\n");
        return SL_ERROR_BASE;
    }

    char* response = NULL;
    const uint32 send_result = tc_sendSmartSDRcommand(command, TRUE, &response);
    if (send_result != SUCCESS || response == NULL) {
        output("AETHER_DV_ERROR registration command=\"%s\" reason=no_response\n",
               command);
        free(response);
        return SL_ERROR_BASE;
    }

    uint32 code = 0U;
    const char* message = NULL;
    if (!parse_response(response, &code, &message)) {
        output("AETHER_DV_ERROR registration command=\"%s\" reason=bad_response\n",
               command);
        free(response);
        return SL_ERROR_BASE;
    }
    if (require_success && code != SUCCESS) {
        output("AETHER_DV_ERROR registration command=\"%s\" code=0x%08X message=\"%s\"\n",
               command,
               code,
               message);
        free(response);
        return code;
    }

    if (response_message != NULL) {
        *response_message = duplicate_string(message);
        if (*response_message == NULL) {
            free(response);
            return SL_OUT_OF_MEMORY;
        }
    }
    free(response);
    return SUCCESS;
}

uint32 digital_voice_mode_registry_init(const char* mode,
                                        const char* underlying_mode,
                                        const char* waveform_name)
{
    active_mode = NULL;
    memset(&active_streams, 0, sizeof(active_streams));
    for (size_t i = 0U; i < sizeof(supported_modes) / sizeof(supported_modes[0]); ++i) {
        const digital_voice_mode_descriptor* candidate = &supported_modes[i];
        if (mode != NULL && underlying_mode != NULL && waveform_name != NULL
                && strcmp(candidate->mode, mode) == 0
                && strcmp(candidate->underlying_mode, underlying_mode) == 0
                && strcmp(candidate->waveform_name, waveform_name) == 0) {
            active_mode = candidate;
            return SUCCESS;
        }
    }
    return SL_UNKNOWN_COMMAND;
}

uint32 digital_voice_mode_registry_register(void)
{
    if (active_mode == NULL) {
        return SL_UNKNOWN_COMMAND;
    }

    memset(&active_streams, 0, sizeof(active_streams));

    // Removing an absent registration is harmless; receiving a response is
    // still required, and the create transaction below must succeed.
    uint32 result = send_verified(FALSE, NULL,
        "waveform remove %s", active_mode->waveform_name);
    if (result != SUCCESS) {
        return result;
    }

    char* create_message = NULL;
    result = send_verified(TRUE, &create_message,
        "waveform create name=%s mode=%s underlying_mode=%s version=%s",
        active_mode->waveform_name,
        active_mode->mode,
        active_mode->underlying_mode,
        active_mode->version);
    if (result != SUCCESS) {
        free(create_message);
        return result;
    }
    if (!digital_voice_mode_registry_apply_create_response(create_message)) {
        output("AETHER_DV_ERROR registration waveform=%s reason=missing_stream_ids response=\"%s\"\n",
               active_mode->waveform_name,
               create_message);
        free(create_message);
        return SL_ERROR_BASE;
    }
    free(create_message);

    const char* commands[] = {
        "waveform set %s tx=1",
        "waveform set %s rx_filter low_cut=-3500",
        "waveform set %s rx_filter high_cut=3500",
        "waveform set %s rx_filter depth=256",
        "waveform set %s tx_filter low_cut=0",
        "waveform set %s tx_filter high_cut=4800",
        "waveform set %s tx_filter depth=256"
    };
    for (size_t i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        result = send_verified(TRUE, NULL, commands[i], active_mode->waveform_name);
        if (result != SUCCESS) {
            return result;
        }
    }

    const uint16 listener_port = hal_ListenerPort();
    if (listener_port == 0U) {
        output("AETHER_DV_ERROR registration waveform=%s reason=udp_listener_not_ready\n",
               active_mode->waveform_name);
        return SL_ERROR_BASE;
    }
    result = send_verified(TRUE, NULL,
        "waveform set %s udpport=%u",
        active_mode->waveform_name,
        listener_port);
    if (result != SUCCESS) {
        return result;
    }

    output("AETHER_DV_READY mode=%s waveform=%s udpport=%u rx_stream=0x%08X tx_stream=0x%08X\n",
           active_mode->mode,
           active_mode->waveform_name,
           listener_port,
           active_streams.rx_stream_in_id,
           active_streams.tx_stream_in_id);
    return SUCCESS;
}

const digital_voice_stream_map* digital_voice_mode_registry_streams(void)
{
    return &active_streams;
}

digital_voice_stream_direction digital_voice_mode_registry_stream_direction(
    uint32 stream_id)
{
    if (!active_streams.valid) {
        return DIGITAL_VOICE_STREAM_UNKNOWN;
    }
    if (stream_id == active_streams.rx_stream_in_id) {
        return DIGITAL_VOICE_STREAM_RX;
    }
    if (stream_id == active_streams.tx_stream_in_id) {
        return DIGITAL_VOICE_STREAM_TX;
    }
    return DIGITAL_VOICE_STREAM_UNKNOWN;
}

const char* digital_voice_mode_registry_mode(void)
{
    return active_mode != NULL ? active_mode->mode : "";
}

const char* digital_voice_mode_registry_waveform_name(void)
{
    return active_mode != NULL ? active_mode->waveform_name : "";
}
