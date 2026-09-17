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
			// AND RE-POST THE TOKEN, because the single post above can be
			// STOLEN. flush_iobuffs() drains this same semaphore with
			// `while (!WaitForSingleObject (a->Sem_BuffReady, 1));`, and a stop
			// that was clocked out leaves flushChannel runnable -- a shape this
			// branch is what first makes reachable. If the flush thread gets its
			// slot while this loop is running, it consumes the worker's wake-up,
			// the worker stays parked on Sem_BuffReady forever, this loop falls
			// through its cap exactly as designed, and destroy_iobuffs() then
			// CloseHandle()s the semaphore under a live waiter -- where glibc's
			// pthread_cond_destroy() blocks and never returns.
			//
			// Measured by ten9876 on #5628: 7 hangs in 16 runs of
			// wdsp_channel_test under 8-way parallel load, 0 in 32 with this
			// line. Both captured cores show pthread_cond_destroy <- CloseHandle
			// <- destroy_iobuffs with wdspmain still in WaitForSingleObject.
			//
			// Extra tokens cost nothing: the worker exits on run == 0 however
			// many are outstanding, and the whole iob is freed immediately
			// afterwards. A post past the semaphore's 1000 maximum simply fails,
			// which is also harmless here.
			ReleaseSemaphore (a->Sem_BuffReady, 1, 0);
			++waited;
		}
	}
	// AetherSDR patch 9: AND NOW THE OTHER DETACHED THREAD. Patch 4 waited for
	// the wdspmain worker and said of this one: "flushChannel() has the same
	// detached shape and no handshake; it has not surfaced, and gets the same
	// treatment if it does." It has surfaced.
	//
	// flushChannel() DOES have a handshake — upstream's, in destroy_iobuffs().
	// It is simply on the wrong side of destroy_main(). CloseChannel() is
	// pre_main_destroy(); destroy_main(); post_main_destroy(), and
	// destroy_iobuffs() is reached only from the third, so a flush thread that a
	// COMPLETED down-ramp made runnable was still inside flush_main() ->
	// flush_rxa() while destroy_main() -> destroy_rxa() freed that same chain
	// underneath it. Running the handshake here, before destroy_main(), is the
	// whole fix; quiesce_flush() is idempotent so destroy_iobuffs() keeps its
	// call and simply finds the work already done.
	//
	// AFTER the worker wait above, not before, and the order is deliberate:
	// flushChannel() takes csDSP, which the worker holds across dexchange(), so
	// quiescing the flush thread first could make this function wait out the
	// worker's block through a second thread. With the worker already gone, both
	// channel sections are free and nothing can be holding them — the host is
	// fenced out of fexchange* by the caller.
	//
	// REPRODUCED BEFORE FIXING. See AETHERSDR-PATCHES.md patch 9.
	quiesce_flush (channel);
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
			// AetherSDR patch 7: a START CANCELS A DOWN-RAMP THAT WAS NEVER CLOCKED OUT.
			//
			// Upstream's case 1 arms the up-slew and re-arms exchange but never touches
			// slew.downflag, and the two flags are read INDEPENDENTLY on opposite sides of
			// fexchange0/fexchange2: upflag gates the input (upslew0/upslew2), downflag
			// gates the output (downslew0/downslew2). A stop sets downflag; the ramp only
			// advances when the host clocks fexchange*. So stop-then-start before the host
			// has clocked the ramp to completion leaves downflag set on a channel whose
			// state is now 1, and the next few blocks finish the stale ramp. That is not
			// merely a cosmetic fade on a running channel: the completion arm of
			// downslew0/downslew2 in iobuffs.c does
			//     InterlockedBitTestAndReset (&ch[channel].exchange, 0);
			// so it CLEARS EXCHANGE. Every later fexchange* then fails its opening
			// `if (exchange)` test and returns having written nothing and reported no
			// error, while ch[channel].state still reads 1. The channel is silently dead
			// until it is closed and rebuilt, and no flag a host can read says so.
			//
			// The asymmetry is the whole point: only the DOWN flag's completion clears
			// exchange, so the mirror case (start, then stop with upflag still pending) is
			// harmless and needs nothing here.
			//
			// flush_slews() rather than a bare clear of downflag, because the flag is not
			// the whole of the ramp: slew.dstate/dcount are the state machine, and clearing
			// the flag alone would strand dstate mid-ramp (DOWNSLEW/ZERO with a live dcount)
			// for the NEXT stop to resume from. flush_slews() resets both directions, which
			// is also what we want for the up-ramp we are about to arm — a fresh fade-in
			// from BEGIN rather than a resume of whatever ustate held. It clears upflag too,
			// hence the ordering: flush first, arm second.
			//
			// Under csEXCH, for the same reason SetChannelTDelayUp/Down and
			// SetChannelTSlewUp/Down take it around their own flush_slews() calls:
			// slew.dstate/dcount are plain ints owned by fexchange*'s critical section, and
			// without the lock this cancel could interleave with an in-flight downslew that
			// then clears exchange after we have set it. Taking it also makes the whole of
			// case 1 atomic against fexchange*. No new lock-order edge: csEXCH is the
			// innermost of the two channel sections (flushChannel takes csDSP then csEXCH),
			// this takes no other lock inside it and does not wait, and the port maps
			// CRITICAL_SECTION to a RECURSIVE pthread mutex, so a host calling this from
			// inside its own fexchange* thread is safe.
			//
			// ch[channel].flushflag is deliberately NOT cleared. The flush request belongs
			// to the flushChannel thread, which is parked on Sem_Flush and can only be
			// released by a ramp that completes; the next genuine stop releases it and the
			// flag clears then. Clearing it here would not wake that thread, only lie to
			// the dmode-1 wait in case 0.
			//
			// AetherSDR patch 8 (K5PTB, PR #5628): CANCELLING THE RAMP IS NOT ENOUGH. The
			// flush_slews() below cancels a ramp that is still PENDING. It cannot cancel a
			// flush that a ramp which already COMPLETED has requested: at that completion
			// fexchange0/fexchange2 cleared exchange and released Sem_Flush
			// (iobuffs.c, the ReleaseSemaphore calls in each) and the flushChannel
			// thread is now runnable but may not have been
			// scheduled. It takes csDSP then csEXCH, flushes, and does
			//     InterlockedBitTestAndSet (&a->exec_bypass, 0);
			// (channel.c). Arm in that window and flushChannel sets exec_bypass AFTER this
			// case has cleared it, so wdspmain() skips dexchange()/xrxa() entirely
			// (main.c) and the worker produces nothing for a channel whose state reads 1.
			// MEASURED on this tree (macOS arm64, RelWithDebInfo), a stress probe that stops,
			// clocks N blocks at the 256/48 kHz cadence, starts with NO gap, and then asks
			// for audio. Non-blocking, spacings 0-10, 40 trials each: 42 of 440 dead — 24 at
			// spacing 3 and 18 at spacing 4, and ZERO at 0-2, which is the window inside the
			// ramp that flush_slews() below already covered. The ramp is exactly three
			// blocks long here (BEGIN 1 + DOWNSLEW ntdown+1 + ZERO out_size+1 = 739 samples
			// at out_size 256), so spacing 3 is the first spacing at which it COMPLETES, and
			// that is where this dies. A control that sleeps 20 ms before each start —
			// giving the flush thread its slot — is 0 of 440 over the same sweep.
			//
			// Blocking mode is worse: the same restart HANGS THE HOST FOREVER, 20 of 20 at
			// spacing 3. With exec_bypass set, wdspmain() never reaches dexchange(), so
			// Sem_OutReady is never released and fexchange2's
			//     if (a->bfo) WaitForSingleObject (a->Sem_OutReady, INFINITE);
			// never returns. Six thread samples of six separate stalls all showed that same
			// two-thread starvation — host parked in fexchange2 holding csEXCH, flushChannel
			// already finished and back on Sem_Flush, worker idle on Sem_BuffReady. A
			// three-way lock cycle (flushChannel holding csDSP and blocking on the csEXCH
			// the parked host holds, worker then blocking on csDSP) is reachable from the
			// same window on a different interleaving, but was not what any sample caught.
			//
			// So WAIT the flush out before arming. "exchange clear AND flushflag set" names
			// exactly the completed-ramp case and nothing else:
			//   - a ramp still pending leaves exchange SET, so this falls straight through
			//     to the flush_slews() cancel below, which is the right treatment for it;
			//   - case 0's dmode-1 timeout force-clears exchange AND flushflag together, so
			//     an abandoned ramp does not wait here either;
			//   - a freshly built channel has flushflag cleared by pre_main_build, so
			//     OpenChannel's start never waits;
			//   - every in-tree restore call (SetDSPBuffsize, SetDSPSamplerate, RXASetNC,
			//     TXASetNC) reaches case 1 only after its own SetChannelState(0,1), which
			//     leaves flushflag clear on both of its exits. None of them wait either.
			// The only caller that can reach this wait is a host that stopped with dmode 0
			// and clocked the ramp out, which is the case that was broken.
			//
			// OUTSIDE csEXCH, and that is load-bearing: flushChannel needs csEXCH to finish
			// and clear flushflag, so waiting while holding it would guarantee the timeout
			// instead of the flush. Waiting here cannot join the three-way cycle either,
			// because this thread holds NO channel lock while it waits, and the host cannot
			// be inside fexchange* on it — WdspChannel::setRunning() and open() both take
			// the control fence, which refuses while a processIq() callback is in flight.
			// Nothing that must run to satisfy this wait can be blocked by it: csDSP is
			// never held across an unbounded wait (dexchange() only memcpys and releases),
			// and flush_iobuffs()'s Sem_BuffReady drain is a 1 ms-timeout poll.
			//
			// BOUNDED by the same count/timeout as case 0, for the same reason patch 4
			// bounds its handshake: a flush thread that never runs must not hang a start
			// forever. Falling through after the cap leaves exactly today's behaviour, no
			// worse. Cost of the wait, measured over 132 starts across spacings 0-10:
			// setRunning(true) mean 251 us, max 3.1 ms, against mean 0.83 us / max 3.1 us
			// with this block reverted. It is 0 in every path listed above; the ~3 ms is
			// one Sleep(1) granularity plus flush_iobuffs()'s own 1 ms-timeout drain, paid
			// only by the restart that would otherwise have killed the channel.
			while (!_InterlockedAnd (&ch[channel].exchange, 1) &&
					_InterlockedAnd (&ch[channel].flushflag, 1) &&
					count < timeout)
			{
				Sleep (1);
				count++;
			}
			EnterCriticalSection (&ch[channel].csEXCH);
			flush_slews (a);
			InterlockedBitTestAndSet (&a->slew.upflag, 0);
			InterlockedBitTestAndSet (&ch[channel].iob.ch_upslew, 0);
			InterlockedBitTestAndReset (&ch[channel].iob.pc->exec_bypass, 0);
			InterlockedBitTestAndSet (&ch[channel].exchange, 0);
			LeaveCriticalSection (&ch[channel].csEXCH);
			break;
		}
	}
	return prior_state;
}

// AetherSDR patch 11: discard TX history when the host stops clocking on unkey.
// Caller excludes fexchange/control calls. A pending clocked flush must finish
// through its normal path; do not race its worker or hide an unfinished stop.
PORT
int DiscardTXAChannelData (int channel)
{
    IOB a = ch[channel].iob.pc;
    int discarded = 0;
    if (ch[channel].type != 1) {
        return 0;
    }
    EnterCriticalSection (&ch[channel].csDSP);
    EnterCriticalSection (&ch[channel].csEXCH);
    if (!_InterlockedAnd (&ch[channel].flushflag, 1))
    {
        InterlockedBitTestAndReset (&ch[channel].exchange, 0);
        InterlockedBitTestAndSet (&a->exec_bypass, 0);
        ch[channel].state = 0;
        flush_iobuffs (channel);
        flush_main (channel);
        discarded = 1;
    }
    LeaveCriticalSection (&ch[channel].csEXCH);
    LeaveCriticalSection (&ch[channel].csDSP);
    return discarded;
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