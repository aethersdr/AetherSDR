/* *****************************************************************************
 *	hal_buffer.c
 *
 *		Buffer structures to support getting samples from the right stream to
 *    		the DSP.
 *
 *
 *	\date 29-MAR-2012
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h> // for memset
#include <math.h>

#include "common.h"
//#include "platform.h"
#include "hal_buffer.h"

BufferDescriptor hal_BufferRequest(uint32 num_samples, uint32 sample_size)
{
	// allocate memory for the new object
//	BufferDescriptor buf = (BufferDescriptor)safe_malloc(sizeof(buffer_descriptor));
	BufferDescriptor buf = (BufferDescriptor)safe_malloc(sizeof(buffer_descriptor));		// substitute non-thread-safe malloc
//	debug(LOG_DEV, TRUE, "\033[32m+ buf_desc alloc: %08X %04X\033[m", (uint32)buf, sizeof(buffer_descriptor));
	if(!buf)
	{
//		debug(LOG_DEV, TRUE, "Error allocating buffer descriptor (size=%u)", sizeof(buffer_descriptor));
		return 0;
	}

	// clear memory of new descriptor object
	memset(buf, 0, sizeof(buffer_descriptor));

	// initialize size and allocate buffer
	buf->num_samples = num_samples;
	buf->sample_size = sample_size;
	const size_t buffer_bytes = (size_t)num_samples * sample_size;
//	buf->buf_ptr = safe_malloc(num_samples * sample_size);
	buf->buf_ptr = safe_malloc(buffer_bytes);	// substitute non-thread-safe malloc
//	debug(LOG_DEV, TRUE, "\033[35m+     buf alloc: %08X, %04X\033[m", (uint32)buf->buf_ptr, num_samples * sample_size);
	if(!buf->buf_ptr)
	{
//		debug(LOG_DEV, TRUE, "Error allocating buffer descriptor (size=%u)", num_samples * sample_size);
//		safe_free(buf); // prevent memory leak
		free(buf);		// un-thread-safe free
		buf = NULL;
		return NULL;
	}

	// clear memory of new buffer object
	memset(buf->buf_ptr, 0, buffer_bytes);

	return buf;
}

void hal_BufferRelease(BufferDescriptor *buf_desc)
{
	if(*buf_desc)
	{
		if((*buf_desc)->buf_ptr != NULL)
		{
//			debug(LOG_DEV, TRUE, "\033[35m-     releasing buf: %08X\033[m", (uint32)(*buf_desc)->buf_ptr);
//			safe_free((*buf_desc)->buf_ptr);
			free((*buf_desc)->buf_ptr);		// un-thread-safe free
			(*buf_desc)->buf_ptr = NULL;
		}

//		debug(LOG_DEV, TRUE, "\033[32m- releasing buf_desc: %08X\033[m", (uint32)*buf_desc);
//		safe_free(*buf_desc);
		free(*buf_desc);		// un-thread-safe free
		*buf_desc = NULL;
	}
}

BufferDescriptor hal_BufferClone(BufferDescriptor buf)
{
	BufferDescriptor new_buf = hal_BufferRequest(buf->num_samples, buf->sample_size);
	if(!new_buf)
	{
//		debug(LOG_DEV, TRUE, "Error allocating new buffer");
		return 0;
	}

	// copy member variables
	new_buf->stream_id = buf->stream_id;
	new_buf->timestamp_int = buf->timestamp_int;
	new_buf->timestamp_frac_h = buf->timestamp_frac_h;
	new_buf->timestamp_frac_l = buf->timestamp_frac_l;
	new_buf->vita_header = buf->vita_header;
	new_buf->vita_packet_count = buf->vita_packet_count;
	new_buf->vita_missing_packets = buf->vita_missing_packets;
	new_buf->arrival_monotonic_ns = buf->arrival_monotonic_ns;
	new_buf->correlation_id = buf->correlation_id;

	// copy the actual buffer
	memcpy(new_buf->buf_ptr,
		   buf->buf_ptr,
		   (size_t)buf->num_samples * buf->sample_size);

	return new_buf;
}

void hal_BufferPrint(BufferDescriptor buf_desc)
{
	int i;
	for(i=0; i<16; i++)
		printf("%.2f ", ((float*)buf_desc->buf_ptr)[i]);
}

float hal_BufferMag(BufferDescriptor buf_desc)
{
	int i;
	float sum = 0.0f;

	for(i=0; i<buf_desc->num_samples*2; i++)
		sum += fabs(((float*)buf_desc->buf_ptr)[i]);

	return sum;
}
