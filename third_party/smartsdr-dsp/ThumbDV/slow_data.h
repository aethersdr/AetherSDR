///*!   \file slow_data.h
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

#ifndef THUMBDV_SLOW_DATA_H_
#define THUMBDV_SLOW_DATA_H_

#include "dstar.h"
#include "common.h"
#include "DStarDefines.h"

void slow_data_addDecodeData(DSTAR_MACHINE dstar, unsigned char * data, uint32 data_len);
void slow_data_resetDecoder(DSTAR_MACHINE dstar);
void slow_data_resetEncoder(DSTAR_MACHINE dstar);
void slow_data_getEncodeBytes( DSTAR_MACHINE dstar, unsigned char * bytes, uint32 num_bytes );
void slow_data_createEncodeBytes(DSTAR_MACHINE dstar);
#endif /* THUMBDV_SLOW_DATA_H_*/
