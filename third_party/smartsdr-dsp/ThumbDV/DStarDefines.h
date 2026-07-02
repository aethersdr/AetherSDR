/*
 *  Copyright (C) 2009,2012 by Jonathan Naylor, G4KLX
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  Modified for FlexRadio WaveformAPI by Ed Gonzalez KG5FBT
 */

/* THUMBDV_DSTARDEFINES_H_ */
#ifndef THUMBDV_DSTARDEFINES_H_
#define THUMBDV_DSTARDEFINES_H_

#include "datatypes.h"

#define DSTAR_GMSK_SYMBOL_RATE  4800U
#define DSTAR_GMSK_BT           0.5F

static const BOOL BIT_SYNC_BITS[]    = {TRUE, FALSE, TRUE, FALSE};
#define BIT_SYNC_LENGTH_BITS = 4U;

static const BOOL FRAME_SYNC_BITS[]  = {TRUE, TRUE,  TRUE, FALSE, TRUE,  TRUE,  FALSE, FALSE,
                                 TRUE, FALSE, TRUE, FALSE, FALSE, FALSE, FALSE};
#define FRAME_SYNC_LENGTH_BITS  15U

//static const unsigned char DATA_SYNC_BYTES[] = {0x55, 0x2D, 0x16};
static const unsigned char DATA_SYNC_BYTES[] = {0xAA, 0xB4, 0x68};

static const BOOL DATA_SYNC_BITS[]   = {TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE,
                                 TRUE,  FALSE, TRUE,  TRUE,  FALSE, TRUE,  FALSE, FALSE,
                                 FALSE, TRUE,  TRUE,  FALSE, TRUE,  FALSE, FALSE, FALSE};

//static const unsigned char END_PATTERN_BYTES[] = {0x55, 0x55, 0x55, 0x55, 0xC8, 0x7A};
static const unsigned char END_PATTERN_BYTES[] = {0xAA, 0xAA, 0xAA, 0xAA, 0x13, 0x5E};
static const BOOL END_PATTERN_BITS[] = {TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE,
                                 TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE,
                                 TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE,
                                 TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE,
                                 FALSE, FALSE, FALSE, TRUE,  FALSE, FALSE, TRUE,  TRUE,
                                 FALSE, TRUE,  FALSE, TRUE,  TRUE,  TRUE,  TRUE,  FALSE};

#define END_PATTERN_LENGTH_BITS  48U
#define END_PATTERN_LENGTH_BYTES (END_PATTERN_LENGTH_BITS / 8U)

static const unsigned char NULL_AMBE_DATA_BYTES[] = {0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8};
static const BOOL NULL_AMBE_DATA_BITS[] = {FALSE, TRUE,  TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,
                                    TRUE,  FALSE, TRUE,  TRUE,  FALSE, FALSE, FALSE, TRUE,
                                    FALSE, TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE,
                                    FALSE, FALSE, FALSE, TRUE,  FALSE, FALSE, FALSE, TRUE,
                                    FALSE, TRUE,  TRUE,  FALSE, FALSE, TRUE,  FALSE, FALSE,
                                    FALSE, TRUE,  FALSE, TRUE,  TRUE,  FALSE, FALSE, FALSE,
                                    TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  FALSE, FALSE,
                                    TRUE,  FALSE, FALSE, FALSE, FALSE, TRUE,  TRUE,  FALSE,
                                    FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  TRUE,  TRUE};

// Note that these are already scrambled, 0x66 0x66 0x66 otherwise
static const BOOL NULL_SLOW_DATA_BITS[] = {FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  TRUE,  FALSE,
                                    FALSE, FALSE, TRUE,  FALSE, TRUE,  FALSE, FALSE, TRUE,
                                    TRUE,  TRUE,  TRUE,  TRUE,  FALSE, TRUE,  FALSE, TRUE};

#define VOICE_FRAME_LENGTH_BITS  72U
#define VOICE_FRAME_LENGTH_BYTES  (VOICE_FRAME_LENGTH_BITS / 8U)

#define DATA_FRAME_LENGTH_BITS   24U
#define DATA_FRAME_LENGTH_BYTES  (DATA_FRAME_LENGTH_BITS / 8U)

#define DV_FRAME_LENGTH_BITS     (VOICE_FRAME_LENGTH_BITS + DATA_FRAME_LENGTH_BITS)
#define DV_FRAME_LENGTH_BYTES    (VOICE_FRAME_LENGTH_BYTES + DATA_FRAME_LENGTH_BYTES)

#define FEC_SECTION_LENGTH_BITS    660U

#define RADIO_HEADER_LENGTH_BITS    330U
#define RADIO_HEADER_LENGTH_BYTES   42U

#define DATA_BLOCK_SIZE_BITS    (21U * DV_FRAME_LENGTH_BITS)
#define DATA_BLOCK_SIZE_BYTES   (21U * DV_FRAME_LENGTH_BYTES)

#define SLOW_DATA_TYPE_MASK      0xF0
#define SLOW_DATA_TYPE_GPSDATA   0x30
#define SLOW_DATA_TYPE_MESSAGE   0x40
#define SLOW_DATA_TYPE_HEADER    0x50
#define SLOW_DATA_TYPE_SQUELCH   0xC0
#define SLOW_DATA_LENGTH_MASK    0x0F

#define DSTAR_AUDIO_BLOCK_SIZE    160U
#define DSTAR_AUDIO_BLOCK_BYTES   (DSTAR_AUDIO_BLOCK_SIZE * 2U)

#define DSTAR_RADIO_SAMPLE_RATE   24000
#define DSTAR_RADIO_BLOCK_SIZE    960U

#define LONG_CALLSIGN_LENGTH    8U
#define SHORT_CALLSIGN_LENGTH   4U

#define SLOW_DATA_MESSAGE_LENGTH_BYTES    20U
#define SLOW_DATA_PACKET_LEN_BYTES          3


#define DATA_MASK             0x80U
#define REPEATER_MASK         0x40U
#define INTERRUPTED_MASK      0x20U
#define CONTROL_SIGNAL_MASK   0x10U
#define URGENT_MASK           0x08U

#define REPEATER_CONTROL_MASK   0x07U
#define REPEATER_CONTROL        0x07U
#define AUTO_REPLY              0x06U
#define RESEND_REQUESTED        0x04U
#define ACK_FLAG                0x03U
#define NO_RESPONSE             0x02U
#define RELAY_UNAVAILABLE       0x01U

#define  DSTAR_BLEEP_FREQ    2000U
#define  DSTAR_BLEEP_LENGTH   100U
#define  DSTAR_BLEEP_AMPL       0.5F

#define DSTAR_RADIO_BIT_LENGTH  (DSTAR_RADIO_SAMPLE_RATE / DSTAR_GMSK_SYMBOL_RATE)

#define  FRAME_TIME_MS 20U

#define  FRAMES_BETWEEN_SYNC 20U

#define TICKS_PER_SEC = 50U

#endif
