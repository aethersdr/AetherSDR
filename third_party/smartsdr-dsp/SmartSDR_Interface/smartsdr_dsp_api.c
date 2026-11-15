///*    \file smartsdr_dsp_api.c
// *    \brief Main SmartSDR DSP API Entry point
// *
// *    \copyright  Copyright 2011-2013 FlexRadio Systems.  All Rights Reserved.
// *                Unauthorized use, duplication or distribution of this software is
// *                strictly prohibited by law.
// *
// *    \date 31-AUG-2014
// *    \author Stephen Hicks, N5AC
// *    \author Graham Haddock, KE9H
// */

/* *****************************************************************************
 *
 *  Copyright (C) 2014 FlexRadio Systems.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Contact Information:
 *  email: gpl<at>flexradiosystems.com
 *  Mail: FlexRadio Systems, Suite 1-150, 4616 W. Howard LN, Austin, TX 78728
 *
 * ************************************************************************** */

#include <semaphore.h>
#include <unistd.h>

#include <sys/prctl.h>

#include "common.h"
#include "digital_voice_mode_registry.h"
#include "discovery_client.h"
#include "hal_listener.h"
#include "sched_waveform.h"
#include "traffic_cop.h"

static uint32 _api_version;
static uint32 _handle;
static pthread_t _console_thread_ID;

static BOOL console_thread_abort = FALSE;
#define PROMPT "\n\033[92mWaveform -->\033[33m"
static sem_t _startup_sem, _communications_sem;

void api_setVersion(uint32 version)
{
    _api_version = version;
    output(ANSI_MAGENTA "version = %d.%d.%d.%d\n",
           _api_version >> 24,
           _api_version >> 16 & 0xFF,
           _api_version >> 8 & 0xFF,
           _api_version & 0xFF);
}

uint32 api_getVersion(void)
{
    return _api_version;
}

void api_setHandle(uint32 handle)
{
    _handle = handle;
    output(ANSI_MAGENTA "handle = 0x%08X\n", handle);
    sem_post(&_communications_sem);
}

uint32 api_getHandle(void)
{
    return _handle;
}

void SmartSDR_API_Shutdown(void)
{
    tc_abort();
}

void* _console_thread(void* param)
{
    (void)param;
    prctl(PR_SET_NAME, "DV-Console");
    cmd_banner();
    output(PROMPT);
    sem_post(&_startup_sem);
    sem_wait(&_communications_sem);
    while (!console_thread_abort) {
        command();
        output(PROMPT);
    }

    SmartSDR_API_Shutdown();
    return NULL;
}

BOOL SmartSDR_API_Init(BOOL enable_console, const char* radio_ip)
{
    sem_init(&_startup_sem, 0, 0);
    sem_init(&_communications_sem, 0, 0);

    lock_printf_init();
    lock_malloc_init();
    sched_waveform_Init();

    if (enable_console) {
        pthread_create(&_console_thread_ID, NULL, &_console_thread, NULL);
        sem_wait(&_startup_sem);
    }

    if (radio_ip != NULL && radio_ip[0] != '\0') {
        output("Connecting directly to radio %s\n", radio_ip);
        if (!hal_Listener_Init(radio_ip)) {
            output("AETHER_DV_ERROR unable to bind secure VITA listener\n");
            return FALSE;
        }
        tc_Init(radio_ip, SMARTSDR_API_PORT);
        return TRUE;
    }

    dc_Init(radio_ip);
    return TRUE;
}

uint32 register_mode(void)
{
    return digital_voice_mode_registry_register();
}
