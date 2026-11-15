/*
 * AetherSDR digital-voice waveform helper entry point.
 *
 * Copyright (C) 2026 AetherSDR contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smartsdr_dsp_api.h"
#include "digital_voice_mode_registry.h"
#include "aether_vocoder_backend.h"
#include "common.h"

static volatile sig_atomic_t shutdown_requested = 0;

static void on_signal(int signo)
{
    (void)signo;
    shutdown_requested = 1;
}

static const char* value_arg(int* i, int argc, char** argv, const char* arg, const char* option)
{
    const size_t option_len = strlen(option);
    if (strncmp(arg, option, option_len) == 0 && arg[option_len] == '=') {
        return arg + option_len + 1;
    }
    if (strcmp(arg, option) == 0 && *i + 1 < argc) {
        (*i)++;
        return argv[*i];
    }
    return NULL;
}

static BOOL is_ascii_alphanumeric(char value)
{
    return (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
}

static BOOL valid_mycall(const char* value)
{
    if (value == NULL) {
        return FALSE;
    }
    const size_t length = strlen(value);
    if (length < 3U || length > 8U) {
        return FALSE;
    }

    BOOL has_letter = FALSE;
    BOOL has_digit = FALSE;
    for (size_t i = 0U; i < length; i++) {
        if (value[i] >= 'A' && value[i] <= 'Z') {
            has_letter = TRUE;
        } else if (value[i] >= '0' && value[i] <= '9') {
            has_digit = TRUE;
        } else {
            return FALSE;
        }
    }
    return has_letter && has_digit;
}

static BOOL valid_suffix(const char* value)
{
    if (value == NULL || value[0] == '\0') {
        return TRUE;
    }
    const size_t length = strlen(value);
    if (length > 4U) {
        return FALSE;
    }
    for (size_t i = 0U; i < length; i++) {
        if (!is_ascii_alphanumeric(value[i])) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL valid_routing(const char* value, BOOL allow_leading_slash)
{
    if (value == NULL) {
        return FALSE;
    }
    const size_t length = strlen(value);
    if (length == 0U || length > 8U) {
        return FALSE;
    }

    size_t offset = 0U;
    if (allow_leading_slash && value[0] == '/') {
        if (length == 1U) {
            return FALSE;
        }
        offset = 1U;
    }
    BOOL has_alphanumeric = FALSE;
    for (size_t i = offset; i < length; i++) {
        if (is_ascii_alphanumeric(value[i])) {
            has_alphanumeric = TRUE;
        } else if (value[i] != ' ') {
            return FALSE;
        }
    }
    return has_alphanumeric;
}

static BOOL valid_message(const char* value)
{
    if (value == NULL || value[0] == '\0') {
        return TRUE;
    }
    const size_t length = strlen(value);
    if (length > 20U) {
        return FALSE;
    }
    for (size_t i = 0U; i < length; i++) {
        const unsigned char byte = (unsigned char)value[i];
        if (byte < 0x20U || byte > 0x7eU || byte == '|') {
            return FALSE;
        }
    }
    return TRUE;
}

static const char* dstar_configuration_error(void)
{
    if (!valid_mycall(getenv("AETHER_DSTAR_MYCALL"))) {
        return "Invalid D-STAR MYCALL";
    }
    if (!valid_suffix(getenv("AETHER_DSTAR_MYCALL_SUFFIX"))) {
        return "Invalid D-STAR MYCALL suffix";
    }

    const char* urcall = getenv("AETHER_DSTAR_URCALL");
    const char* rpt1 = getenv("AETHER_DSTAR_RPT1");
    const char* rpt2 = getenv("AETHER_DSTAR_RPT2");
    if (!valid_routing(urcall != NULL ? urcall : "CQCQCQ", TRUE)) {
        return "Invalid D-STAR URCALL";
    }
    if (!valid_routing(rpt1 != NULL ? rpt1 : "DIRECT", FALSE)
        || !valid_routing(rpt2 != NULL ? rpt2 : "DIRECT", FALSE)) {
        return "Invalid D-STAR repeater routing";
    }
    if (!valid_message(getenv("AETHER_DSTAR_MESSAGE"))) {
        return "Invalid D-STAR message";
    }
    return NULL;
}

int main(int argc, char* argv[])
{
    BOOL enable_console = FALSE;
    const char* radio_ip = NULL;
    const char* vocoder = NULL;
    const char* mode = NULL;
    const char* underlying_mode = NULL;
    const char* waveform_name = NULL;
    BOOL probe_serial = FALSE;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* value = NULL;
        if (strcmp(arg, "--console") == 0) {
            enable_console = TRUE;
        } else if (strcmp(arg, "--probe-serial") == 0) {
            probe_serial = TRUE;
        } else if ((value = value_arg(&i, argc, argv, arg, "--host")) != NULL
                   || (value = value_arg(&i, argc, argv, arg, "--ip")) != NULL) {
            radio_ip = value;
        } else if ((value = value_arg(&i, argc, argv, arg, "--serial")) != NULL) {
            setenv("AETHER_DV_THUMBDV_SERIAL", value, 1);
        } else if ((value = value_arg(&i, argc, argv, arg, "--vocoder")) != NULL
                   || (value = value_arg(&i, argc, argv, arg, "--backend")) != NULL) {
            vocoder = value;
        } else if ((value = value_arg(&i, argc, argv, arg, "--mycall")) != NULL) {
            setenv("AETHER_DSTAR_MYCALL", value, 1);
        } else if ((value = value_arg(&i, argc, argv, arg, "--mycall-suffix")) != NULL) {
            setenv("AETHER_DSTAR_MYCALL_SUFFIX", value, 1);
        } else if ((value = value_arg(&i, argc, argv, arg, "--urcall")) != NULL) {
            setenv("AETHER_DSTAR_URCALL", value, 1);
        } else if ((value = value_arg(&i, argc, argv, arg, "--rpt1")) != NULL) {
            setenv("AETHER_DSTAR_RPT1", value, 1);
        } else if ((value = value_arg(&i, argc, argv, arg, "--rpt2")) != NULL) {
            setenv("AETHER_DSTAR_RPT2", value, 1);
        } else if ((value = value_arg(&i, argc, argv, arg, "--message")) != NULL) {
            setenv("AETHER_DSTAR_MESSAGE", value, 1);
        } else if ((value = value_arg(&i, argc, argv, arg, "--mode")) != NULL) {
            mode = value;
        } else if ((value = value_arg(&i, argc, argv, arg, "--underlying-mode")) != NULL) {
            underlying_mode = value;
        } else if ((value = value_arg(&i, argc, argv, arg, "--waveform-name")) != NULL) {
            waveform_name = value;
        } else {
            output("Unknown parameter - '%s'\n", arg);
        }
    }

    if (vocoder == NULL || vocoder[0] == '\0') {
        vocoder = getenv("AETHER_DV_VOCODER");
    }
    if (aether_vocoder_set_kind_from_name(vocoder) != 0) {
        output("Unknown D-Star vocoder backend '%s'\n", vocoder);
        return 2;
    }

    if (probe_serial) {
        if (!aether_vocoder_requires_serial()) {
            output("AETHER_DV_PROBE verified backend=%s\n", aether_vocoder_name());
            return 0;
        }
        if (getenv("AETHER_DV_THUMBDV_SERIAL") == NULL) {
            output("AETHER_DV_ERROR Missing --serial/AETHER_DV_THUMBDV_SERIAL\n");
            return 2;
        }
        if (aether_vocoder_probe_serial() == 0) {
            output("AETHER_DV_PROBE verified backend=%s\n", aether_vocoder_name());
            return 0;
        }
        output("AETHER_DV_ERROR %s\n", aether_vocoder_last_error());
        return 3;
    }

    if (radio_ip == NULL || radio_ip[0] == '\0') {
        radio_ip = getenv("SSDR_RADIO_ADDRESS");
    }
    if (radio_ip == NULL || radio_ip[0] == '\0') {
        output("Missing --host/SSDR_RADIO_ADDRESS\n");
        return 2;
    }
    if (aether_vocoder_requires_serial() && getenv("AETHER_DV_THUMBDV_SERIAL") == NULL) {
        output("Missing --serial/AETHER_DV_THUMBDV_SERIAL\n");
        return 2;
    }
    if (mode == NULL || mode[0] == '\0') {
        mode = getenv("AETHER_DV_MODE");
    }
    if (underlying_mode == NULL || underlying_mode[0] == '\0') {
        underlying_mode = getenv("AETHER_DV_UNDERLYING_MODE");
    }
    if (waveform_name == NULL || waveform_name[0] == '\0') {
        waveform_name = getenv("AETHER_DV_WAVEFORM_NAME");
    }
    if (digital_voice_mode_registry_init(mode, underlying_mode, waveform_name)
            != SUCCESS) {
        output("Unsupported digital-voice mode registration\n");
        return 2;
    }
    const char* configuration_error = dstar_configuration_error();
    if (configuration_error != NULL) {
        output("%s\n", configuration_error);
        return 2;
    }
    output("Digital voice vocoder backend: %s\n", aether_vocoder_name());
    if (!SmartSDR_API_Init(enable_console, radio_ip)) {
        return 4;
    }
    while (!shutdown_requested) {
        pause();
    }
    SmartSDR_API_Shutdown();
    return 0;
}
