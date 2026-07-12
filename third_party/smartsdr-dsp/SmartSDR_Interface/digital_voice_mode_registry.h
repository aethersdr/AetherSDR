#ifndef DIGITAL_VOICE_MODE_REGISTRY_H_
#define DIGITAL_VOICE_MODE_REGISTRY_H_

#include "datatypes.h"

typedef enum digital_voice_stream_direction
{
    DIGITAL_VOICE_STREAM_UNKNOWN = 0,
    DIGITAL_VOICE_STREAM_RX,
    DIGITAL_VOICE_STREAM_TX
} digital_voice_stream_direction;

typedef struct digital_voice_stream_map
{
    uint32 tx_stream_in_id;
    uint32 rx_stream_in_id;
    uint32 tx_stream_out_id;
    uint32 rx_stream_out_id;
    BOOL valid;
} digital_voice_stream_map;

uint32 digital_voice_mode_registry_init(const char* mode,
                                        const char* underlying_mode,
                                        const char* waveform_name);
uint32 digital_voice_mode_registry_register(void);
BOOL digital_voice_mode_registry_apply_create_response(const char* message);
const digital_voice_stream_map* digital_voice_mode_registry_streams(void);
digital_voice_stream_direction digital_voice_mode_registry_stream_direction(
    uint32 stream_id);
const char* digital_voice_mode_registry_mode(void);
const char* digital_voice_mode_registry_waveform_name(void);

#endif /* DIGITAL_VOICE_MODE_REGISTRY_H_ */
