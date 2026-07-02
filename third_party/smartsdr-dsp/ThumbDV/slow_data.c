///*!   \file slow_data.c
// *
// *    Handles scrambling and descrambling of DSTAR Header
// *
// *    \date 25-AUG-2015
// *    \author Ed Gonzalez KG5FBT
// */

/* *****************************************************************************
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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slow_data.h"
#include "DStarDefines.h"
#include "dstar.h"
#include "gmsk_modem.h"
#include "thumbDV.h"

uint32 slow_data_createDecoder(SLOW_DATA_DECODER slow_decoder)
{
    SLOW_DATA_DECODER decoder = ( SLOW_DATA_DECODER ) safe_malloc(sizeof(slow_data_decoder)) ;

    if ( decoder == NULL ) {
        output("Could not allocate slow data decoder\n");
        return FAIL;
    }

    memset(decoder, 0, sizeof(slow_data_decoder));

    decoder->decode_state = FIRST_FRAME;

    slow_decoder = decoder;
    return SUCCESS;
}

static void _slow_data_processHeaderBytes(DSTAR_MACHINE dstar)
{
    uint32 i = 0;
    dstar_pfcs pfcs;
    pfcs.crc16 = 0xFFFF;

    SLOW_DATA_DECODER slow_decoder = (SLOW_DATA_DECODER) dstar->slow_decoder;

    BOOL bits[RADIO_HEADER_LENGTH_BITS] = {0};
    gmsk_bytesToBits(slow_decoder->header_bytes, bits, RADIO_HEADER_LENGTH_BITS);

    for ( i = 0 ; i < 312 ; i += 8 ) {
      dstar_pfcsUpdate( &pfcs, bits + i);
    }

    BOOL pfcs_match = FALSE;
    pfcs_match = dstar_pfcsCheck( &pfcs, bits + 312 );

    if ( pfcs_match ) {
        output("SLOW DATA HEADER PARSED\n");

      dstar_processHeader(slow_decoder->header_bytes, &dstar->incoming_header);
      dstar_updateStatus( dstar, dstar->slice, STATUS_RX );
    }

    slow_data_resetDecoder(dstar);
}

static void _slow_data_processMessage(DSTAR_MACHINE dstar)
{
    char message[21];
    uint32 i, j;
    for ( i = 0 ; i < 4 ; i++ ) {
        for ( j = 0 ; j < 5 ; j++ ) {
            message[(i*5) + j] = dstar->slow_decoder->message[i][j];
        }
    }

    message[20] = '\0';
    strncpy(dstar->slow_decoder->message_string, message, 21);
    output("SLOW DATA MESSAGE PARSED = '%s'\n", message);
    dstar_updateStatus(dstar, dstar->slice, STATUS_SLOW_DATA_MESSAGE);

    slow_data_resetDecoder(dstar);
}

void slow_data_createEncodeBytes(DSTAR_MACHINE dstar)
{
    uint32 i = 0;
    uint32 j = 0;
    SLOW_DATA_ENCODER encoder = dstar->slow_encoder;
    uint32 message_index = 0;
    dstar_pfcs pfcs;

    /* Set all bytes to 0x66 */
    memset(encoder->message_bytes, 0x66, SLOW_DATA_PACKET_LEN_BYTES * FRAMES_BETWEEN_SYNC);

    /* Generate Message Bytes */
    for ( i = 0 ; i < 4 ; i++ ) {
        encoder->message_bytes[(i * 6)] = SLOW_DATA_TYPE_MESSAGE | i;
        for ( j = 1 ; j < 6 ; j++ ) {
            encoder->message_bytes[(i * 6) + j] = encoder->message[message_index++];
        }
    }

    thumbDV_dump("MESSAGE BYTES", encoder->message_bytes, SLOW_DATA_PACKET_LEN_BYTES * FRAMES_BETWEEN_SYNC);

    /* Generate HEADER Bytes */
    /* Set all bytes to 0x66 */
    memset(encoder->header_bytes, 0x66, SLOW_DATA_PACKET_LEN_BYTES * FRAMES_BETWEEN_SYNC);
    uint32 header_index = 0;

    unsigned char header_bytes[RADIO_HEADER_LENGTH_BYTES] = {0};
    dstar_headerToBytes(&dstar->outgoing_header, header_bytes);
    pfcs.crc16 = 0xFFFF;
    dstar_pfcsUpdateBuffer( &pfcs, header_bytes, 312 / 8 );
    dstar_pfcsResult( &pfcs, header_bytes + 312 / 8 );

    uint32 bits_left = RADIO_HEADER_LENGTH_BYTES;
    uint32 second_loop_limit = 0;
    for ( i = 0 ; i < RADIO_HEADER_LENGTH_BYTES / 5 + 1; i++ ) {
        if ( bits_left >= 5 )
            second_loop_limit = 6;
        else
            second_loop_limit = bits_left;
        encoder->header_bytes[(i * 6)] = SLOW_DATA_TYPE_HEADER | (second_loop_limit - 1);
        for ( j = 1 ; j < second_loop_limit ; j++ ) {
            encoder->header_bytes[(i * 6) + j] = header_bytes[header_index++];
        }

        bits_left -= 5;
    }

    thumbDV_dump("Header bytes", encoder->header_bytes, SLOW_DATA_PACKET_LEN_BYTES * FRAMES_BETWEEN_SYNC);


    slow_data_resetEncoder(dstar);


}

void slow_data_getEncodeBytes( DSTAR_MACHINE dstar, unsigned char * bytes , uint32 num_bytes)
{
    uint32 i = 0;
    memset(bytes, 0x66, num_bytes);

    SLOW_DATA_ENCODER encoder = dstar->slow_encoder;
    switch ( encoder->encode_state ) {
    case MESSAGE_TX:
        if ( encoder->message_index != 0 &&  encoder->message_index % 3 != 0 ) {
            output("Message indexing problem. message_index = %d\n", encoder->message_index);
            slow_data_resetEncoder(dstar);
            break;
        }

        for ( i = 0 ; i < SLOW_DATA_PACKET_LEN_BYTES ; i++ ) {
            bytes[i] = encoder->message_bytes[encoder->message_index++];
        }

        if ( encoder->message_index >= SLOW_DATA_PACKET_LEN_BYTES * FRAMES_BETWEEN_SYNC ) {
            /* Done sending the message change to header state */
            encoder->message_index = 0;
            encoder->encode_state = HEADER_TX;
        }

        break;
    case HEADER_TX:
        if ( encoder->header_index != 0 &&  encoder->header_index % 3 != 0 ) {
            output("Header indexing problem. header_index = %d\n", encoder->header_index);
            slow_data_resetEncoder(dstar);
            break;
        }

        for ( i = 0 ; i < SLOW_DATA_PACKET_LEN_BYTES ; i++ ) {
            bytes[i] = encoder->header_bytes[encoder->header_index++];
        }

        if ( encoder->header_index >= SLOW_DATA_PACKET_LEN_BYTES * FRAMES_BETWEEN_SYNC ) {
            /* Done sending header. Reset index and keep sending header slow data */
            encoder->header_index = 0;
        }
    break;
    }
}

void slow_data_resetEncoder(DSTAR_MACHINE dstar)
{
    dstar->slow_encoder->encode_state = MESSAGE_TX;
    dstar->slow_encoder->message_index = 0;
    dstar->slow_encoder->header_index = 0;
}

void slow_data_resetDecoder(DSTAR_MACHINE dstar)
{
    dstar->slow_decoder->decode_state = FIRST_FRAME;
    dstar->slow_decoder->header_array_index = 0;
    dstar->slow_decoder->message_index = 0;
}

void slow_data_addDecodeData(DSTAR_MACHINE dstar, unsigned char * data, uint32 data_len)
{
    if ( data_len != SLOW_DATA_PACKET_LEN_BYTES ) {
        output("Invalid data length - slow_data_addData\n");
        return;
    }

    SLOW_DATA_DECODER slow_decoder = (SLOW_DATA_DECODER) (dstar->slow_decoder);

    if ( slow_decoder == NULL ) {
        output("NULL slow_decoder\n");
        return;
    }

    uint32 i = 0;

    switch(slow_decoder->decode_state) {
    case FIRST_FRAME:
    {
        //output("FIRST FRAME\n");
        switch(data[0] & SLOW_DATA_TYPE_MASK ) {
        case SLOW_DATA_TYPE_HEADER:
        {
            for ( i = 1 ; i < 3 ; i++ ) {
                slow_decoder->header_bytes[slow_decoder->header_array_index++] =
                        data[i];
                if ( slow_decoder->header_array_index >= RADIO_HEADER_LENGTH_BYTES )
                {
                    _slow_data_processHeaderBytes(dstar);
                    break;
                }
            }

            slow_decoder->decode_state = HEADER_SECOND_FRAME;


            break;
        }
        case SLOW_DATA_TYPE_MESSAGE:
        {

            uint32 message_index = data[0] & SLOW_DATA_LENGTH_MASK;

            if ( message_index != slow_decoder->message_index ) {
                output("Out of order SLOW DATA MESSAGE setting to new index\n");
                slow_decoder->message_index = message_index;
                break;
            }

            for ( i = 1 ; i < 3 ; i++ ) {
                slow_decoder->message[message_index][i - 1 ] = data[i];
            }

            slow_decoder->decode_state = MESSAGE_SECOND_FRAME;

            break;
        }
        default:
            //output("SLOW DATA BYTES 0x%X 0x%X  0x%X \n", data[0], data[1], data[2]);
            break;
        }
        break;
    }
    case HEADER_SECOND_FRAME:
    {
        //output("HEADER SECOND FRAME\n");
        if ( slow_decoder->header_array_index == 0 ) {
            /* We reached the end of the array so we need to reset and find the first frame again */
            slow_decoder->decode_state = FIRST_FRAME;
            break;
        }

        for ( i = 0 ; i < 3 ; i++ ) {
           slow_decoder->header_bytes[slow_decoder->header_array_index++] =
                   data[i];
           if ( slow_decoder->header_array_index >= RADIO_HEADER_LENGTH_BYTES )
           {
               _slow_data_processHeaderBytes(dstar);
               break;
           }
        }

        slow_decoder->decode_state = FIRST_FRAME;

        break;
    }
    case MESSAGE_SECOND_FRAME:
    {
        for ( i = 0 ; i < 3 ; i++ ) {
            slow_decoder->message[slow_decoder->message_index][i+2] = data[i];
        }

        slow_decoder->message_index++;

        if ( slow_decoder->message_index >= 4 ) {
            _slow_data_processMessage(dstar);
        }

        slow_decoder->decode_state = FIRST_FRAME;

        break;
    }
    }

}
