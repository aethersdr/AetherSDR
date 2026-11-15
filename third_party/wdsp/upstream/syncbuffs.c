/*  syncbuffs.c

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

#include "comm.h"

void start_syncbthread (SYNCB a)
{
	HANDLE handle = (HANDLE) _beginthread(syncb_main, 0, (void *)a);
	SetThreadPriority (handle, THREAD_PRIORITY_HIGHEST);
}

SYNCB create_syncbuffs (int accept, int nstreams, int max_insize, int max_outsize, int outsize, double** out, void (*exf)(void))
{
	SYNCB a = (SYNCB) malloc0 (sizeof(syncb));
	int i;
	a->accept = accept;
	a->nstreams = nstreams;
	a->run = 1;
	a->max_in_size = max_insize;
	a->max_outsize = max_outsize;
	a->r1_outsize = outsize;
	a->out = out;
	a->exf = exf;
	if (a->max_outsize > a->max_in_size)
		a->r1_size = a->max_outsize;
	else
		a->r1_size = a->max_in_size;
	a->r1_active_buffsize = SYNCB_MULT * a->r1_size;
	a->r1_baseptr = (double **) malloc0 (a->nstreams * sizeof (double *));
	for (i = 0; i < a->nstreams; i++)
		a->r1_baseptr[i] = (double *) malloc0 (a->r1_active_buffsize * sizeof (complex));
	a->r1_inidx = 0;
	a->r1_outidx = 0;
	a->r1_unqueuedsamps = 0;
	a->Sem_BuffReady = CreateSemaphore(0, 0, 1000, 0);
	InitializeCriticalSectionAndSpinCount ( &a->csIN, 2500 );
	InitializeCriticalSectionAndSpinCount ( &a->csOUT,  2500 );
	start_syncbthread (a);
	return a;
}

void destroy_syncbuffs (SYNCB a)
{
	int i;
	InterlockedBitTestAndReset(&a->accept, 0);		// shut the Syncbound() gate to prevent new infusions
	EnterCriticalSection (&a->csIN);				// wait until the current Inbound() infusion is finished
	EnterCriticalSection (&a->csOUT);				// block the syncb thread before syncbdata()
	Sleep (25);										// wait for the thread to arrive at the top of the syncb_main() loop
	InterlockedBitTestAndReset(&a->run, 0);			// set a trap for the syncb thread
	ReleaseSemaphore(a->Sem_BuffReady, 1, 0);		// be sure the syncb thread can pass WaitForSingleObject in syncb_main()									// 
	LeaveCriticalSection (&a->csOUT);				// let the thread pass to the trap in syncbdata()
	LeaveCriticalSection (&a->csIN);				
	Sleep (2);										// wait for the syncb thread to die
	DeleteCriticalSection (&a->csOUT);
	DeleteCriticalSection (&a->csIN);
	CloseHandle (a->Sem_BuffReady);
	for (i = 0; i < a->nstreams; i++)
		_aligned_free (a->r1_baseptr[i]);
	_aligned_free (a->r1_baseptr);
	_aligned_free (a);
}

void flush_syncbuffs (SYNCB a)
{
	int i;
	for (i = 0; i < a->nstreams; i++)
		memset (a->r1_baseptr[i], 0, a->r1_active_buffsize * sizeof (complex));
	a->r1_inidx = 0;
	a->r1_outidx = 0;
	a->r1_unqueuedsamps = 0;
	while (!WaitForSingleObject (a->Sem_BuffReady, 1)) ;
}

void Syncbound (SYNCB a, int nsamples, double** in)	
{
	int i, n;
	int first, second;

	if (_InterlockedAnd (&a->accept, 1))
	{
		EnterCriticalSection (&a->csIN);
		if (nsamples > (a->r1_active_buffsize - a->r1_inidx))
		{
			first = a->r1_active_buffsize - a->r1_inidx;
			second = nsamples - first;
		}
		else
		{
			first = nsamples;
			second = 0;
		}
		for (i = 0; i < a->nstreams; i++)
		{
			memcpy (a->r1_baseptr[i] + 2 * a->r1_inidx, in[i],             first  * sizeof (complex));
			memcpy (a->r1_baseptr[i],                   in[i] + 2 * first, second * sizeof (complex));
		}
		if ((a->r1_unqueuedsamps += nsamples) >= a->r1_outsize)
		{
			n = a->r1_unqueuedsamps / a->r1_outsize;
			ReleaseSemaphore (a->Sem_BuffReady, n, 0);
			a->r1_unqueuedsamps -= n * a->r1_outsize;
		}
		if ((a->r1_inidx += nsamples) >= a->r1_active_buffsize)
			a->r1_inidx -= a->r1_active_buffsize;
		LeaveCriticalSection (&a->csIN);
	}
}

void syncbdata (SYNCB a)
{
	int i;
	int first, second;
	EnterCriticalSection (&a->csOUT);
	if (!_InterlockedAnd (&a->run, 1)) 
	{
		LeaveCriticalSection (&a->csOUT);
		_endthread();
	}
	if (a->r1_outsize > (a->r1_active_buffsize - a->r1_outidx))
	{
		first = a->r1_active_buffsize - a->r1_outidx;
		second = a->r1_outsize - first;
	}
	else
	{
		first = a->r1_outsize;
		second = 0;
	}
	for (i = 0; i < a->nstreams; i++)
	{
		memcpy (a->out[i],             a->r1_baseptr[i] + 2 * a->r1_outidx, first  * sizeof (complex));
		memcpy (a->out[i] + 2 * first, a->r1_baseptr[i],                    second * sizeof (complex));
	}
	if ((a->r1_outidx += a->r1_outsize) >= a->r1_active_buffsize)
		a->r1_outidx -= a->r1_active_buffsize;
	LeaveCriticalSection (&a->csOUT);
}

void syncb_main (void *p)
{
	SYNCB a = (SYNCB)p;
	
	while (_InterlockedAnd (&a->run, 1))
	{
		WaitForSingleObject (a->Sem_BuffReady,INFINITE);
		syncbdata (a);
		a->exf();
	}
	_endthread();
}

void SetSYNCBRingOutsize (SYNCB a, int size)
{
	InterlockedBitTestAndReset(&a->accept, 0);		// shut the Syncbound() gate to prevent new infusions
	EnterCriticalSection (&a->csIN);				// wait until the current Syncbound() infusion is finished
	EnterCriticalSection (&a->csOUT);				// block the syncb thread before syncbdata()
	Sleep (25);										// wait for the thread to arrive at the top of the syncb_main() loop
	InterlockedBitTestAndReset(&a->run, 0);			// set a trap for the syncb thread
	ReleaseSemaphore(a->Sem_BuffReady, 1, 0);		// be sure the syncb thread can pass WaitForSingleObject in syncb_main()									// 
	LeaveCriticalSection (&a->csOUT);				// let the thread pass to the trap in syncbdata()
	Sleep (2);										// wait for the syncb thread to die
	flush_syncbuffs(a);								// restore ring to pristine condition
	a->r1_outsize = size;							// set its new outsize
	InterlockedBitTestAndSet(&a->run,0);			// remove the syncb thread trap
	start_syncbthread(a);							// start the syncb thread
	LeaveCriticalSection (&a->csIN);				// enable Syncbound() processing
	InterlockedBitTestAndSet(&a->accept, 0);		// open the Syncbound() gate
}

/********************************************************************************************************
*																										*
*											Dummy Filter												*
*																										*
********************************************************************************************************/

DUMFILT create_dumfilt (int run, int delay, int opsize, double* in, double* out)
{
	DUMFILT a = (DUMFILT) malloc0 (sizeof (dumfilt));
	a->run = run;
	a->delay = delay;
	a->opsize = opsize;
	a->rsize = a->opsize + a->delay;
	a->in = in;
	a->out = out;
	a->ring = (double *) malloc0 (a->rsize * sizeof (complex));
	a->outidx = 0;
	a->inidx = a->delay;
	return a;
}

void destroy_dumfilt (DUMFILT a)
{
	_aligned_free (a->ring);
	_aligned_free (a);
}

void flush_dumfilt (DUMFILT a)
{
	memset (a->ring, 0, a->rsize * sizeof (complex));
	a->outidx = 0;
	a->inidx = a->delay;
}

void xdumfilt (DUMFILT a)
{
	int first, second;
	if (a->run)
	{
		if (a->opsize > (a->rsize - a->inidx))
		{
			first = a->rsize - a->inidx;
			second = a->opsize - first;
		}
		else
		{
			first = a->opsize;
			second = 0;
		}
		memcpy (a->ring + 2 * a->inidx, a->in,             first  * sizeof (complex));
		memcpy (a->ring,                a->in + 2 * first, second * sizeof (complex));
		if ((a->inidx += a->opsize) > a->rsize) a->inidx -= a->rsize;
		if (a->opsize > a->rsize - a->outidx)
		{
			first = a->rsize - a->outidx;
			second = a->opsize - first;
		}
		else
		{
			first = a->opsize;
			second = 0;
		}
		memcpy (a->out,             a->ring + 2 * a->outidx, first  * sizeof (complex));
		memcpy (a->out + 2 * first, a->ring,                 second * sizeof (complex));
		if ((a->outidx += a->opsize) > a->rsize) a->outidx -= a->rsize;
	}
}