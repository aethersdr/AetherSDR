/* *****************************************************************************
 *	hal_buffer.h
 *
 *		Buffer structures to support getting samples from the right stream to
 *			the DSP.
 *
 *	\date Mar 29, 2012
 *	\author Eric & Steve
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

#ifndef _BUFFER_H
#define _BUFFER_H

#include <pthread.h>

#include "datatypes.h"

#pragma pack(4)

///@{
//! buffer_descriptor
//! \brief describes a VITA-49 buffer received for processing and decomposed
typedef struct _buffer_descriptor
{
	//! stream id showing where this buffer came from
	uint32 stream_id;
	//! number of samples stored in the buffer
	uint32 num_samples; // in frames
	//! size in bytes of the sample
	uint32 sample_size;
	//! pointer to the buffer containing the samples
	void* buf_ptr; // pointer to the actual buffer
	//! integer timestamp for the first sample in the buffer (see VITA-49 spec)
	uint32 timestamp_int;
	//! high 32-bits of fractional portion of the timestamp
	uint32 timestamp_frac_h;
	//! low 32-bits of the fractional portion of the timestamp
	uint32 timestamp_frac_l;
	//! pointer to next buffer descriptor
	struct _buffer_descriptor* next;
	//! pointer to previous buffer descriptor
	struct _buffer_descriptor* prev;
} buffer_descriptor, *BufferDescriptor;
///@}

#pragma pack()

//! Requests a Buffer to be used in the DSP
//! \param size Number of frames to allocate
BufferDescriptor hal_BufferRequest(uint32 size, uint32 sample_size);

//! To be called once finished with the buffer (cleanup)
//! \param buf Buffer that is no longer in use
void hal_BufferRelease(BufferDescriptor *buf);

//! Does a deep copy of the buffer descriptor including the buffer
//! \param buf The buffer descriptor to copy
BufferDescriptor hal_BufferClone(BufferDescriptor buf);

void hal_BufferPrint(BufferDescriptor buf_desc);
float hal_BufferMag(BufferDescriptor buf_desc);

#endif // _BUFFER_H
