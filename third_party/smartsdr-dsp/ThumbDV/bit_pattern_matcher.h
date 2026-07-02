///*!   \file bit_pattern_matcher.h
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


#ifndef THUMBDV_BIT_PATTERN_MATCHER_H_
#define THUMBDV_BIT_PATTERN_MATCHER_H_

#include "datatypes.h"

typedef struct _bit_pattern_matcher {
    BOOL * pattern;
    BOOL * data;
    uint32 length;
    uint32 data_length;
} bit_pm, * BIT_PM;

void bitPM_destroy( BIT_PM bpm );
BIT_PM bitPM_create( const BOOL * to_match, uint32 length );

BOOL bitPM_addBit( BIT_PM bpm, BOOL bit );
void bitPM_reset( BIT_PM bpm );

#endif /* THUMBDV_BIT_PATTERN_MATCHER_H_ */
