/*  eer.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2014 Warren Pratt, NR0V

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

PORT
EER create_eer (int run, int size, double* in, double* out, double* outM, int rate, double mgain, double pgain, int rundelays, double mdelay, double pdelay, int amiq)
{
	EER a = (EER) malloc0 (sizeof (eer));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->outM = outM;
	a->rate = rate;
	a->mgain = mgain;
	a->pgain = pgain;
	a->rundelays = rundelays;
	a->mdelay = mdelay;
	a->pdelay = pdelay;
	a->amiq = amiq;
	a->mdel = create_delay (
		a->rundelays,								// run
		a->size,									// size
		a->outM,									// input buffer
		a->outM,									// output buffer
		a->rate,									// sample rate
		20.0e-09,									// delta (delay stepsize)
		a->mdelay);									// delay
	a->pdel = create_delay (
		a->rundelays,								// run
		a->size,									// size
		a->out,										// input buffer
		a->out,										// output buffer
		a->rate,									// sample rate
		20.0e-09,									// delta (delay stepsize)
		a->pdelay);									// delay
	InitializeCriticalSectionAndSpinCount(&a->cs_update, 2500);

	a->legacy  = (double *) malloc0 (2048 * sizeof (complex));														/////////////// legacy interface - remove
	a->legacyM = (double *) malloc0 (2048 * sizeof (complex));														/////////////// legacy interface - remove

	return a;
}

PORT
void destroy_eer (EER a)
{
	DeleteCriticalSection (&a->cs_update);
	destroy_delay (a->pdel);
	destroy_delay (a->mdel);
	_aligned_free (a);
}

PORT
void flush_eer (EER a)
{
	flush_delay (a->mdel);
	flush_delay (a->pdel);
}

PORT
void xeer (EER a)
{
	EnterCriticalSection (&a->cs_update);
	if (a->run)
	{
		int i;
		double I, Q, mag;
		for (i = 0; i < a->size; i++)
		{
			I = a->in[2 * i + 0];
			Q = a->in[2 * i + 1];
			a->outM[2 * i + 0] = I * a->mgain;
			a->outM[2 * i + 1] = Q * a->mgain;
			switch (a->amiq)
			{
			case 0:		// send phase info only, magnitude is constant
				mag = sqrt (I * I + Q * Q);
				a->out [2 * i + 0] = a->pgain * I / mag;
				a->out [2 * i + 1] = a->pgain * Q / mag;
				break;
			case 1:		// send magnitude and phase information, I and Q
				a->out [2 * i + 0] = a->pgain * I;
				a->out [2 * i + 1] = a->pgain * Q;
				break;
			case 2:		// send envelope
				mag = sqrt (I * I + Q * Q);
				a->out [2 * i + 0] = a->out[2 * i + 1] = a->pgain * mag;
				break;
			}
		}
		xdelay (a->mdel);		// delay for outM
		xdelay (a->pdel);		// delay for out
	}
	else if (a->out != a->in)
		memcpy (a->out, a->in, a->size * sizeof (complex));
	LeaveCriticalSection (&a->cs_update);
}

/********************************************************************************************************
*																										*
*									    CALLS FOR EXTERNAL USE											*
*																										*
********************************************************************************************************/

#define MAX_EXT_EERS	(2)							// maximum number of EERs called from outside wdsp
__declspec (align (16)) EER peer[MAX_EXT_EERS];		// array of pointers for EERs used EXTERNAL to wdsp


PORT
void create_eerEXT (int id, int run, int size, int rate, double mgain, double pgain, int rundelays, double mdelay, double pdelay, int amiq)
{
	peer[id] = create_eer (run, size, 0, 0, 0, rate, mgain, pgain, rundelays, mdelay, pdelay, amiq);
}

PORT
void destroy_eerEXT (int id)
{
	destroy_eer (peer[id]);
}

PORT
void flush_eerEXT (int id)
{
	flush_eer (peer[id]);
}

PORT
void SetEERRun (int id, int run)
{
	EER a = peer[id];
	EnterCriticalSection (&a->cs_update);
	a->run = run;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetEERAMIQ (int id, int amiq)
{
	EER a = peer[id];
	EnterCriticalSection (&a->cs_update);
	a->amiq = amiq;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetEERMgain (int id, double gain)
{
	EER a = peer[id];
	EnterCriticalSection (&a->cs_update);
	a->mgain = gain;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetEERPgain (int id, double gain)
{
	EER a = peer[id];
	EnterCriticalSection (&a->cs_update);
	a->pgain = gain;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetEERRunDelays (int id, int run)
{
	EER a = peer[id];
	EnterCriticalSection (&a->cs_update);
	a->rundelays = run;
	SetDelayRun (a->mdel, a->rundelays);
	SetDelayRun (a->pdel, a->rundelays);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetEERMdelay (int id, double delay)
{
	EER a = peer[id];
	EnterCriticalSection (&a->cs_update);
	a->mdelay = delay;
	SetDelayValue (a->mdel, a->mdelay);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetEERPdelay (int id, double delay)
{
	EER a = peer[id];
	EnterCriticalSection (&a->cs_update);
	a->pdelay = delay;
	SetDelayValue (a->pdel, a->pdelay);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetEERSize (int id, int size)
{
	EER a = peer[id];
	EnterCriticalSection (&a->cs_update);
	a->size = size;
	SetDelayBuffs (a->mdel, a->size, a->outM, a->outM);
	SetDelayBuffs (a->pdel, a->size, a->out, a->out);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetEERSamplerate (int id, int rate)
{
	EER a = peer[id];
	EnterCriticalSection (&a->cs_update);
	a->rate = rate;
	destroy_delay (a->mdel);
	destroy_delay (a->pdel);
	a->mdel = create_delay (
		a->rundelays,								// run
		a->size,									// size
		a->outM,									// input buffer
		a->outM,									// output buffer
		a->rate,									// sample rate
		20.0e-09,									// delta (delay stepsize)
		a->mdelay);									// delay
	a->pdel = create_delay (
		a->rundelays,								// run
		a->size,									// size
		a->out,										// input buffer
		a->out,										// output buffer
		a->rate,									// sample rate
		20.0e-09,									// delta (delay stepsize)
		a->pdelay);									// delay
	LeaveCriticalSection (&a->cs_update);
}

/********************************************************************************************************
*																										*
*								            POINTER-BASED PROPERTIES	    							*
*																										*
********************************************************************************************************/

PORT
void pSetEERRun (EER a, int run)
{
	EnterCriticalSection (&a->cs_update);
	a->run = run;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void pSetEERAMIQ (EER a, int amiq)
{
	EnterCriticalSection (&a->cs_update);
	a->amiq = amiq;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void pSetEERMgain (EER a, double gain)
{
	EnterCriticalSection (&a->cs_update);
	a->mgain = gain;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void pSetEERPgain (EER a, double gain)
{
	EnterCriticalSection (&a->cs_update);
	a->pgain = gain;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void pSetEERRunDelays (EER a, int run)
{
	EnterCriticalSection (&a->cs_update);
	a->rundelays = run;
	SetDelayRun (a->mdel, a->rundelays);
	SetDelayRun (a->pdel, a->rundelays);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void pSetEERMdelay (EER a, double delay)
{
	EnterCriticalSection (&a->cs_update);
	a->mdelay = delay;
	SetDelayValue (a->mdel, a->mdelay);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void pSetEERPdelay (EER a, double delay)
{
	EnterCriticalSection (&a->cs_update);
	a->pdelay = delay;
	SetDelayValue (a->pdel, a->pdelay);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void pSetEERSize (EER a, int size)
{
	EnterCriticalSection (&a->cs_update);
	a->size = size;
	SetDelayBuffs (a->mdel, a->size, a->outM, a->outM);
	SetDelayBuffs (a->pdel, a->size, a->out, a->out);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void pSetEERSamplerate (EER a, int rate)
{
	EnterCriticalSection (&a->cs_update);
	a->rate = rate;
	destroy_delay (a->mdel);
	destroy_delay (a->pdel);
	a->mdel = create_delay (
		a->rundelays,								// run
		a->size,									// size
		a->outM,									// input buffer
		a->outM,									// output buffer
		a->rate,									// sample rate
		20.0e-09,									// delta (delay stepsize)
		a->mdelay);									// delay
	a->pdel = create_delay (
		a->rundelays,								// run
		a->size,									// size
		a->out,										// input buffer
		a->out,										// output buffer
		a->rate,									// sample rate
		20.0e-09,									// delta (delay stepsize)
		a->pdelay);									// delay
	LeaveCriticalSection (&a->cs_update);
}


/********************************************************************************************************
*																										*
*										   Legacy Interface												*
*																										*
********************************************************************************************************/

PORT
void xeerEXTF (int id, float* inI, float* inQ, float* outI, float* outQ, float* outMI, float* outMQ, int mox, int size)
{
	EER a = peer[id];
	if (mox && a->run)
	{
		int i;
		a->in   = a->legacy;
		a->out  = a->legacy;
		a->outM = a->legacyM;
		a->size = size;
		SetDelayBuffs (a->mdel, a->size, a->outM, a->outM);
		SetDelayBuffs (a->pdel, a->size, a->out, a->out);
		for (i = 0; i < a->size; i++)
		{
			a->legacy[2 * i + 0] = (double)inI[i];
			a->legacy[2 * i + 1] = (double)inQ[i];
		}
		xeer (a);
		for (i = 0; i < a->size; i++)
		{
			outI[i]  = (float)a->legacy [2 * i + 0];
			outQ[i]  = (float)a->legacy [2 * i + 1];
			outMI[i] = (float)a->legacyM[2 * i + 0];
			outMQ[i] = (float)a->legacyM[2 * i + 1];
		}
	}
}