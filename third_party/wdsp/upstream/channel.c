/*  channel.c

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

struct _ch ch[MAX_CHANNELS];

void start_thread (int channel)
{
	// AetherSDR patch 4: arm the exit handshake before the thread exists, on every
	// (re)build path — OpenChannel and the SetInput*/SetDSP* rebuilds all come here.
	// A GENERATION, not a 0/1 flag. If a previous pre_main_destroy() fell through
	// its cap, that worker is still alive and will store eventually; with a flag
	// its late store would land on the NEW worker's slot and satisfy the next
	// wait for free, silently disabling the handshake for the rest of the
	// channel's life (#5411 review). Storing the generation makes a stale store
	// inert: it writes an old number that never equals the current mainGen.
	InterlockedIncrement (&ch[channel].mainGen);
	HANDLE handle = (HANDLE) _beginthread(wdspmain, 0, (void *)(uintptr_t)channel);
	//SetThreadPriority(handle, THREAD_PRIORITY_HIGHEST);
}

void pre_main_build (int channel)
{
	if (ch[channel].in_rate  >= ch[channel].dsp_rate)
		ch[channel].dsp_insize  = ch[channel].dsp_size * (ch[channel].in_rate  / ch[channel].dsp_rate);
	else
		ch[channel].dsp_insize  = ch[channel].dsp_size / (ch[channel].dsp_rate /  ch[channel].in_rate);

	if (ch[channel].out_rate >= ch[channel].dsp_rate)
		ch[channel].dsp_outsize = ch[channel].dsp_size * (ch[channel].out_rate / ch[channel].dsp_rate);
	else
		ch[channel].dsp_outsize = ch[channel].dsp_size / (ch[channel].dsp_rate / ch[channel].out_rate);

	if (ch[channel].in_rate  >= ch[channel].out_rate)
		ch[channel].out_size    = ch[channel].in_size  / (ch[channel].in_rate  / ch[channel].out_rate);
	else
		ch[channel].out_size    = ch[channel].in_size  * (ch[channel].out_rate /  ch[channel].in_rate);

	InitializeCriticalSectionAndSpinCount ( &ch[channel].csDSP, 2500 );
	InitializeCriticalSectionAndSpinCount ( &ch[channel].csEXCH,  2500 );
	InterlockedBitTestAndReset (&ch[channel].flushflag, 0);
	create_iobuffs (channel);
}

void post_main_build (int channel)
{
	InterlockedBitTestAndSet (&ch[channel].run, 0);
	start_thread (channel);
	if (ch[channel].state == 1)
	 	InterlockedBitTestAndSet (&ch[channel].exchange, 0);
}

void build_channel (int channel)
{
	pre_main_build (channel);
	create_main (channel);
	post_main_build (channel);
}

PORT
void OpenChannel (int channel, int in_size, int dsp_size, int input_samplerate, int dsp_rate, int output_samplerate, 
	int type, int state, double tdelayup, double tslewup, double tdelaydown, double tslewdown, int bfo)
{
	ch[channel].in_size = in_size;
	ch[channel].dsp_size = dsp_size;
	ch[channel].in_rate = input_samplerate;
	ch[channel].dsp_rate = dsp_rate;
	ch[channel].out_rate = output_samplerate;
	ch[channel].type = type;
	ch[channel].state = state;
	ch[channel].tdelayup = tdelayup;
	ch[channel].tslewup = tslewup;
	ch[channel].tdelaydown = tdelaydown;
	ch[channel].tslewdown = tslewdown;
	ch[channel].bfo = bfo;
	InterlockedBitTestAndReset (&ch[channel].exchange, 0);
	build_channel (channel);
	if (ch[channel].state)
	{
		InterlockedBitTestAndSet (&ch[channel].iob.pc->slew.upflag, 0);
		InterlockedBitTestAndSet (&ch[channel].iob.ch_upslew, 0);
		InterlockedBitTestAndReset (&ch[channel].iob.pc->exec_bypass, 0);
		InterlockedBitTestAndSet (&ch[channel].exchange, 0);
	}
	_MM_SET_FLUSH_ZERO_MODE (_MM_FLUSH_ZERO_ON);
}

void pre_main_destroy (int channel)
{
	IOB a = ch[channel].iob.pc;
	InterlockedBitTestAndReset (&ch[channel].exchange, 0);
	// AetherSDR patch 4: exec_bypass BEFORE run, which is the reverse of
	// upstream's order. The worker reads exec_bypass and then, inside
	// dexchange(), reads run (iobuffs.c). Clearing run first opens a window
	// where it sees "not bypassed" and then "not running" and unwinds through
	// dexchange()'s early return. Setting the bypass first makes the bypass
	// branch win for any worker that has not yet read it. The window is
	// narrowed, not closed — a worker can read exec_bypass just before this
	// line — but either way the worker leaves through wdspmain()'s tail and
	// performs the handshake there, so correctness does not rest on the
	// ordering; it only saves a wakeup.
	InterlockedBitTestAndSet (&ch[channel].iob.pc->exec_bypass, 0);
	InterlockedBitTestAndReset (&ch[channel].run, 0);
	ReleaseSemaphore (a->Sem_BuffReady, 1, 0);
	// AetherSDR patch 4. Upstream slept 25 ms here as its only barrier between
	// the worker's exit and destroy_main()/post_main_destroy() freeing the
	// semaphore, mutex and buffers the worker is still touching. A sleep is a
	// scheduling bet, not synchronization: a starved worker leaves
	// pthread_cond_wait() on freed memory (TSan: 116 reports, 21 failing tests
	// across the HL2/WDSP family, #5275). Wait for the worker's own exit store
	// instead. BOUNDED, because upstream ignores thread-creation failure and an
	// unbounded wait would then hang CloseChannel() forever; falling through
	// after the cap restores upstream's behaviour exactly, no worse.
	{
		const long gen = _InterlockedAnd (&ch[channel].mainGen, ~0L);
		int waited = 0;
		while (_InterlockedAnd (&ch[channel].mainExited, ~0L) != gen && waited < 1000)
		{
			Sleep (1);
			++waited;
		}
	}
}

void post_main_destroy (int channel)
{
	destroy_iobuffs (channel);
	DeleteCriticalSection ( &ch[channel].csEXCH  );
	DeleteCriticalSection ( &ch[channel].csDSP );
}

PORT
void CloseChannel (int channel)
{
	pre_main_destroy (channel);
	destroy_main (channel);
	post_main_destroy (channel);
}

void flushChannel (void* p)
{
	int channel = (int)(uintptr_t)p;
	IOB a = ch[channel].iob.pc;
	while (!InterlockedAnd(&a->flush_bypass, 0xffffffff))
	{
		WaitForSingleObject(a->Sem_Flush, INFINITE);
		if (!InterlockedAnd(&a->flush_bypass, 0xffffffff))
		{
			EnterCriticalSection(&ch[channel].csDSP);
			EnterCriticalSection(&ch[channel].csEXCH);
			flush_iobuffs(channel);
			InterlockedBitTestAndSet(&a->exec_bypass, 0);
			flush_main(channel);
			LeaveCriticalSection(&ch[channel].csEXCH);
			LeaveCriticalSection(&ch[channel].csDSP);
			InterlockedBitTestAndReset(&ch[channel].flushflag, 0);
		}
	}
	InterlockedBitTestAndReset(&a->flush_bypass, 0);
}

/********************************************************************************************************
*																										*
*										Channel Properties												*
*																										*
********************************************************************************************************/

PORT
void SetType (int channel, int type)
{	// no need to rebuild buffers; but we did anyway
	if (type != ch[channel].type)
	{
		CloseChannel (channel);
		ch[channel].type = type;
		build_channel (channel);
	}
}

PORT
void SetInputBuffsize (int channel, int in_size)
{	// we do not rebuild main here since it didn't change
	if (in_size != ch[channel].in_size)
	{
		pre_main_destroy (channel);
		post_main_destroy (channel);
		ch[channel].in_size = in_size;
		pre_main_build (channel);
		post_main_build (channel);
	}
}

PORT
void SetDSPBuffsize (int channel, int dsp_size)
{
	if (dsp_size != ch[channel].dsp_size)
	{
		int oldstate = SetChannelState (channel, 0, 1);
		pre_main_destroy (channel);
		post_main_destroy (channel);
		ch[channel].dsp_size = dsp_size;
		pre_main_build (channel);
		setDSPBuffsize_main (channel);
		post_main_build (channel);
		SetChannelState (channel, oldstate, 0);
	}
}

PORT
void SetInputSamplerate (int channel, int in_rate)
{	// no re-build of main required
	if (in_rate != ch[channel].in_rate)
	{
		pre_main_destroy (channel);
		post_main_destroy (channel);
		ch[channel].in_rate = in_rate;
		pre_main_build (channel);
		setInputSamplerate_main (channel);
		post_main_build (channel);
	}
}

PORT
void SetDSPSamplerate (int channel, int dsp_rate)
{
	if (dsp_rate != ch[channel].dsp_rate)
	{
		int oldstate = SetChannelState (channel, 0, 1);
		pre_main_destroy (channel);
		post_main_destroy (channel);
		ch[channel].dsp_rate = dsp_rate;
		pre_main_build (channel);
		setDSPSamplerate_main (channel);
		post_main_build (channel);
		SetChannelState (channel, oldstate, 0);
	}
}

PORT
void SetOutputSamplerate (int channel, int out_rate)
{	// no re-build of main required
	if (out_rate != ch[channel].out_rate)
	{
		pre_main_destroy (channel);
		post_main_destroy (channel);
		ch[channel].out_rate = out_rate;
		pre_main_build (channel);
		setOutputSamplerate_main (channel);
		post_main_build (channel);
	}
}

PORT
void SetAllRates (int channel, int in_rate, int dsp_rate, int out_rate)
{
	if ((in_rate != ch[channel].in_rate) || (dsp_rate != ch[channel].dsp_rate) || (out_rate != ch[channel].out_rate))
	{
		pre_main_destroy (channel);
		post_main_destroy (channel);
		ch[channel].in_rate  = in_rate;
		ch[channel].dsp_rate = dsp_rate;
		ch[channel].out_rate = out_rate;
		pre_main_build (channel);
		setInputSamplerate_main (channel);
		setDSPSamplerate_main (channel);
		setOutputSamplerate_main (channel);
		post_main_build (channel);
	}
}

PORT
int SetChannelState (int channel, int state, int dmode)
{
	IOB a = ch[channel].iob.pc;
	int prior_state = ch[channel].state;
	int count = 0;
	const int timeout = 100;
	if (ch[channel].state != state)
	{
		ch[channel].state = state;
		switch (ch[channel].state)
		{
		case 0:
			InterlockedBitTestAndSet (&a->slew.downflag, 0);
			InterlockedBitTestAndSet (&ch[channel].flushflag, 0);
			if (dmode)
			{
				while (_InterlockedAnd (&ch[channel].flushflag, 1) && count < timeout) 
				{
					Sleep(1);
					count++;
				}
			}
			if (count >= timeout)
			{
				InterlockedBitTestAndReset (&ch[channel].exchange, 0);
				InterlockedBitTestAndReset (&ch[channel].flushflag, 0);
				InterlockedBitTestAndReset (&a->slew.downflag, 0);
			}
			break;
		case 1:
			InterlockedBitTestAndSet (&a->slew.upflag, 0);
			InterlockedBitTestAndSet (&ch[channel].iob.ch_upslew, 0);
			InterlockedBitTestAndReset (&ch[channel].iob.pc->exec_bypass, 0);
			InterlockedBitTestAndSet (&ch[channel].exchange, 0);
			break;
		}
	}
	return prior_state;
}

PORT
void SetChannelTDelayUp (int channel, double time)
{
	IOB a;
	EnterCriticalSection (&ch[channel].csEXCH);
	a = ch[channel].iob.pc;
	ch[channel].tdelayup = time;
	a->slew.ndelup = (int)(ch[a->channel].tdelayup * ch[a->channel].in_rate);
	flush_slews (a);
	LeaveCriticalSection (&ch[channel].csEXCH);
}

PORT
void SetChannelTSlewUp (int channel, double time)
{
	IOB a;
	EnterCriticalSection (&ch[channel].csEXCH);
	a = ch[channel].iob.pc;
	ch[channel].tslewup = time;
	destroy_slews (a);
	create_slews (a);
	LeaveCriticalSection (&ch[channel].csEXCH);
}

PORT
void SetChannelTDelayDown (int channel, double time)
{
	IOB a;
	EnterCriticalSection (&ch[channel].csEXCH);
	a = ch[channel].iob.pc;
	ch[channel].tdelaydown = time;
	a->slew.ndeldown = (int)(ch[a->channel].tdelaydown * ch[a->channel].out_rate);
	flush_slews (a);
	LeaveCriticalSection (&ch[channel].csEXCH);
}

PORT
void SetChannelTSlewDown (int channel, double time)
{
	IOB a;
	EnterCriticalSection (&ch[channel].csEXCH);
	a = ch[channel].iob.pc;
	ch[channel].tslewdown = time;
	destroy_slews (a);
	create_slews (a);
	LeaveCriticalSection (&ch[channel].csEXCH);
}