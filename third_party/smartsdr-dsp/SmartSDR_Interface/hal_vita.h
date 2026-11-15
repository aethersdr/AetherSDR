/* *****************************************************************************
 *	vita.h
 *
 *		Describes VITA 49 structures
 *
 *	\date 28-MAR-2012
 *	\author Eric Wachsmann, KE5DTO
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

#ifndef _HAL_VITA_H
#define _HAL_VITA_H

#include "datatypes.h"
#include "vita.h"

//! gets the size of the payload in bytes based on the header
uint32 hal_VitaIFPacketPayloadSize(VitaIFData packet);

#endif /* _HAL_VITA_H */
