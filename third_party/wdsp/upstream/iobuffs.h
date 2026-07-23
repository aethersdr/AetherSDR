/*  iobuffs.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013 Warren Pratt, NR0V

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

The author can be reached by email at  

warren@wpratt.com

*/

#ifndef _iobuffs_h
#define _iobuffs_h
#include "comm.h"
typedef struct _iobf
{
	int   channel;
	int   in_size;								// input number of complex samples in a fexchange call
	int   out_size;								// output number of complex samples in a fexchange call
	int   r1_outsize;							// number of complex samples taken out of the input-pseudo-ring for processing 
	int   r2_insize;							// number of processed complex samples returned into the output-pseudo-ring
	int   r1_size;								// size of a single maximum sized transfer
	int   r2_size;								// size of a single maximum sized transfer
	int   r1_active_buffsize;					// size of input pseudo-ring (in complex samples)
	int   r2_active_buffsize;					// size of output pseudo-ring (in complex samples)
	
	double* r1_baseptr;							// pointer to input pseudo-ring
	int   r1_inidx;								// in 'double', actual index into the buffer is 2 times this
	int   r1_outidx;							// in 'double', actual index into the buffer is 2 times this
	int   r1_unqueuedsamps;						// number of input samples not yet queued/released for execution

	double* r2_baseptr;							// pointer to output pseudo-ring
	int   r2_inidx;								// in 'double', actual index into the buffer is 2 times this
	int   r2_outidx;							// in 'double', actual index into the buffer is 2 times this
	int   r2_havesamps;							// number of processed samples in output pseudo-ring
	int   r2_unqueuedsamps;						// number of output samples not yet queued / released for output
	CRITICAL_SECTION r2_ControlSection;

	int bfo;									// block_for_output, wait until output is available before proceeding
	HANDLE Sem_OutReady;						// count = number of 'out_size' buffers processed and available for output
	HANDLE Sem_BuffReady;						// count = number of 'dsp_size' buffers queued for processing
	volatile long exec_bypass;
	volatile long flush_bypass;
	HANDLE Sem_Flush;
	struct
	{
		int ustate;
		int dstate;
		int ucount;
		int dcount;
		int ndelup;
		int ntup;
		double* cup;
		int ndeldown;
		int ntdown;
		double* cdown;
		volatile long upflag;
		volatile long downflag;
	} slew;
} iob, *IOB;

extern void create_slews (IOB a);

extern void destroy_slews (IOB a);

extern void flush_slews (IOB a);

extern void create_iobuffs (int channel);

extern void destroy_iobuffs (int channel);

extern void flush_iobuffs (int channel);

PORT	// double, interleaved I/Q
void fexchange0 (int channel, double* in, double* out, int* error);	

PORT	// separate I/Q buffers
extern void fexchange2 (int channel, INREAL *Iin, INREAL *Qin, OUTREAL *Iout, OUTREAL *Qout, int* error);

extern void dexchange (int channel, double* in, double* out);

#endif