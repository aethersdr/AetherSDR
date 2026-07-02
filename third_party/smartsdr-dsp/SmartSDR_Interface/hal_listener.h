///*! \file hal_listener.h
// *	\brief Listener for VITA-49 packets
// *
// *	\copyright	Copyright 2012-2013 FlexRadio Systems.  All Rights Reserved.
// *				Unauthorized use, duplication or distribution of this software is
// *				strictly prohibited by law.
// *
// *	\date Mar 28, 2012
// *	\author Eric & Steve
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

#ifndef _LISTENER_H
#define _LISTENER_H

#include "../common.h"
// #include "hal_display.h"
// #include "display_list.h"
#include "datatypes.h"

#define HAL_RX_BUFFER_SIZE	128
#define HAL_TX_BUFFER_SIZE	HAL_RX_BUFFER_SIZE
#define HAL_SAMPLE_SIZE	sizeof(Complex);

enum STREAM_DIRECTION
{
	INPUT 	= 1,
	OUTPUT  = 2
};
typedef enum STREAM_DIRECTION StreamDirection;

enum STREAM_TYPEX
{
	FFT = 1,
	MMX = 2,
	IQD = 3,
	AUD = 4,
	MET = 5,
	DSC = 6,
	TXD = 7,
	PAN = 8,
	WFL = 9,
	WFM = 10,
	XXX = 99
};
typedef enum STREAM_TYPEX ShortStreamType;
//
//typedef struct _stream_counters
//{
//	uint32 stream_id;
//	StreamDirection direction;
//	ShortStreamType stream_type;
//	uint32 class_id_h;
//	uint32 class_id_l;
//	uint32 size;
//	uint32 count;
//	uint32 status;
//	float speed;
//	BOOL printed;
//	uint32 ip;
//	uint16 port;
//} stream_count_type, *StreamCount;

void hal_Listener_Init(void);
//DisplayClient hal_ListenerGetFFTClient();
//void hal_showStreamReport(void);
//float hal_getStreamRate(uint32 stread_id);
//void HAL_update_count(uint32 stream_id, uint32 class_id_h, uint32 class_id_l, uint32 size, uint32 status, StreamDirection direction, ShortStreamType strm_type, uint32 ip, uint16 port);



#define HAL_STATUS_PROCESSED 1
#define HAL_STATUS_INVALID_OUI 2
#define HAL_STATUS_NO_DESC 3
#define HAL_STATUS_UNSUPPORTED_SAMP 4
#define HAL_STATUS_UNSUPPORTED_FFT 5
#define HAL_STATUS_BAD_TYPE 6
#define HAL_STATUS_FFT_NO_STREAM 7
#define HAL_STATUS_IQ_NO_STREAM 8
#define HAL_STATUS_OUTPUT_OK 9
#define HAL_STATUS_DSP_NO_STREAM 10
#define HAL_STATUS_DAX_NO_STREAM 11
#define HAL_STATUS_DAX_SIZE_WRONG 12
#define HAL_STATUS_DAX_WRONG_CHAN 13
#define HAL_STATUS_TX_SKIP 14
#define HAL_STATUS_TX_ZERO 15
#define HAL_STATUS_UNK_STREAM 16


/* Waveform defines */
#define HAL_STATUS_WFM_SIZE_WRONG 17
#define HAL_STATUS_WFM_NO_STREAM 18


#endif // _LISTENER_H
