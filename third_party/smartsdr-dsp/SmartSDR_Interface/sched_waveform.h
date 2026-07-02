///*!   \file sched_waveform.h
// *    \brief Schedule Wavefrom Streams
// *
// *    \copyright  Copyright 2012-2014 FlexRadio Systems.  All Rights Reserved.
// *                Unauthorized use, duplication or distribution of this software is
// *                strictly prohibited by law.
// *
// *    \date 29-AUG-2014
// *    \author Ed Gonzalez
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

#ifndef SCHED_WAVEFORM_H_
#define SCHED_WAVEFORM_H_

#include "hal_buffer.h"
#include "ftd2xx.h"

void sched_waveform_Schedule(BufferDescriptor buf);
void sched_waveform_Init(void);
void sched_waveform_signal(void);
void sched_waveformTreadExit(void);

void sched_waveform_setDestinationRptr(uint32 slice , const char * destination_rptr );
void sched_waveform_setDepartureRptr(uint32 slice , const char * departure_rptr );
void sched_waveform_setCompanionCall( uint32 slice, const char * companion_call);
void sched_waveform_setOwnCall1( uint32 slice , const char * owncall1 );
void sched_waveform_setOwnCall2(uint32 slice , const char * owncall2 );
void sched_waveform_setMessage( uint32 slice, const char * message);

void sched_waveform_sendStatus(uint32 slice);
void sched_waveform_setHandle( FT_HANDLE * handle );
void sched_waveform_setEndOfTX(BOOL end_of_transmission);
void sched_waveform_setDSTARSlice( uint32 slice );
#endif /* SCHED_WAVEFORM_H_ */
