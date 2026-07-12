/* *****************************************************************************
 *	file stream.h
 *	\brief Collection for system streams
 *
 *	\copyright	Copyright 2012-2013 FlexRadio Systems.  All Rights Reserved.
 *				Unauthorized use, duplication or distribution of this software is
 *				strictly prohibited by law.
 *
 *	\date Mar 29, 2012
 *	\author Eric & Steve
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

#ifndef _STREAM_H
#define _STREAM_H

#include "../common.h"

#define STREAM_BITS_IN			0x80000000
#define STREAM_BITS_OUT		    0x00000000

#define STREAM_BITS_METER		0x08000000
#define STREAM_BITS_WAVEFORM    0x01000000

#define STREAM_BITS_MASK	    (STREAM_BITS_IN | STREAM_BITS_OUT | STREAM_BITS_METER | \
							        STREAM_BITS_WAVEFORM)

#define STREAM_PREFIX           0x7700

enum STREAM_TYPE
{
	STREAM_NULL = STREAM_PREFIX,
	STREAM_IN_RF_IQ,
	STREAM_IN_AUDIO,
	STREAM_IN_RF_NAR,
	STREAM_IN_FFT,
	STREAM_OUT_RF_IQ,
	STREAM_OUT_PANADAPTER,
	STREAM_OUT_WATERFALL,
	STREAM_OUT_AUDIO,
	STREAM_OUT_METER,
	STREAM_IN_WAVEFORM,
	STREAM_OUT_WAVEFORM
};

typedef enum STREAM_TYPE STREAMtype;

typedef uint32 Stream_ID;

#endif // _STREAM_H
