/* *****************************************************************************
 * 	vita49_context.h
 *
 *		VITA49 context
 *
 *	\date 12-FEB-2011
 *	\author Stephen Hicks, N5AC
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

#ifndef VITA49_CONTEXT_H_
#define VITA49_CONTEXT_H_

#include "../common.h"

#define VITA_C_CONTEXT_FIELD_CHG_INDICATOR	0x80000000
#define VITA_C_REFERENCE_POINT_IDENTIFIER	0x40000000
#define VITA_C_BANDWIDTH					0x20000000
#define VITA_C_IF_REFERENCE_FREQ			0x10000000
#define VITA_C_RF_REFERENCE_FREQ			0x08000000
#define VITA_C_RF_REFERENCE_FREQ_OFFSET		0x04000000
#define VITA_C_IF_BAND_OFFSET				0x02000000
#define VITA_C_REFERENCE_LEVEL				0x01000000
#define VITA_C_GAIN							0x00800000
#define VITA_C_OVER_RANGE_COUNT				0x00400000
#define VITA_C_SAMPLE_RATE					0x00200000
#define VITA_C_TIMESTAMP_ADJUSTMENT			0x00100000
#define VITA_C_TIMESTAMP_CALIBRATION_TIME	0x00080000
#define VITA_C_TEMPERATURE					0x00040000
#define VITA_C_DEVICE_IDENTIFIER			0x00020000
#define VITA_C_STATE_AND_EVENT_INDICATORS	0x00010000
#define VITA_C_DATA_PACKET_PAYLOAD_FORMAT	0x00008000
#define VITA_C_FORMATTED_GPS_GEOLOCATION	0x00004000
#define VITA_C_FORMATTED_INS_LOCATION		0x00002000
#define VITA_C_ECEF_EPHEMERIS				0x00001000
#define VITA_C_RELATIVE_EPHEMERIS			0x00000800
#define VITA_C_EPHEMERIS_REFERENCE_IND		0x00000400
#define VITA_C_GPS_ASCII					0x00000200
#define VITA_C_CONTEXT_ASSOCIATION_LISTS	0x00000100
#define VITA_C_RESERVED						0x000000FF

// bit positions for status and indicator fields
#define VITA_S_CALIBRATED_TIME_E			0x80000000
#define VITA_S_VALID_DATA_E					0x40000000
#define VITA_S_REFERENCE_LOCK_E				0x20000000
#define VITA_S_AGC_INDICATOR_E				0x10000000
#define VITA_S_DETECTED_SIGNAL_E			0x08000000
#define VITA_S_SPECTRAL_INVERSION_E			0x04000000
#define VITA_S_OVER_RANGE_E					0x02000000
#define VITA_S_SAMPLE_LOSS_E				0x01000000
#define VITA_S_CALIBRATED_TIME_I			0x00080000
#define VITA_S_VALID_DATA_I					0x00040000
#define VITA_S_REFERENCE_LOCK_I				0x00020000
#define VITA_S_AGC_INDICATOR_I				0x00010000
#define VITA_S_DETECTED_SIGNAL_I			0x00008000
#define VITA_S_SPECTRAL_INVERSION_I			0x00004000
#define VITA_S_OVER_RANGE_I					0x00002000
#define VITA_S_SAMPLE_LOSS_I				0x00001000

#define VITA_P_PACKING_METHOD				0x0 << 31	 	// we are processing efficient
#define VITA_P_REAL_COMPLEX					0x1 << 28		// complex cartesian
#define VITA_P_DATA_ITEM_FORMAT				0x00 << 24		// signed, fixed point
#define VITA_P_SAMPLE_REPEAT				0x0 << 23		// no sample-component repeating
#define VITA_P_EVENT_TAG_SIZE				0x0 << 20		// no event tags
#define VITA_P_CHANNEL_TAG_SIZE				0x0 << 18		// no channel tags
#define VITA_P_ITEM_PACKING_FIELD_SIZE		32 << 6			// 32-bit fields
#define VITA_P_DATA_ITEM_SIZE				24				// 24-bits per item
#define VITA_P_REPEAT_COUNT					0 << 16			// no repeating
#define VITA_P_VECTOR_SIZE					0				// no vectors


#define VITA_P_PACKING_H	VITA_P_PACKING_METHOD | \
							VITA_P_REAL_COMPLEX | \
							VITA_P_DATA_ITEM_FORMAT | \
							VITA_P_SAMPLE_REPEAT | \
							VITA_P_EVENT_TAG_SIZE | \
							VITA_P_CHANNEL_TAG_SIZE | \
							VITA_P_ITEM_PACKING_FIELD_SIZE | \
							VITA_P_DATA_ITEM_SIZE
#define VITA_P_PACKING_L	VITA_P_REPEAT_COUNT | \
							VITA_P_VECTOR_SIZE

#define VITA_PERIODIC 1000 // send out packets once per second even if no changes (in ms)

typedef struct _context
{
	uint32 header;
	uint32 streamID;
	uint32 classID_H;
	uint32 classID_L;
//	uint32 timestamp_sec; // we'll add these later
//	uint64 timestamp_frac;
	uint32 context_indicator;
	uint64 bandwidth;
	uint64 if_reference;
	uint64 rf_reference;
	uint32 reference_level;
	uint16 gain2;
	uint16 gain1;
	uint64 sample_rate;
	uint32 state_event_ind;
	uint32 packet_format_H;
	uint32 packet_format_L;
} vitacontext_type, *VITACONTEXT;



#endif /* VITA49_CONTEXT_H_ */
