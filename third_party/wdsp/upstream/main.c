/*  main.c

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

void wdspmain (void *pargs)
{
	DWORD taskIndex = 0;
	HANDLE hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);
	if (hTask != 0) AvSetMmThreadPriority(hTask, 2);
	else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	int channel = (int)(uintptr_t)pargs;
	// AetherSDR patch 4: this worker's generation, read once and kept in a
	// LOCAL for the exit store below. Deliberately not published anywhere in
	// ch[]: a successor worker would overwrite it, and an abandoned worker
	// would then acknowledge on the successor's behalf (#5411 second-opinion
	// review). dexchange() returning rather than calling _endthread() is what
	// makes one local sufficient.
	const long myGen = _InterlockedAnd (&ch[channel].mainGen, ~0L);
	while (_InterlockedAnd (&ch[channel].run, 1))
	{
		WaitForSingleObject(ch[channel].iob.pd->Sem_BuffReady,INFINITE);
		EnterCriticalSection (&ch[channel].csDSP);
		if (!_InterlockedAnd (&ch[channel].iob.pd->exec_bypass, 1))
		{
			// AetherSDR patch 4: dexchange() reports "run cleared, unwind"
			// rather than calling _endthread() mid-function. Unlock and leave
			// the loop so the tail below runs — the worker's single exit.
			int stop = 0;
			switch (ch[channel].type)
			{
			case 0:		// rxa
				stop = dexchange (channel, rxa[channel].outbuff, rxa[channel].inbuff);
				if (!stop) xrxa (channel);
				break;
			case 1:		// txa
				stop = dexchange (channel, txa[channel].outbuff, txa[channel].inbuff);
				if (!stop) xtxa (channel);
				break;
			case 31:	//

				break;
			}
			if (stop)
			{
				LeaveCriticalSection (&ch[channel].csDSP);
				break;
			}
		}
		LeaveCriticalSection (&ch[channel].csDSP);
	}
	if (hTask != 0) AvRevertMmThreadCharacteristics (hTask);
	// AetherSDR patch 4: the LAST statement. After this store the thread touches
	// nothing in ch[channel] or its iob, so pre_main_destroy() may free them.
	// Stores THIS worker's generation, so a store from an abandoned worker can
	// never satisfy a later generation's wait.
	InterlockedExchange (&ch[channel].mainExited, myGen);
}

void create_main (int channel)
{
	switch (ch[channel].type)
	{
	case 0:
		create_rxa (channel);
		break;
	case 1:
		create_txa (channel);
		break;
	case 31:  //
		
		break;
	}
}

void destroy_main (int channel)
{
	switch (ch[channel].type)
	{
	case 0:
		destroy_rxa (channel);
		break;
	case 1:
		destroy_txa (channel);
		break;
	case 31:  //
		
		break;
	}
}

void flush_main (int channel)
{
	switch (ch[channel].type)
	{
	case 0:
		flush_rxa (channel);
		break;
	case 1:
		flush_txa (channel);
		break;
	case 31:
		
		break;
	}
}

void setInputSamplerate_main (int channel)
{
	switch (ch[channel].type)
	{
	case 0:
		setInputSamplerate_rxa (channel);
		break;
	case 1:
		setInputSamplerate_txa (channel);
		break;
	case 31:  //

		break;
	}
}

void setOutputSamplerate_main (int channel)
{
	switch (ch[channel].type)
	{
	case 0:
		setOutputSamplerate_rxa (channel);
		break;
	case 1:
		setOutputSamplerate_txa (channel);
		break;
	case 31:  //

		break;
	}
}

void setDSPSamplerate_main (int channel)
{
	switch (ch[channel].type)
	{
	case 0:
		setDSPSamplerate_rxa (channel);
		break;
	case 1:
		setDSPSamplerate_txa (channel);
		break;
	case 31:  //

		break;
	}
}

void setDSPBuffsize_main (int channel)
{
	switch (ch[channel].type)
	{
	case 0:
		setDSPBuffsize_rxa (channel);
		break;
	case 1:
		setDSPBuffsize_txa (channel);
		break;
	case 31:  //

		break;
	}
}
