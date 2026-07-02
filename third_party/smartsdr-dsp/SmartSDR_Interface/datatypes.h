/* *****************************************************************************
 * 	datatypes.h
 *
 *		datatypes definition file
 *
 *
 *     \date Sep 15, 2011
 *	\author Eric
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


#ifndef _DATATYPES_H
#define _DATATYPES_H


typedef unsigned char  uint8;
typedef unsigned short uint16;
typedef unsigned int   uint32;
typedef unsigned long long uint64;

typedef signed char int8;
typedef signed short int16;
typedef signed int int32;
typedef signed long long int64;

typedef uint8 BOOL;

/// VITA-49 format frequency data
typedef int64 VITAfrequency;
typedef int16 VITAfrequency_trunc;
typedef int32 VITAdb;
typedef int16 VITAdb_trunc;
typedef uint32 packedVITAcalPoint;
typedef int32 VITAtemp;
typedef int16 VITAtemp_trunc;


typedef uint32 ant_port_id_type;

#define TRUE (uint8)1
#define FALSE (uint8)0

#define INVALID -1

#endif // _DATATYPES_H
