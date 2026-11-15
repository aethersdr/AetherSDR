///*    \file status_processor.c
// *    \brief Main SmartSDR DSP API Entry point
// *
// *    \copyright  Copyright 2011-2013 FlexRadio Systems.  All Rights Reserved.
// *                Unauthorized use, duplication or distribution of this software is
// *                strictly prohibited by law.
// *
// *    \date 31-AUG-2014
// *    \author Stephen Hicks, N5AC
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
 *  Mail:  FlexRadio Systems, Suite 1-150, 4616 W. Howard LN, Austin, TX 78728
 *
 * ************************************************************************** */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "common.h"
#include "aether_smartsdr_command.h"
#include "digital_voice_slice_ownership.h"
#include "digital_voice_tx_gate.h"
#include "traffic_cop.h"
#include "sched_waveform.h"

static char* _status_value( char * token, const char * key )
{
    size_t key_len = strlen( key );
    if ( token == NULL || strncmp( token, key, key_len ) != 0 || token[key_len] != '=' ) {
        return NULL;
    }

    return token + key_len + 1;
}

static BOOL _parse_status_uint32( const char * value, uint32 * parsed_value )
{
    const char * end = NULL;
    if ( aether_smartsdr_parse_uint32(
             value, 0, parsed_value, &end ) != 0
         || *end != '\0' ) {
        return FALSE;
    }
    return TRUE;
}

static char * _status_strtok( char * string, const char * delimiters,
                              char ** context )
{
#ifdef _WIN32
    return strtok_s( string, delimiters, context );
#else
    return strtok_r( string, delimiters, context );
#endif
}

static uint32 _dstar_slice = 0;
static BOOL _dstar_slice_valid = FALSE;
static digital_voice_slice_ownership _slice_ownership =
    DIGITAL_VOICE_SLICE_OWNERSHIP_INITIALIZER;
static digital_voice_tx_gate _tx_gate = DIGITAL_VOICE_TX_GATE_INITIALIZER;

static void _apply_dstar_slice_settings( uint32 slc )
{
    char cmd[512] = {0};
    snprintf(cmd, sizeof(cmd), "slice s %d fm_deviation=1200 post_demod_low=0 post_demod_high=6000 dfm_pre_de_emphasis=0 post_demod_bypass=1 squelch=0", slc);
    tc_sendSmartSDRcommand(cmd,FALSE, NULL);
    sched_waveform_setDSTARSlice(slc);
}

static void _apply_tx_gate_action( digital_voice_tx_gate_action action )
{
    switch ( action ) {
    case DIGITAL_VOICE_TX_GATE_BEGIN:
        output( ANSI_MAGENTA "D-STAR waveform transmit started\n" );
        sched_waveform_beginTransmit();
        break;
    case DIGITAL_VOICE_TX_GATE_REQUEST_END:
        output( ANSI_MAGENTA "D-STAR waveform unkey requested\n" );
        sched_waveform_requestTransmitEnd();
        break;
    case DIGITAL_VOICE_TX_GATE_CANCEL:
        output( ANSI_MAGENTA "D-STAR waveform transmit ended\n" );
        sched_waveform_cancelTransmit();
        if ( _dstar_slice_valid ) {
            output( ANSI_MAGENTA "reasserting D-STAR receive settings on slice %d\n",
                    _dstar_slice );
            _apply_dstar_slice_settings( _dstar_slice );
        }
        sched_waveform_requestReceiveReset();
        break;
    case DIGITAL_VOICE_TX_GATE_NONE:
        break;
    }
}

void status_processor_setControlledSlice( uint32 slice,
                                          BOOL owner_valid,
                                          uint32 owner )
{
    digital_voice_slice_ownership_set(
        &_slice_ownership, slice, owner_valid, owner );
    _dstar_slice = slice;
    _dstar_slice_valid = TRUE;
    output( ANSI_MAGENTA
            "controlled D-STAR slice=%u owner=%s0x%08X\n",
            slice,
            owner_valid && owner != 0U ? "" : "unknown/",
            owner );
    _apply_dstar_slice_settings( slice );
    _apply_tx_gate_action(
        digital_voice_tx_gate_setExpectedOwner(
            &_tx_gate, owner_valid, owner ) );
    _apply_tx_gate_action(
        digital_voice_tx_gate_setModeSlice( &_tx_gate, TRUE, slice ) );
}

void status_processor_setAuthoritativeTxSelection( BOOL valid,
                                                   uint32 slice,
                                                   BOOL mode_active )
{
    output( ANSI_MAGENTA
            "authoritative TX selection: valid=%d slice=%u dstr=%d\n",
            valid, slice, mode_active );
    _apply_tx_gate_action(
        digital_voice_tx_gate_setAuthoritativeSelection(
            &_tx_gate, valid, slice, mode_active ) );
}

static BOOL _parse_interlock_event( const char * state,
                                    digital_voice_tx_event * event )
{
    if ( state == NULL || event == NULL ) {
        return FALSE;
    }

    if ( strcmp( state, "PTT_REQUESTED" ) == 0 ) {
        *event = DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED;
    } else if ( strcmp( state, "TRANSMITTING" ) == 0 ) {
        *event = DIGITAL_VOICE_TX_EVENT_TRANSMITTING;
    } else if ( strcmp( state, "UNKEY_REQUESTED" ) == 0 ) {
        *event = DIGITAL_VOICE_TX_EVENT_UNKEY_REQUESTED;
    } else if ( strcmp( state, "READY" ) == 0 ) {
        *event = DIGITAL_VOICE_TX_EVENT_READY;
    } else if ( strcmp( state, "RECEIVE" ) == 0 ) {
        *event = DIGITAL_VOICE_TX_EVENT_RECEIVE;
    } else if ( strcmp( state, "NOT_READY" ) == 0 ) {
        *event = DIGITAL_VOICE_TX_EVENT_NOT_READY;
    } else if ( strcmp( state, "TX_FAULT" ) == 0 ) {
        *event = DIGITAL_VOICE_TX_EVENT_TX_FAULT;
    } else if ( strcmp( state, "TIMEOUT" ) == 0 ) {
        *event = DIGITAL_VOICE_TX_EVENT_TIMEOUT;
    } else if ( strcmp( state, "STUCK_INPUT" ) == 0 ) {
        *event = DIGITAL_VOICE_TX_EVENT_STUCK_INPUT;
    } else {
        return FALSE;
    }
    return TRUE;
}

static void _handle_status(char* string)
{
    int argc;
    uint32 slc; // slice number
    char *argv[MAX_ARGC_STATUS + 1];       //Add one extra so we can null terminate the array

    // get the actual status message -- we don't care about the handle
    char* save = 0;
    char* start = _status_strtok(string,"|",&save);
    start = _status_strtok(NULL,"|",&save);
    if ( start == NULL ) {
        return;
    }

    // first let's look for a slice status -- these are most important
    if (strncmp(start, "slice", strlen("slice")) == 0)
    {
        tokenize(start, &argc, argv, MAX_ARGC);

        if (argc < 3)
        {
            // bad slice status ... ignoring it
            return;
        }

        if ( !_parse_status_uint32( argv[1], &slc ) )
        {
            output(ANSI_RED "Unable to parse slice number (%s)\n", argv[1]);
            return;
        }

        BOOL in_use_present = FALSE;
        BOOL in_use = TRUE;
        BOOL mode_present = FALSE;
        BOOL mode_active = FALSE;
        BOOL tx_present = FALSE;
        BOOL tx = FALSE;
        BOOL client_handle_present = FALSE;
        uint32 client_handle = 0U;
        int i;
        for (i = 2; i < argc; i++)
        {
            char* smode = _status_value( argv[i], "mode" );
            if(smode != NULL)
            {
                mode_present = TRUE;
                mode_active = strcmp( smode, "DSTR" ) == 0;
            }
            char* in_use_value = _status_value( argv[i], "in_use" );
            if(in_use_value != NULL)
            {
                uint32 parsed = 0U;
                if ( _parse_status_uint32( in_use_value, &parsed ) ) {
                    in_use_present = TRUE;
                    in_use = parsed != 0U;
                }
            }
            char* tx_value = _status_value( argv[i], "tx" );
            if(tx_value != NULL)
            {
                uint32 parsed = 0U;
                if ( _parse_status_uint32( tx_value, &parsed ) ) {
                    tx_present = TRUE;
                    tx = parsed != 0U;
                }
            }
            char* client_handle_value = _status_value(
                argv[i], "client_handle" );
            if(client_handle_value != NULL)
            {
                uint32 parsed = 0U;
                if ( _parse_status_uint32(
                         client_handle_value, &parsed ) ) {
                    client_handle_present = parsed != 0U;
                    client_handle = parsed;
                }
            }
        }

        const BOOL removed = in_use_present && !in_use;
        if ( !digital_voice_slice_ownership_accepts(
                 &_slice_ownership,
                 slc,
                 client_handle_present,
                 client_handle,
                 removed ) ) {
            return;
        }
        if ( removed ) {
            output(ANSI_MAGENTA "slice %d has been removed\n",slc);
            if ( _dstar_slice_valid && slc == _dstar_slice ) {
                _dstar_slice_valid = FALSE;
            }
        } else if ( mode_present && mode_active ) {
            output(ANSI_MAGENTA "slice %d is now in DSTR mode\n",slc);
            _dstar_slice = slc;
            _dstar_slice_valid = TRUE;
            _apply_dstar_slice_settings(slc);
        } else if ( mode_present ) {
            output(ANSI_MAGENTA "slice %d has left DSTR mode\n",slc);
            if ( _dstar_slice_valid && slc == _dstar_slice ) {
                _dstar_slice_valid = FALSE;
            }
        }
        if ( tx_present && !removed ) {
            output(ANSI_MAGENTA "slice %d is %sthe transmit slice\n",
                   slc, tx ? "" : "NOT " );
        }
        _apply_tx_gate_action(
            digital_voice_tx_gate_setSliceStatus(
                &_tx_gate,
                slc,
                in_use_present,
                in_use,
                mode_present,
                mode_active,
                tx_present,
                tx ) );

    }
    else if (strncmp(start, "interlock", strlen("interlock")) == 0)
    {
        tokenize(start, &argc, argv, MAX_ARGC);

        if (argc < 2)
        {
            // bad interlock status ... ignoring it
            return;
        }

        int i;
        char* state = NULL;
        char* source = NULL;
        BOOL tx_owner_field_present = FALSE;
        BOOL tx_owner_present = FALSE;
        uint32 tx_owner = 0U;
        for (i = 1; i < argc; i++)
        {
            char* state_value = _status_value( argv[i], "state" );
            if(state_value != NULL)
            {
                state = state_value;
                continue;
            }

            char* source_value = _status_value( argv[i], "source" );
            if(source_value != NULL)
            {
                source = source_value;
                continue;
            }

            char* owner_value = _status_value( argv[i], "tx_client_handle" );
            if(owner_value != NULL)
            {
                uint32 parsed = 0U;
                if ( _parse_status_uint32( owner_value, &parsed ) ) {
                    tx_owner_field_present = TRUE;
                    tx_owner_present = parsed != 0U;
                    tx_owner = parsed;
                }
            }
        }

        if(state != NULL)
        {
            digital_voice_tx_event event;
            if ( _parse_interlock_event( state, &event ) ) {
                const BOOL source_present = source != NULL && source[0] != '\0';
                digital_voice_tx_source tx_source =
                    DIGITAL_VOICE_TX_SOURCE_UNKNOWN;
                if ( source_present && strcmp( source, "TUNE" ) == 0 ) {
                    tx_source = DIGITAL_VOICE_TX_SOURCE_TUNE;
                } else if ( source_present && strcmp( source, "SW" ) == 0 ) {
                    tx_source = DIGITAL_VOICE_TX_SOURCE_SOFTWARE;
                } else if ( source_present
                            && ( strcmp( source, "MIC" ) == 0
                                 || strcmp( source, "ACC" ) == 0
                                 || strcmp( source, "RCA" ) == 0 ) ) {
                    tx_source = DIGITAL_VOICE_TX_SOURCE_HARDWARE;
                }
                if ( tx_owner_field_present ) {
                    _apply_tx_gate_action(
                        digital_voice_tx_gate_setTxOwner(
                            &_tx_gate, tx_owner_present, tx_owner ) );
                }
                if ( tx_source == DIGITAL_VOICE_TX_SOURCE_TUNE
                     && ( event == DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED
                          || event == DIGITAL_VOICE_TX_EVENT_TRANSMITTING ) ) {
                    output( ANSI_MAGENTA
                            "tune transmit requested - inhibiting D-STAR waveform TX\n" );
                }
                _apply_tx_gate_action(
                    digital_voice_tx_gate_observe(
                        &_tx_gate, event, source_present, tx_source ) );
            }
        }

    }

    // now we could check for other statuses that were interesting


}


void status_processor(char* string)
{
    switch (*string)
    {
        case 'V': // version
        {
            string++;
            uint32 version = getIP(string);
            api_setVersion(version);
            break;
        }
        case 'H': // handle
        {
            string++;
            uint32 val;
            sscanf(string, "%08X", &val);
            api_setHandle(val);
            break;
        }
        case 'R': // response
        {
            string++;
            const char* end = NULL;
            uint32 val = 0U;
            if (aether_smartsdr_parse_uint32(
                    string, 10, &val, &end) != 0
                || *end != '|') {
                output(ANSI_RED "Status Processor: malformed response frame\n");
                break;
            }
            char* response = (char*)end + 1;
            if (tc_setupDiagnosticsEnabled())
            {
                output("AETHER_DV_DIAG setup_rx sequence=%u response=\"%s\"\n",
                       val,
                       response != NULL ? response : "");
            }
            tc_commandList_respond(val, response);
            break;
        }
        case 'C': // command
        {
            string++;
            const char* end = NULL;
            uint32 val = 0U;
            if (aether_smartsdr_parse_uint32(
                    string, 10, &val, &end) != 0
                || *end != '|') {
                output(ANSI_RED "Status Processor: malformed command frame\n");
                break;
            }
            char* cmd = (char*)end + 1;
#ifdef DEBUG
            output("\033[32mExecuting command from SmartSDR: \033[m%s\n",cmd);
#endif
            const uint32 ret = cmd[0] != '\0'
                ? process_command(cmd)
                : SL_BAD_COMMAND;
            char response[1024];
            snprintf(response,
                     sizeof(response),
                     "waveform response %u|%u",
                     val,
                     ret);
            tc_sendSmartSDRcommand(response, FALSE, NULL );
            break;
        }
        case 'S': // status
        {
            // here we translate from SmartSDR status message we are interested in
            // and the corresponding commands we want to execute
            _handle_status(string);
            break;
        }
        case 'M': // message

            break;
        default:
            output(ANSI_YELLOW "Status Processor: unknown status \033[m%s\n",string);
            break;
    }
}
