/* *****************************************************************************
 * common.h															2015 JAN 27
 *
 *  Created on: Aug 28, 2014
 *      Author: Graham / KE9H
 *
 * 	Included common project headers
 *
 * *****************************************************************************
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

#ifndef COMMON_H_
#define COMMON_H_

#include <pthread.h>

#include "datatypes.h"
#include "hal_listener.h"
#include "io_utils.h"
#include "vita_output.h"
#include "vita49_context.h"
#include "smartsdr_dsp_api.h"
#include "utils.h"
#include "main.h"
#include "vita.h"
#include "cmd.h"


#define SUCCESS 0
#define FAIL    1

#define SMARTSDR_API_PORT           "4992"
#define VITA_49_PORT                5000
#define VITA_49_SOURCE_PORT			VITA_49_PORT
#define VITA_49_FFT_SOURCE_PORT		30003
#define VITA_49_METER_SOURCE_PORT	30002
#define VITA_49_STATUS_PORT			VITA_49_PORT
#define DISCOVERY_PORT				4992
#define FLEXRADIO_OUI               0x001C2D
#define INVALID_SLICE_RX            0xFFFF0000
#define SL_BAD_COMMAND              0x50000FFF

#define SL_VITA_INFO_CLASS			0x534C

#define SL_CLASS_SAMPLING_24KHZ		0x03
#define SL_CLASS_32BPS				(3 << 5)
#define SL_CLASS_AUDIO_STEREO		(0x3 << 7)
#define SL_CLASS_IEEE_754			(0x1 << 9)
#define SL_VITA_SLICE_AUDIO_CLASS	(SL_VITA_INFO_CLASS << 16) | SL_CLASS_SAMPLING_24KHZ | SL_CLASS_32BPS | SL_CLASS_AUDIO_STEREO | SL_CLASS_IEEE_754

#define SL_ERROR_BASE                   0x50000000
#define SL_OUT_OF_MEMORY                SL_ERROR_BASE + 0x00B
#define SL_UNKNOWN_COMMAND              SL_ERROR_BASE + 0x015
#define SL_TERMINATE                    SL_ERROR_BASE + 0x037
#define SL_CLOSE_CLIENT                 SL_ERROR_BASE + 0x03A

#define ANSI_ESC                "\033["
#define ANSI_RED 	ANSI_ESC  	"91m"
#define ANSI_GREEN 	ANSI_ESC 	"92m"
#define ANSI_YELLOW ANSI_ESC    "93m"
#define ANSI_BLUE 	ANSI_ESC  	"94m"
#define ANSI_MAGENTA ANSI_ESC   "95m"
#define ANSI_CYAN 	ANSI_ESC 	"96m"
#define ANSI_WHITE 	ANSI_ESC	"97m"
#define CLR_WHT 	"\033[97m"
#define ANSI_COLOR_OFF ANSI_ESC "m"

#endif /* COMMON_H_ */
