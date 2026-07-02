/* *****************************************************************************
 *  vita.h                                                          2014 AUG 31
 *
 *      Describes VITA 49 structures
 *
 *  \date 2012-03-28
 *  \author Eric Wachsmann, KE5DTO
 *
 * *****************************************************************************
 *
 *  Copyright (C) 2012-2014 FlexRadio Systems.
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

#ifndef _VITA_H
#define _VITA_H

#include <linux/if_ether.h>

#include "datatypes.h"

/* Packet Header Definitions */
#define VITA_HEADER_PACKET_TYPE_MASK                0xF0000000
#define VITA_PACKET_TYPE_IF_DATA                    0x00000000
#define VITA_PACKET_TYPE_IF_DATA_WITH_STREAM_ID     0x10000000
#define VITA_PACKET_TYPE_EXT_DATA                   0x20000000
#define VITA_PACKET_TYPE_EXT_DATA_WITH_STREAM_ID    0x30000000
#define VITA_PACKET_TYPE_CONTEXT                    0x40000000
#define VITA_PACKET_TYPE_EXT_CONTEXT                0x50000000

#define VITA_HEADER_C_MASK                          0x08000000
#define VITA_HEADER_CLASS_ID_PRESENT                0x08000000

#define VITA_HEADER_T_MASK                          0x04000000
#define VITA_HEADER_TRAILER_PRESENT                 0x04000000

#define VITA_HEADER_TSI_MASK                        0x00C00000
#define VITA_TSI_NONE                               0x00000000
#define VITA_TSI_UTC                                0x00400000
#define VITA_TSI_GPS                                0x00800000
#define VITA_TSI_OTHER                              0x00C00000

#define VITA_HEADER_TSF_MASK                        0x00300000
#define VITA_TSF_NONE                               0x00000000
#define VITA_TSF_SAMPLE_COUNT                       0x00100000
#define VITA_TSF_REAL_TIME                          0x00200000
#define VITA_TSF_FREE_RUNNING                       0x00300000

#define VITA_HEADER_PACKET_COUNT_MASK               0x000F0000
#define VITA_HEADER_PACKET_SIZE_MASK                0x0000FFFF

#define VITA_CLASS_ID_OUI_MASK                      0x00FFFFFF
#define VITA_CLASS_ID_INFORMATION_CLASS_MASK        0xFFFF0000
#define VITA_CLASS_ID_PACKET_CLASS_MASK             0x0000FFFF

#pragma pack(4)

// 16 ip header
#define MAX_TCP_DATA_SIZE (ETH_DATA_LEN-16)

// 16 ip header, 6 udp header
#define MAX_UDP_DATA_SIZE (2048)

#define MAX_IF_DATA_PAYLOAD_SIZE (MAX_UDP_DATA_SIZE) //-28)
typedef struct _vita_if_data
{
    uint32 header;
    uint32 stream_id;
    uint32 class_id_h;
    uint32 class_id_l;
    uint32 timestamp_int;
    uint32 timestamp_frac_h;
    uint32 timestamp_frac_l;
    uint8  payload[MAX_IF_DATA_PAYLOAD_SIZE];
} vita_if_data, *VitaIFData;

#define MAX_METER_DATA_PAYLOAD_SIZE (MAX_UDP_DATA_SIZE) //-28)
typedef struct _vita_meter_data
{
    uint32 header;
    uint32 stream_id;
    uint32 class_id_h;
    uint32 class_id_l;
    uint32 timestamp_int;
    uint32 timestamp_frac_h;
    uint32 timestamp_frac_l;
    uint8  payload[MAX_METER_DATA_PAYLOAD_SIZE];
} vita_meter_data, *VitaMeterData;

#define MAX_METER_PAYLOAD_SIZE (MAX_UDP_DATA_SIZE) //-36)
typedef struct _vita_ext_data_meter
{
    uint32 header;
    uint32 streamID;
    uint32 classID_1;
    uint32 classID_2;
    uint32 integer_seconds;
    uint64 frac_seconds;
    uint32 extended_packet_type; // = 1 is meter
    uint32 number_of_meters;
    uint8  payload[MAX_METER_PAYLOAD_SIZE];
} vita_ext_data_meter, *VitaExtDataMeter;

typedef struct _vita_timestamp
{
    uint32 ts_int;
    union
    {
        struct
        {
            uint32 ts_frac_h;
            uint32 ts_frac_l;
        };
        uint64 ts_frac;
    };

} vita_timestamp, *VitaTimestamp;

typedef uint32 vita_date;

#pragma pack()

#endif /* _VITA_H */
