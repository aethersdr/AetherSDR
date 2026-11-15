/*  cmbuffs.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2018 Warren Pratt, NR0V

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

warren@pratt.one

*/

#ifndef _syncbuffs_h
#define _syncbuffs_h
#include "comm.h"

#define SYNCB_MULT		(3)						
typedef struct _syncb
{
	void (*exf)(void);							// pointer to function to execute after output buffer is filled
	double** out;								// pointer to array of output buffers
	int nstreams;								// number of streams of data being buffered
	int   max_in_size;							// max input number of complex samples
	int   max_outsize;							// max output number of complex samples
	int   r1_outsize;							// number of complex samples taken out of the ring for processing 

	int   r1_size;								// size of a single maximum sized transfer
	int   r1_active_buffsize;					// size of ring (in complex samples)
	
	double** r1_baseptr;						// array of pointers, one to each ring
	int   r1_inidx;								// in 'double', actual index into the buffer is 2 times this
	int   r1_outidx;							// in 'double', actual index into the buffer is 2 times this
	int   r1_unqueuedsamps;						// number of input samples not yet queued/released for execution
	volatile long run;							// when 1, thread loops; when 0, thread terminates
	volatile long accept;						// flag indicating whether accepting input data
	HANDLE Sem_BuffReady;						// count = number of output-sized buffers queued for processing
	CRITICAL_SECTION csOUT;						// used to block output while parameters are updated or buffers flushed
	CRITICAL_SECTION csIN;						// used to block input while parameters are updated or buffers flushed
} syncb, *SYNCB;

extern SYNCB create_syncbuffs (int accept, int nstreams, int max_insize, int max_outsize, int outsize, double** out, void (*exf)(void));

extern void destroy_syncbuffs (SYNCB a);

extern void flush_syncbuffs (SYNCB a);

extern void Syncbound (SYNCB a, int nsamples, double** in);	

extern void syncbdata (SYNCB a);

extern void syncb_main (void *p);

extern void SetSYNCBRingOutsize (SYNCB a, int size);

#endif

/********************************************************************************************************
*																										*
*											Dummy Filter												*
*																										*
********************************************************************************************************/

#ifndef _dumfilt_h
#define _dumfilt_h

typedef struct _dumfilt
{
	int run;									// run flag
	int rsize;									// ring size (complex samples)
	int delay;									// delay (complex samples)
	int opsize;									// number of complex samples exchanged per call
	int inidx;									// input index into ring (complex samples)
	int outidx;									// output index from ring (complex samples)
	double* in;									// pointer to input buffer  (may be same as output buffer)
	double* out;								// pointer to output buffer  (may be same as input buffer)
	double* ring;								// pointer to ring buffer
} dumfilt, *DUMFILT;

extern DUMFILT create_dumfilt (int run, int delay, int opsize, double* in, double* out);

extern void destroy_dumfilt (DUMFILT a);

extern void flush_dumfilt (DUMFILT a);

extern void xdumfilt (DUMFILT a);

#endif