#include "SmartSDR_Interface/digital_voice_mode_registry.h"
#include "SmartSDR_Interface/traffic_cop.h"
#include "common.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int command_count = 0U;
static unsigned int fail_command = 0U;

uint16 hal_ListenerPort(void)
{
    return 49152U;
}

static char* copy_string(const char* value)
{
    const size_t length = strlen(value);
    char* copy = (char*)malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, value, length + 1U);
    }
    return copy;
}

uint32 tc_sendSmartSDRcommand(char* command, BOOL block, char** response)
{
    assert(block == TRUE);
    assert(response != NULL);
    ++command_count;

    if (command_count == fail_command) {
        *response = copy_string("50000016|rejected");
    } else if (strstr(command, "waveform create ") == command) {
        *response = copy_string(
            "0|tx_stream_in_id=0x81000001 rx_stream_in_id=0x81000000 "
            "tx_stream_out_id=0x01000001 rx_stream_out_id=0x01000000");
    } else if (strstr(command, "waveform remove ") == command) {
        *response = copy_string("50001000|not present");
    } else {
        *response = copy_string("0|");
    }
    return *response != NULL ? SUCCESS : SL_OUT_OF_MEMORY;
}

int main(void)
{
    assert(digital_voice_mode_registry_init("DSTR", "DFM", "AetherDStar")
           == SUCCESS);
    assert(digital_voice_mode_registry_init("BAD", "DFM", "AetherDStar")
           != SUCCESS);

    assert(digital_voice_mode_registry_init("DSTR", "DFM", "AetherDStar")
           == SUCCESS);
    assert(digital_voice_mode_registry_apply_create_response(
        "tx_stream_in_id=0x81000001 rx_stream_in_id=0x81000000 "
        "tx_stream_out_id=0x01000001 rx_stream_out_id=0x01000000"));
    assert(!digital_voice_mode_registry_apply_create_response(
        "tx_stream_in_id=0 rx_stream_in_id=2 "
        "tx_stream_out_id=3 rx_stream_out_id=4"));
    assert(!digital_voice_mode_registry_apply_create_response(
        "tx_stream_in_id=1 rx_stream_in_id=2 "
        "tx_stream_out_id=1 rx_stream_out_id=4"));
    assert(!digital_voice_mode_registry_apply_create_response(
        "tx_stream_in_id=1 tx_stream_in_id=5 rx_stream_in_id=2 "
        "tx_stream_out_id=3 rx_stream_out_id=4"));
    assert(digital_voice_mode_registry_stream_direction(0x81000000U)
           == DIGITAL_VOICE_STREAM_RX);
    assert(digital_voice_mode_registry_stream_direction(0x81000001U)
           == DIGITAL_VOICE_STREAM_TX);
    assert(digital_voice_mode_registry_stream_direction(0x81000002U)
           == DIGITAL_VOICE_STREAM_UNKNOWN);

    command_count = 0U;
    fail_command = 0U;
    assert(digital_voice_mode_registry_register() == SUCCESS);
    assert(command_count == 10U);
    assert(digital_voice_mode_registry_streams()->valid == TRUE);

    command_count = 0U;
    fail_command = 4U;
    assert(digital_voice_mode_registry_register() == 0x50000016U);
    assert(command_count == fail_command);

    puts("All digital voice mode registry tests passed.");
    return 0;
}
