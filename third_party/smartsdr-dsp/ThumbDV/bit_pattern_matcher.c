///*!   \file bit_pattern_matcher.c
// *    \brief Allows matching of a pattern in an array ob gits.
// *
// *    \copyright  Copyright 2012-2014 FlexRadio Systems.  All Rights Reserved.
// *                Unauthorized use, duplication or distribution of this software is
// *                strictly prohibited by law.
// *
// *    \date 26-MAY-2015
// *    \author     Ed Gonzalez
// *
// *
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
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "bit_pattern_matcher.h"

BIT_PM bitPM_create( const BOOL * to_match, uint32 length ) {
    BIT_PM bpm = ( BIT_PM ) safe_malloc( sizeof( bit_pm ) );

    bpm->pattern = ( BOOL * ) safe_malloc( sizeof( BOOL ) * length );
    bpm->data = ( BOOL * ) safe_malloc( sizeof( BOOL ) * length );
    bpm->length = length;
    bpm->data_length = 0;

    /* We have to put the pattern in reverse since
     * the samples are fed left to right
     */
    uint32 i = 0;
    uint32 n = length - 1;

    for ( i = 0 ; i < length; i++ ) {
        bpm->pattern[n--] = to_match[i];
    }

    output( "Creating pattern matcher !!!!!\n" );

    for ( i = 0 ; i < length ; i++ ) {
        output( "%d", bpm->pattern[i] );
    }

    output( "\n" );

    for ( i = 0 ; i < length ; i++ ) {
        output( "%d", to_match[i] );
    }

    output( "\n" );


    memset( bpm->data, 0, length * sizeof( BOOL ) );

    return bpm;
}

void bitPM_destroy( BIT_PM bpm ) {
    safe_free( bpm->data );
    safe_free( bpm->pattern );
    safe_free( bpm );
}

void bitPM_reset( BIT_PM bpm ) {
    bpm->data_length = 0;
    memset( bpm->data, 0, bpm->length * sizeof( BOOL ) );
}

BOOL bitPM_addBit( BIT_PM bpm, BOOL bit ) {
    uint32 i = 0;

    /* Shift the existing buffer to make space for new bit */
    for ( i = bpm->length - 1; i >= 1 ; i-- ) {
        bpm->data[i] = bpm->data[i - 1];
    }

    bpm->data[0] = bit;

    if ( bpm->data_length < bpm->length ) {
        bpm->data_length++;
    }

    if ( bpm->data_length != bpm->length ) {
        /* If not enough data has accumulated then simply return FALSE */
        return FALSE;
    }

    for ( i = 0; i < bpm->length ; i++ ) {
        if ( bpm->pattern[i] != bpm->data[i] ) {
            return FALSE;
        }
    }

#ifdef DEBUG_BIT_PM
    output( ANSI_GREEN "Match Found\nPat: " );

    for ( i = 0; i < bpm->length ; i++ ) {
        output( "%d ", bpm->pattern[i] );
    }

    output( "\nMat: " );

    for ( i = 0; i < bpm->length ; i++ ) {
        output( "%d ", bpm->data[i] );
    }

    output( "\n" );


#endif

    /* If we make it here all checks have passed */
    return TRUE;
}
