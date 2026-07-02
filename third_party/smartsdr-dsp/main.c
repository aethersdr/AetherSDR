/*
 * AetherSDR D-Star waveform helper entry point.
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
#include "aether_vocoder_backend.h"
#include "common.h"

const char* APP_NAME = "ThumbDV";
char * cfg_path = NULL;
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

int main(int argc, char* argv[])
{
    BOOL enable_console = FALSE;
    const char* radio_ip = NULL;
    const char* vocoder = NULL;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* value = NULL;
        if (strcmp(arg, "--console") == 0) {
            enable_console = TRUE;
        } else if ((value = value_arg(&i, argc, argv, arg, "--host")) != NULL
                   || (value = value_arg(&i, argc, argv, arg, "--ip")) != NULL) {
            radio_ip = value;
        } else if ((value = value_arg(&i, argc, argv, arg, "--serial")) != NULL) {
            setenv("AETHER_DSTAR_THUMBDV_SERIAL", value, 1);
        } else if ((value = value_arg(&i, argc, argv, arg, "--vocoder")) != NULL
                   || (value = value_arg(&i, argc, argv, arg, "--backend")) != NULL) {
            vocoder = value;
        } else if ((value = value_arg(&i, argc, argv, arg, "--cfg_path")) != NULL) {
            cfg_path = strdup(value);
        } else if (value_arg(&i, argc, argv, arg, "--mode") != NULL
                   || value_arg(&i, argc, argv, arg, "--underlying-mode") != NULL) {
            continue;
        } else {
            output("Unknown parameter - '%s'\n", arg);
        }
    }

    if (vocoder == NULL || vocoder[0] == '\0') {
        vocoder = getenv("AETHER_DSTAR_VOCODER");
    }
    if (aether_vocoder_set_kind_from_name(vocoder) != 0) {
        output("Unknown D-Star vocoder backend '%s'\n", vocoder);
        return 2;
    }

    if (radio_ip == NULL || radio_ip[0] == '\0') {
        radio_ip = getenv("SSDR_RADIO_ADDRESS");
    }
    if (radio_ip == NULL || radio_ip[0] == '\0') {
        output("Missing --host/SSDR_RADIO_ADDRESS\n");
        return 2;
    }
    if (aether_vocoder_requires_serial() && getenv("AETHER_DSTAR_THUMBDV_SERIAL") == NULL) {
        output("Missing --serial/AETHER_DSTAR_THUMBDV_SERIAL\n");
        return 2;
    }
    if (cfg_path == NULL) {
        cfg_path = strdup("./");
    }

    output("D-Star vocoder backend: %s\n", aether_vocoder_name());
    SmartSDR_API_Init(enable_console, radio_ip);
    while (!shutdown_requested) {
        pause();
    }
    SmartSDR_API_Shutdown();
    free(cfg_path);
    return 0;
}
