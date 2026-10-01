/*  iobuffs.c

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

#include "comm.h"

// AetherSDR patch 13: the port's test-only pause, called from dexchange().
// Declared here rather than in a port header so the same line serves the POSIX
// port and the native Windows build alike.
void wdspPortHandoffPauseForTest (void);

/********************************************************************************************************
*																										*
*										    Begin Slew Code												*
*																										*
********************************************************************************************************/

enum _slew
{
	BEGIN = 0,
	DELAYUP,
	UPSLEW,
	ON,
	DELAYDOWN,
	DOWNSLEW,
	ZERO,
	OFF
};

void create_slews (IOB a)
{
	int i;
	double delta, theta;
	a->slew.ustate = BEGIN;
	a->slew.dstate = BEGIN;
	a->slew.ucount = 0;
	a->slew.dcount = 0;
	a->slew.ndelup = (int)(ch[a->channel].tdelayup * ch[a->channel].in_rate);
	a->slew.ndeldown = (int)(ch[a->channel].tdelaydown * ch[a->channel].out_rate);
	a->slew.ntup = (int)(ch[a->channel].tslewup * ch[a->channel].in_rate);
	a->slew.ntdown = (int)(ch[a->channel].tslewdown * ch[a->channel].out_rate);
	a->slew.cup   = (double *) malloc0 ((a->slew.ntup + 1) * sizeof (double));
	a->slew.cdown = (double *) malloc0 ((a->slew.ntdown + 1) * sizeof (double));

	delta = PI / (double)a->slew.ntup;
	theta = 0.0;
	for (i = 0; i <= a->slew.ntup; i++)
	{
		a->slew.cup[i] = 0.5 * (1.0 - cos (theta));
		theta += delta;
	}

	delta = PI / (double)a->slew.ntdown;
	theta = 0.0;
	for (i = 0; i <= a->slew.ntdown; i++)
	{
		a->slew.cdown[i] = 0.5 * (1 + cos (theta));
		theta += delta;
	}

	InterlockedBitTestAndReset (&a->slew.upflag, 0);
	InterlockedBitTestAndReset (&a->slew.downflag, 0);
}

void destroy_slews(IOB a)
{
	_aligned_free (a->slew.cdown);
	_aligned_free (a->slew.cup);
}

void flush_slews (IOB a)
{
	a->slew.ustate = BEGIN;
	a->slew.dstate = BEGIN;
	a->slew.ucount = 0;
	a->slew.dcount = 0;
	InterlockedBitTestAndReset (&a->slew.upflag, 0);
	InterlockedBitTestAndReset (&a->slew.downflag, 0);
}

void upslew0 (IOB a, double* pin)
{
	int i;
	double *pout;
	double I, Q;
	pout = a->r1_baseptr + 2 * a->r1_inidx;
	for (i = 0; i < a->in_size; i++)
	{
		I = pin[2 * i + 0];
		Q = pin[2 * i + 1];
		switch (a->slew.ustate)
			{
			case BEGIN:
				pout[2 * i + 0] = 0.0;
				pout[2 * i + 1] = 0.0;
				if ((I != 0.0) || (Q != 0.0))
				{
					if (a->slew.ndelup > 0)
					{
						a->slew.ustate = DELAYUP;
						a->slew.ucount = a->slew.ndelup;
					}
					else if (a->slew.ntup > 0)
					{
						a->slew.ustate = UPSLEW;
						a->slew.ucount = a->slew.ntup;
					}
					else
						a->slew.ustate = ON;
				}
				break;
			case DELAYUP:
				pout[2 * i + 0] = 0.0;
				pout[2 * i + 1] = 0.0;
				if (a->slew.ucount-- == 0)
				{
					if (a->slew.ntup > 0)
					{
						a->slew.ustate = UPSLEW;
						a->slew.ucount = a->slew.ntup;
					}
					else
						a->slew.ustate = ON;
				}
				break;
			case UPSLEW:
				pout[2 * i + 0] = I * a->slew.cup[a->slew.ntup - a->slew.ucount];
				pout[2 * i + 1] = Q * a->slew.cup[a->slew.ntup - a->slew.ucount];
				if (a->slew.ucount-- == 0)
					a->slew.ustate = ON;
				break;
			case ON:
				pout[2 * i + 0] = I;
				pout[2 * i + 1] = Q;
				if (i == a->in_size - 1)
				{
					a->slew.ustate = BEGIN;
					InterlockedBitTestAndReset (&a->slew.upflag, 0);
				}
				break;
			}
	}
}

void upslew2 (IOB a, INREAL* pIin, INREAL* pQin)
{
	int i;
	double *pout;
	double I, Q;
	pout = a->r1_baseptr + 2 * a->r1_inidx;
	for (i = 0; i < a->in_size; i++)
	{
		I = (double)pIin[i];
		Q = (double)pQin[i];
		switch (a->slew.ustate)
			{
			case BEGIN:
				pout[2 * i + 0] = 0.0;
				pout[2 * i + 1] = 0.0;
				if ((I != 0.0) || (Q != 0.0))
				{
					if (a->slew.ndelup > 0)
					{
						a->slew.ustate = DELAYUP;
						a->slew.ucount = a->slew.ndelup;
					}
					else if (a->slew.ntup > 0)
					{
						a->slew.ustate = UPSLEW;
						a->slew.ucount = a->slew.ntup;
					}
					else
						a->slew.ustate = ON;
				}
				break;
			case DELAYUP:
				pout[2 * i + 0] = 0.0;
				pout[2 * i + 1] = 0.0;
				if (a->slew.ucount-- == 0)
				{
					if (a->slew.ntup > 0)
					{
						a->slew.ustate = UPSLEW;
						a->slew.ucount = a->slew.ntup;
					}
					else
						a->slew.ustate = ON;
				}
				break;
			case UPSLEW:
				pout[2 * i + 0] = I * a->slew.cup[a->slew.ntup - a->slew.ucount];
				pout[2 * i + 1] = Q * a->slew.cup[a->slew.ntup - a->slew.ucount];
				if (a->slew.ucount-- == 0)
					a->slew.ustate = ON;
				break;
			case ON:
				pout[2 * i + 0] = I;
				pout[2 * i + 1] = Q;
				if (i == a->in_size - 1)
				{
					a->slew.ustate = BEGIN;
					InterlockedBitTestAndReset (&a->slew.upflag, 0);
				}
				break;
			}
	}
}

void downslew0 (IOB a, double* pout)
{
	int i;
	double *pin;
	double I, Q;
	pin = a->r2_baseptr + 2 * a->r2_outidx;
	for (i = 0; i < a->out_size; i++)
	{
		I = pin[2 * i + 0];
		Q = pin[2 * i + 1];
		switch (a->slew.dstate)
			{
			case BEGIN:
				pout[2 * i + 0] = I;
				pout[2 * i + 1] = Q;
				if (a->slew.ndeldown > 0)
				{
					a->slew.dstate = DELAYDOWN;
					a->slew.dcount = a->slew.ndeldown;
				}
				else if (a->slew.ntdown > 0)
				{
					a->slew.dstate = DOWNSLEW;
					a->slew.dcount = a->slew.ntdown;
				}
				else
				{
					a->slew.dstate = ZERO;
					a->slew.dcount = a->out_size;
				}
				break;
			case DELAYDOWN:
				pout[2 * i + 0] = I;
				pout[2 * i + 1] = Q;
				if (a->slew.dcount-- == 0)
				{
					if (a->slew.ntdown > 0)
					{
						a->slew.dstate = DOWNSLEW;
						a->slew.dcount = a->slew.ntdown;
					}
					else
					{
						a->slew.dstate = ZERO;
						a->slew.dcount = a->out_size;
					}
				}
				break;
			case DOWNSLEW:
				pout[2 * i + 0] = I * a->slew.cdown[a->slew.ntdown - a->slew.dcount];
				pout[2 * i + 1] = Q * a->slew.cdown[a->slew.ntdown - a->slew.dcount];
				if (a->slew.dcount-- == 0)
				{
					a->slew.dstate = ZERO;
					a->slew.dcount = a->out_size;
				}
				break;
			case ZERO:
				pout[2 * i + 0] = 0.0;
				pout[2 * i + 1] = 0.0;
				if (a->slew.dcount-- == 0)
					a->slew.dstate = OFF;
				break;
			case OFF:
				pout[2 * i + 0] = 0.0;
				pout[2 * i + 1] = 0.0;
				if (i == a->out_size - 1)
				{
					a->slew.dstate = BEGIN;
					InterlockedBitTestAndReset (&a->slew.downflag, 0);
				}
				break;
			}
	}
}

void downslew2 (IOB a, OUTREAL* pIout, OUTREAL* pQout)
{
	int i;
	double *pin;
	double I, Q;
	pin = a->r2_baseptr + 2 * a->r2_outidx;
	for (i = 0; i < a->out_size; i++)
	{
		I = pin[2 * i + 0];
		Q = pin[2 * i + 1];
		switch (a->slew.dstate)
			{
			case BEGIN:
				pIout[i] = (OUTREAL)I;
				pQout[i] = (OUTREAL)Q;
				if (a->slew.ndeldown > 0)
				{
					a->slew.dstate = DELAYDOWN;
					a->slew.dcount = a->slew.ndeldown;
				}
				else if (a->slew.ntdown > 0)
				{
					a->slew.dstate = DOWNSLEW;
					a->slew.dcount = a->slew.ntdown;
				}
				else
				{
					a->slew.dstate = ZERO;
					a->slew.dcount = a->out_size;
				}
				break;
			case DELAYDOWN:
				pIout[i] = (OUTREAL)I;
				pQout[i] = (OUTREAL)Q;
				if (a->slew.dcount-- == 0)
				{
					if (a->slew.ntdown > 0)
					{
						a->slew.dstate = DOWNSLEW;
						a->slew.dcount = a->slew.ntdown;
					}
					else
					{
						a->slew.dstate = ZERO;
						a->slew.dcount = a->out_size;
					}
				}
				break;
			case DOWNSLEW:
				pIout[i] = (OUTREAL)(I * a->slew.cdown[a->slew.ntdown - a->slew.dcount]);
				pQout[i] = (OUTREAL)(Q * a->slew.cdown[a->slew.ntdown - a->slew.dcount]);
				if (a->slew.dcount-- == 0)
				{
					a->slew.dstate = ZERO;
					a->slew.dcount = a->out_size;
				}
				break;
			case ZERO:
				pIout[i] = 0.0;
				pQout[i] = 0.0;
				if (a->slew.dcount-- == 0)
					a->slew.dstate = OFF;
				break;
			case OFF:
				pIout[i] = 0.0;
				pQout[i] = 0.0;
				if (i == a->out_size - 1)
				{
					a->slew.dstate = BEGIN;
					InterlockedBitTestAndReset (&a->slew.downflag, 0);
				}
				break;
			}
	}
}

/********************************************************************************************************
*																										*
*										  Begin Buffer Code												*
*																										*
********************************************************************************************************/

void create_iobuffs (int channel)
{
	int n;
	IOB a = (IOB) malloc0 (sizeof(iob));
	ch[channel].iob.pc = ch[channel].iob.pd = ch[channel].iob.pe = ch[channel].iob.pf = a;
	a->channel = channel;
	a->in_size = ch[channel].in_size;
	a->r1_outsize = ch[channel].dsp_insize;
	if (a->r1_outsize > a->in_size)
		a->r1_size = a->r1_outsize;
	else
		a->r1_size = a->in_size;
	a->out_size = ch[channel].out_size;
	a->r2_insize = ch[channel].dsp_outsize;
	if (a->out_size > a->r2_insize)
		a->r2_size = a->out_size;
	else
		a->r2_size = a->r2_insize;
	a->r1_active_buffsize = DSP_MULT * a->r1_size;
	a->r2_active_buffsize = DSP_MULT * a->r2_size;
	a->r1_baseptr = (double*) malloc0 (a->r1_active_buffsize * sizeof (complex));
	a->r2_baseptr = (double*) malloc0 (a->r2_active_buffsize * sizeof (complex));
	a->r1_inidx = 0;
	a->r1_outidx = 0;
	a->r1_unqueuedsamps = 0;
	a->r2_inidx = (DSP_MULT - 1) * a->r2_size;
	a->r2_outidx = 0;
	a->r2_havesamps = (DSP_MULT - 1) * a->r2_size;
	n = a->r2_havesamps / a->out_size;
	a->r2_unqueuedsamps = a->r2_havesamps - n * a->out_size;
	InitializeCriticalSectionAndSpinCount(&a->r2_ControlSection, 2500);
	a->Sem_BuffReady = CreateSemaphore(0, 0, 1000, 0);
	a->Sem_OutReady  = CreateSemaphore(0, n, 1000, 0);
	a->bfo = ch[channel].bfo;
	create_slews (a);

	InterlockedBitTestAndReset(&a->flush_bypass, 0);
	// AetherSDR patch 9: explicit, alongside upstream's flush_bypass reset. The
	// iob comes from malloc0 so this is already zero, but the handshake reads
	// it and the reset above sets the convention for this pair of flags.
	InterlockedBitTestAndReset(&a->flush_quiesced, 0);
	a->Sem_Flush = CreateSemaphore(0, 0, 1, 0);
	_beginthread(flushChannel, 0, (void*)(uintptr_t)a->channel);
}

// AetherSDR patch 9: THE flushChannel EXIT HANDSHAKE, LIFTED OUT OF
// destroy_iobuffs() SO IT CAN RUN ON THE OTHER SIDE OF destroy_main().
//
// The handshake itself is upstream's, moved rather than invented: set
// flush_bypass, release Sem_Flush so a parked thread wakes and sees it, and
// wait for the thread's own reset of flush_bypass at the tail of
// flushChannel() (channel.c). What was wrong was WHERE upstream ran it.
// destroy_iobuffs() is reached only from post_main_destroy(), and CloseChannel()
// is pre_main_destroy(); destroy_main(); post_main_destroy() — so the flush
// thread was still live across destroy_main(), which is destroy_rxa()/
// destroy_txa(), exactly the chain flush_main() -> flush_rxa() walks. See
// AETHERSDR-PATCHES.md patch 9 for the measurement.
//
// BOUNDED, where upstream's spin was not, for the reason patch 4 gives for
// bounding its worker handshake: upstream ignores _beginthread() failure, and
// an unbounded wait on a thread that was never created would hang CloseChannel()
// forever. NOT free, though, and the registry entry is precise about it:
// upstream's wait could not fall through, this one can, and on exhaustion the
// CloseHandle() in destroy_iobuffs() runs under a possibly-parked flush thread
// -- patch 4's own failure mode, inherited. The cap is deliberately generous
// for that reason; do not shorten it without re-reading patch 9.
//
// IDEMPOTENT, because both ends of the pre/post pair call it. A second pass must
// not re-arm flush_bypass — the thread that would acknowledge it is gone, and
// the wait would burn the whole cap on every close.
void quiesce_flush (int channel)
{
	IOB a = ch[channel].iob.pc;
	int waited = 0;
	if (_InterlockedAnd (&a->flush_quiesced, 1)) return;
	InterlockedBitTestAndSet (&a->flush_bypass, 0);
	// May legitimately fail: Sem_Flush has a maximum count of 1, and a ramp that
	// completed has already released it. That token wakes the thread just as
	// ours would, and flush_bypass is what it reads on waking, so the wake is
	// delivered either way.
	ReleaseSemaphore (a->Sem_Flush, 1, 0);
	while (InterlockedAnd (&a->flush_bypass, 0xffffffff) && waited < 1000)
	{
		Sleep (1);
		waited++;
	}
	InterlockedBitTestAndSet (&a->flush_quiesced, 0);
}

void destroy_iobuffs (int channel)
{
	IOB a = ch[channel].iob.pc;

	// AetherSDR patch 9: normally a no-op — pre_main_destroy() has already run
	// the handshake, before destroy_main(). Kept here so destroy_iobuffs()
	// remains self-sufficient for any future caller that does not pair with
	// pre_main_destroy(); today it has exactly one, post_main_destroy().
	quiesce_flush (channel);
	CloseHandle(a->Sem_Flush);

	destroy_slews (a);
	CloseHandle (a->Sem_OutReady);
	CloseHandle (a->Sem_BuffReady);
	DeleteCriticalSection(&a->r2_ControlSection);
	_aligned_free (a->r2_baseptr);
	_aligned_free (a->r1_baseptr);
	_aligned_free (a);
}

void flush_iobuffs (int channel)
{
	int n;
	IOB a = ch[channel].iob.pf;
	memset (a->r1_baseptr, 0, a->r1_active_buffsize * sizeof (complex));
	memset (a->r2_baseptr, 0, a->r2_active_buffsize * sizeof (complex));
	a->r1_inidx = 0;
	a->r1_outidx = 0;
	a->r1_unqueuedsamps = 0;
	a->r2_inidx = (DSP_MULT - 1) * a->r2_size;
	a->r2_outidx = 0;
	a->r2_havesamps = (DSP_MULT - 1) * a->r2_size;
	while (!WaitForSingleObject (a->Sem_BuffReady, 1));
	n = a->r2_havesamps / a->out_size;
	a->r2_unqueuedsamps = a->r2_havesamps - n * a->out_size;
	CloseHandle (a->Sem_OutReady);
	a->Sem_OutReady  = CreateSemaphore(0, n, 1000, 0);
	flush_slews (a);
}


PORT	//double, interleaved I/Q
void fexchange0 (int channel, double* in, double* out, int* error)
{
	int n;
	int doit = 0;
	IOB a;
	*error = 0;
	if (_InterlockedAnd (&ch[channel].exchange, 1))
	{
		EnterCriticalSection (&ch[channel].csEXCH);
		a = ch[channel].iob.pe;
		if (_InterlockedAnd (&a->slew.upflag, 1))
			upslew0 (a, in);
		else
			memcpy (a->r1_baseptr + 2 * a->r1_inidx, in, a->in_size * sizeof (complex));
																											// add check with *error += -1; for case when r1 is full and an overwrite occurs
		if ((a->r1_unqueuedsamps += a->in_size) >= a->r1_outsize)
		{
			n = a->r1_unqueuedsamps / a->r1_outsize;
			ReleaseSemaphore(a->Sem_BuffReady, n, 0);
			a->r1_unqueuedsamps -= n * a->r1_outsize;
		}
		if ((a->r1_inidx += a->in_size) == a->r1_active_buffsize)
			a->r1_inidx = 0;

		EnterCriticalSection (&a->r2_ControlSection);
		if (a->r2_havesamps >= a->out_size)
			doit = 1;
		if ((a->r2_havesamps -= a->out_size) < 0) a->r2_havesamps = 0;
		LeaveCriticalSection (&a->r2_ControlSection);
		if (a->bfo) WaitForSingleObject (a->Sem_OutReady, INFINITE);
		if (a->bfo || doit)
			if (_InterlockedAnd (&a->slew.downflag, 1))
			{
				downslew0 (a, out);
				if (!_InterlockedAnd (&a->slew.downflag, 1))
				{
					InterlockedBitTestAndReset (&ch[channel].exchange, 0);
					ReleaseSemaphore(a->Sem_Flush, 1, 0);
				}
			}
			else
				memcpy (out, a->r2_baseptr + 2 * a->r2_outidx, a->out_size * sizeof (complex));
		else
		{
			memset (out, 0, a->out_size * sizeof (complex));
			*error += -2;
		}
		if ((a->r2_outidx += a->out_size) == a->r2_active_buffsize)
			a->r2_outidx = 0;
		LeaveCriticalSection (&ch[channel].csEXCH);
	}
}

PORT	//separate I/Q buffers
void fexchange2 (int channel, INREAL *Iin, INREAL *Qin, OUTREAL *Iout, OUTREAL *Qout, int* error)
{
	int i, n;
	int doit = 0;
	IOB a;
	*error = 0;
	if (_InterlockedAnd (&ch[channel].exchange, 1))
	{
		EnterCriticalSection (&ch[channel].csEXCH);
		a = ch[channel].iob.pe;
		if (_InterlockedAnd (&a->slew.upflag, 1))
			upslew2 (a, Iin, Qin);
		else
			for (i = 0; i < a->in_size; i++)
			{
				(a->r1_baseptr + 2 * a->r1_inidx)[2 * i + 0] = (double)(Iin[i]);
				(a->r1_baseptr + 2 * a->r1_inidx)[2 * i + 1] = (double)(Qin[i]);
			}
																										// add check with *error += -1; for case when r1 is full and an overwrite occurs
		if ((a->r1_unqueuedsamps += a->in_size) >= a->r1_outsize)
		{
			n = a->r1_unqueuedsamps / a->r1_outsize;
			ReleaseSemaphore(a->Sem_BuffReady, n, 0);	
			a->r1_unqueuedsamps -= n * a->r1_outsize;
		}
		if ((a->r1_inidx += a->in_size) == a->r1_active_buffsize)
			a->r1_inidx = 0;

		EnterCriticalSection (&a->r2_ControlSection);
		if (a->r2_havesamps >= a->out_size)
			doit = 1;
		if ((a->r2_havesamps -= a->out_size) < 0) a->r2_havesamps = 0;
		LeaveCriticalSection (&a->r2_ControlSection);
		if (a->bfo) WaitForSingleObject (a->Sem_OutReady, INFINITE);
		if (a->bfo || doit)
		{
			if (_InterlockedAnd (&a->slew.downflag, 1))
			{
				downslew2 (a, Iout, Qout);
				if (!_InterlockedAnd (&a->slew.downflag, 1))
				{
					InterlockedBitTestAndReset (&ch[channel].exchange, 0);
					ReleaseSemaphore(a->Sem_Flush, 1, 0);
				}
			}
			else
				for (i = 0; i < a->out_size; i++)
				{
					Iout[i] = (OUTREAL)((a->r2_baseptr + 2 * a->r2_outidx)[2 * i + 0]);
					Qout[i] = (OUTREAL)((a->r2_baseptr + 2 * a->r2_outidx)[2 * i + 1]);
				}
		}
		else
		{
			memset (Iout, 0, a->out_size * sizeof (OUTREAL));
			memset (Qout, 0, a->out_size * sizeof (OUTREAL));
			*error += -2;
		}
		if ((a->r2_outidx += a->out_size) == a->r2_active_buffsize)
			a->r2_outidx = 0;
		LeaveCriticalSection (&ch[channel].csEXCH);
	}
}

int dexchange (int channel, double* in, double* out)
{
	int n;
	IOB a = ch[channel].iob.pd;
	if (!_InterlockedAnd (&ch[channel].run, 1))
	{
		// AetherSDR patch 4: upstream called _endthread() here — terminating
		// the worker mid-function, with csDSP HELD (wdspmain() calls us inside
		// it and _endthread() does not unwind, so post_main_destroy() later
		// found a locked section to DeleteCriticalSection), and skipping
		// wdspmain()'s exit handshake entirely, so pre_main_destroy() waited
		// out its full cap on a thread that was already gone and then fell
		// through with no barrier at all.
		//
		// RETURN instead, and let the caller unwind. That makes wdspmain()'s
		// tail the worker's single exit, which is what lets the handshake
		// store a generation held in a LOCAL — shared state would be wrong
		// here: after a fall-through the abandoned worker is still alive, and
		// anything it reads from ch[] could belong to its successor (#5411
		// second-opinion review).
		return 1;
	}

	// AetherSDR patch 13: TAKE THE INPUT BEFORE RELEASING THE HOST (#5734).
	//
	// Upstream copies this r1 slot out AFTER releasing Sem_OutReady below. With
	// bfo set, that release is what lets the host's blocked fexchange* return,
	// and the host's NEXT call writes the next input block into r1 before it
	// waits again. When in_size == dsp_insize the ring holds DSP_MULT (2) slots,
	// so host call w+2 writes the very slot worker iteration w has not read yet:
	// a worker preempted between the release and this copy processes block w+2
	// (or a torn mix of w and w+2) in place of block w, and reports nothing.
	// Idle, the window is a few instructions wide and never loses; under CPU
	// load on Linux it lost in 33 of 600 trials. Only bfo channels release
	// here, so this is the blocking (offline/test) mode, not live audio. See
	// AETHERSDR-PATCHES.md patch 13.
	//
	// The copy is all that moves. r1_outidx is touched only by this thread and
	// by flush_iobuffs() (which holds csDSP, as our caller does), and `out`
	// (the chain's inbuff) is a different allocation from `in` (its outbuff) in
	// both RXA.c and TXA.c, so the reorder changes no value -- only when the
	// host is told it may proceed.
	memcpy (out, a->r1_baseptr + 2 * a->r1_outidx, a->r1_outsize * sizeof (complex));
	if ((a->r1_outidx += a->r1_outsize) == a->r1_active_buffsize)
		a->r1_outidx = 0;

	EnterCriticalSection (&a->r2_ControlSection);
	a->r2_havesamps += a->r2_insize;
	LeaveCriticalSection (&a->r2_ControlSection);
	memcpy (a->r2_baseptr + 2 * a->r2_inidx, in, a->r2_insize * sizeof (complex));
	if ((a->r2_inidx += a->r2_insize) == a->r2_active_buffsize)
		a->r2_inidx = 0;
	if (a->bfo && (a->r2_unqueuedsamps += a->r2_insize) >= a->out_size)
	{
		n = a->r2_unqueuedsamps / a->out_size;
		ReleaseSemaphore(a->Sem_OutReady, n, 0);	
		a->r2_unqueuedsamps -= n * a->out_size;
	}
	// AetherSDR patch 13: test-only; a no-op unless wdsp_channel_test sets it.
	// Sits immediately after the release on purpose -- see wdsp_port.c.
	wdspPortHandoffPauseForTest ();
	return 0;
}
