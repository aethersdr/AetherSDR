/*  dexp.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2018, 2019 Warren Pratt, NR0V

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

DEXP pdexp[4];

DELRING calc_delring (int rsize, int size, int delay, double* in, double* out)
{
	DELRING a = (DELRING) malloc0 (sizeof (delring));
	a->rsize = rsize;
	a->size = size;
	a->rdelay = delay;
	a->in = in;
	a->out = out;
	a->ring = (double *) malloc0 (a->rsize * sizeof (complex));
	a->inptr = a->rdelay;
	a->outptr = 0;
	return a;
}

void decalc_delring (DELRING a)
{
	_aligned_free (a->ring);
	_aligned_free (a);
}

void flush_delring (DELRING a)
{
	memset (a->ring, 0, a->rsize * sizeof (complex));
	a->inptr = a->rdelay;
	a->outptr = 0;
}

void xdelring (DELRING a)
{
	int first, second;
	// copy in
	if (a->size > (a->rsize - a->inptr))
	{
		first = a->rsize - a->inptr;
		second = a->size - first;
	}
	else
	{
		first = a->size;
		second = 0;
	}
	memcpy (a->ring + 2 * a->inptr, a->in, first * sizeof (complex));
	memcpy (a->ring, a->in + 2 * first, second * sizeof (complex));
	a->inptr = (a->inptr + a->size) % a->rsize;
	// copy out
	if (a->size > (a->rsize - a->outptr))
	{
		first = a->rsize - a->outptr;
		second = a->size - first;
	}
	else
	{
		first = a->size;
		second = 0;
	}
	memcpy (a->out, a->ring + 2 * a->outptr, first * sizeof (complex));
	memcpy (a->out + 2 * first, a->ring, second * sizeof (complex));
	a->outptr = (a->outptr + a->size) % a->rsize;
}

void calc_slews (DEXP a)
{
	int i;
	double delta, theta;
	delta = PI / (double)a->nattack;
	theta = 0.0;
	for (i = 0; i <= a->nattack; i++)
	{
		a->cattack[i] = a->low_gain + (1.0 - a->low_gain) * 0.5 * (1.0 - cos (theta));
		theta += delta;
	}
	delta = PI / (double)a->ndecay;
	theta = 0.0;
	for (i = 0; i <= a->ndecay; i++)
	{
		a->cdecay[i] = a->low_gain + (1.0 - a->low_gain) * 0.5 * (1.0 + cos (theta));
		theta += delta;
	}
}

void calc_buffs (DEXP a)
{
	a->trigsig   = (double *)malloc0 (2 * a->size * sizeof(complex));	// allow for double-sized output of filter
	a->delsig    = (double *)malloc0 (    a->size * sizeof(complex));
	a->audbuffer = (double *)malloc0 (    a->size * sizeof(complex));
}

void decalc_buffs (DEXP a)
{
	_aligned_free (a->audbuffer);
	_aligned_free (a->delsig);
	_aligned_free (a->trigsig);
}

void calc_dexp (DEXP a)
{
	// trigger signal preparation
	a->avm = exp(-1.0 / (a->rate * a->dettau));
	a->onem_avm = 1.0 - a->avm;
	a->avsig = 0.0;
	// level change
	a->nattack = (int)(a->tattack * a->rate);
	a->ndecay = (int)(a->tdecay * a->rate);
	a->cattack = (double *)malloc0((a->nattack + 1) * sizeof(double));
	a->cdecay = (double *)malloc0((a->ndecay + 1) * sizeof(double));
	a->low_gain = 1.0 / a->exp_ratio;
	calc_slews(a);
	// control
	a->state = 0;
	a->count = 0;
	a->hold_thresh = a->hysteresis_ratio * a->attack_thresh;	// hysteresis ratio < 1.0
	a->nhold = (int)(a->thold * a->rate);
	// vox
	a->vox_count = (int)(a->audelay * a->rate);
	// audio delay
	a->audring = calc_delring ((int)a->rate, a->size, (int)(a->audelay * a->rate), a->audbuffer, a->out);
}

void decalc_dexp (DEXP a)
{
	decalc_delring (a->audring);
	_aligned_free (a->cdecay);
	_aligned_free (a->cattack);
}

void calc_filter (DEXP a)
{
	double* impulse;
	// 2.0 gain on filter is somewhat arbitrarily chosen to get trigger input similar to that without the filter, knowing
	//    that for any reasonable use of the filter there will be a reduction in trigger signal.
	impulse = fir_bandpass (a->nc, a->low_cut, a->high_cut, a->rate, a->wintype, 1, 2.0/(double)(2 * a->size));
	// print_impulse ("scf.txt", a->nc, impulse, 1, 0);
	a->p = create_fircore (a->size, a->in, a->trigsig, a->nc, 1, 4, impulse);
	_aligned_free (impulse);
	a->scdring = calc_delring (a->size + a->nc / 2, a->size, a->nc / 64, a->in, a->delsig);
}

void decalc_filter (DEXP a)
{
	destroy_fircore (a->p);
	decalc_delring (a->scdring);
}

void calc_antivox(DEXP a)
{
	a->antivox_mult = exp(-1.0 / (a->antivox_rate * a->antivox_tau));
	a->antivox_onemmult = 1.0 - a->antivox_mult;
	a->antivox_data = (double *) malloc0 (a->antivox_size * sizeof (complex));
}

void decalc_antivox(DEXP a)
{
	_aligned_free (a->antivox_data);
}

PORT
void create_dexp (int id, int run_dexp, int size, double* in, double* out, int rate, double dettau, double tattack, double tdecay, 
	double thold, double exp_ratio, double hyst_ratio, double attack_thresh, int nc, int wtype, double lowcut, double highcut, 
	int run_filt, int run_vox, int run_audelay, double audelay, void (__stdcall *pushvox)(int id, int active),
	int antivox_run, int antivox_size, int antivox_rate, double antivox_gain, double antivox_tau)
{
	DEXP a = (DEXP) malloc0 (sizeof (dexp));
	a->id = id;
	a->run_dexp = run_dexp;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = (double)rate;
	a->dettau = dettau;
	a->tattack = tattack;
	a->tdecay = tdecay;
	a->thold = thold;
	a->exp_ratio = exp_ratio;
	a->hysteresis_ratio = hyst_ratio;
	a->attack_thresh = attack_thresh;
	a->nc = nc;
	a->wintype = wtype;
	a->low_cut = lowcut;
	a->high_cut = highcut;
	a->run_filt = run_filt;
	a->run_vox = run_vox;
	a->run_audelay = run_audelay;
	a->audelay = audelay;
	a->pushvox = pushvox;
	a->antivox_run = antivox_run;
	a->antivox_size = antivox_size;
	a->antivox_rate = (double)antivox_rate;
	a->antivox_gain = antivox_gain;
	a->antivox_tau = antivox_tau;
	calc_buffs (a);
	calc_dexp (a);
	calc_filter (a);
	calc_antivox (a);
	InitializeCriticalSectionAndSpinCount(&a->cs_update, 2500);
	pdexp[id] = a;
	return;
}

PORT
void destroy_dexp (int id)
{
	DEXP a = pdexp[id];
	DeleteCriticalSection (&a->cs_update);
	decalc_antivox (a);
	decalc_filter (a);
	decalc_dexp (a);
	decalc_buffs (a);
	_aligned_free (a);
}

PORT
void flush_dexp (int id)
{
	DEXP a = pdexp[id];
	memset (a->audbuffer, 0, a->size * sizeof (complex));
	memset (a->trigsig, 0, a->size * sizeof (complex));
	memset (a->delsig,  0, a->size * sizeof (complex));
	a->avsig = 0.0;
	a->state = 0;
	a->count = 0;
	flush_fircore (a->p);
	flush_delring (a->scdring);
	flush_delring (a->audring);
}

enum _dexpstate
{
	DEXP_LOW,
	DEXP_ATTACK,
	DEXP_HIGH,
	DEXP_HOLD,
	DEXP_DECAY
};

PORT
void xdexp (int id)
{
	DEXP a = pdexp[id];
	int i;
	double sig, gain, asig;
	double max = 0.0;
	EnterCriticalSection (&a->cs_update);

	// ******* BEGIN SIDE-CHANNEL FILTER *******
	if (a->run_filt)
	{
		xdelring (a->scdring);		// input is 'a->in'; output is 'a->delsig'
		xfircore (a->p);			// input is 'a->in'; output is 'a->trigsig'
	}
	else
	{
		memcpy (a->delsig,  a->in, a->size * sizeof (complex));
		memcpy (a->trigsig, a->in, a->size * sizeof (complex));
	}
	// ******* END SIDE-CHANNEL FILTER *******

	// ******* CALCULATE ANTIVOX LEVEL *******
	if (a->state == DEXP_LOW && a->antivox_new != 0)
	{
		// if VOX is currently NOT triggered, and, if we have new antivox data to process
		for (i = 0; i < a->antivox_size; i++)
		{
			sig = sqrt (a->antivox_data[2 * i + 0] * a->antivox_data[2 * i + 0] + a->antivox_data[2 * i + 1] * a->antivox_data[2 * i + 1]);
			a->antivox_level = a->antivox_mult * a->antivox_level + a->antivox_onemmult * sig;
		}
		// set the new_data flag to zero
		a->antivox_new = 0;
	}
	// ******* END CALCULATE ANTIVOX LEVEL *******

	// ******* BEGIN DEXP *******
	// uses 'a->trigsig' as trigger signal; uses 'a->delsig' as audio input
	// 'a->audbuffer' is audio output
	// DEXP code runs continuously so it can be used to trigger VOX also.
	for (i = 0; i < a->size; i++)
	{
		sig = sqrt (a->trigsig[2 * i + 0] * a->trigsig[2 * i + 0] + a->trigsig[2 * i + 1] * a->trigsig[2 * i + 1]);
		a->avsig = a->avm * a->avsig + a->onem_avm * sig;
		if (a->avsig > max)  max = a->avsig;
		switch (a->state)
		{
		case DEXP_LOW:
			if (a->antivox_run)
				asig = a->avsig - a->antivox_gain * a->antivox_level;
			else
				asig = a->avsig;
			if (asig > a->attack_thresh)
			{
				a->state = DEXP_ATTACK;
				a->count = a->nattack;
			}
			a->audbuffer[2 * i + 0] = a->low_gain * a->delsig[2 * i + 0];
			a->audbuffer[2 * i + 1] = a->low_gain * a->delsig[2 * i + 1];

			// ******* BEGIN VOX *******
			// If we're going to attack, turn on VOX immediately.
			// Prepare 'vox_count' for the next turnoff too.
			if (a->run_vox && a->state == DEXP_ATTACK)
			{
				(a->pushvox)(a->id, 1);
				// Set vox_count for delay IF the audio delay is also enabled.
				if (a->run_audelay)
					a->vox_count = (int)(a->audelay * a->rate);
				else
					a->vox_count = 1;
			}
			// If we're sitting in this state and the delayed vox count expires, turn OFF VOX.
			else if (a->run_vox && --(a->vox_count) == 0)
				(a->pushvox)(a->id, 0);
			// Don't let 'vox_count' keep counting down.
			else if (a->vox_count < 0)
				a->vox_count = 0;
			// ******* END VOX *******

			break;
		case DEXP_ATTACK:
			gain = a->cattack[a->nattack - a->count];
			a->audbuffer[2 * i + 0] = a->delsig[2 * i + 0] * gain;
			a->audbuffer[2 * i + 1] = a->delsig[2 * i + 1] * gain;
			if (a->count-- == 0)
				a->state = DEXP_HIGH;
			break;
		case DEXP_HIGH:
			if (a->avsig < a->hold_thresh)
			{
				a->state = DEXP_HOLD;
				a->count = a->nhold;
			}
			a->audbuffer[2 * i + 0] = a->delsig[2 * i + 0];
			a->audbuffer[2 * i + 1] = a->delsig[2 * i + 1];
			break;
		case DEXP_HOLD:
			a->audbuffer[2 * i + 0] = a->delsig[2 * i + 0];
			a->audbuffer[2 * i + 1] = a->delsig[2 * i + 1];
			if (a->avsig > a->attack_thresh)
				a->state = DEXP_HIGH;
			else if (a->count-- == 0)
			{
				a->state = DEXP_DECAY;
				a->count = a->ndecay;
			}
			break;
		case DEXP_DECAY:
			gain = a->cdecay[a->ndecay - a->count];
			a->audbuffer[2 * i + 0] = a->delsig[2 * i + 0] * gain;
			a->audbuffer[2 * i + 1] = a->delsig[2 * i + 1] * gain;
			if (a->count-- == 0)
				a->state = DEXP_LOW;
			break;
		}
	}
	a->peak = max;
	// If DEXP functionality is set to OFF, copy its input to overwrite its output.
	if (!a->run_dexp)
		memcpy (a->audbuffer, a->delsig, a->size * sizeof (complex));
	// ******* END DEXP *******

	// ******* BEGIN AUDIO DELAY *******
	if (a->run_audelay)
		xdelring (a->audring);		// uses 'a->audbuffer' as audio input; uses 'a->out' as audio output
	else
		memcpy (a->out, a->audbuffer, a->size * sizeof (complex));
	// ******* END AUDIO DELAY *******

	LeaveCriticalSection (&a->cs_update);
}

PORT
void SendCBPushDexpVox (int id, void (__stdcall *pushvox)(int id, int active))
{
	// Call to set the address of the callback to operate VOX.
	DEXP a = pdexp[id];
	a->pushvox = pushvox;
}

PORT
void SetDEXPRun (int id, int run)
{
	// run != 0, puts dexp in the audio processing path; otherwise, it's only used to trigger VOX
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	a->run_dexp = run;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPSize (int id, int size)
{
	// There are some constraints on the input/output buffer sizes.
	//   * must be a power-of-two
	//   * must be less than or equal to 'nc', the number of filter coefficients, which is also a power-of-two
	//   * must be less than 'rate' samples because of the sizing of 'audring'
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_filter (a);
	decalc_dexp (a);
	decalc_buffs (a);
	a->size = size;
	calc_buffs (a);
	calc_dexp (a);
	calc_filter (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPIOBuffers (int id, double* in, double* out)
{
	// Sets the input/output buffers.  They can be the same.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_filter (a);
	decalc_dexp (a);
	a->in = in;
	a->out = out;
	calc_dexp (a);
	calc_filter (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPRate (int id, double rate)
{
	// Sets the sample rate.
	// This is used for timing and filter calculations as well as sizing 'audring'.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_filter (a);
	decalc_dexp (a);
	a->rate = rate;
	calc_dexp (a);
	calc_filter (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPDetectorTau (int id, double tau)
{
	// Time-constant for smoothing the signal for detection (seconds).
	// 0.01 seconds is a good starting point to try.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_dexp (a);
	a->dettau = tau;
	calc_dexp (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPAttackTime (int id, double time)
{
	// Set attack time, seconds.
	// 0.002 - 0.100 should be a good range.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_dexp (a);
	a->tattack = time;
	calc_dexp (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPReleaseTime (int id, double time)
{
	// Set release time, seconds.
	// 0.002 - 0.999 should be a good range.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_dexp (a);
	a->tdecay = time;
	calc_dexp (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPHoldTime (int id, double time)
{
	// Set hold time, seconds.
	// 0.000 - 2.000 should be a good range.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_dexp (a);
	a->thold = time;
	calc_dexp (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPExpansionRatio (int id, double ratio)
{
	// Set expansion ratio.  High_gain = 1.0; Low_gain = 1.0/exp_ratio.
	// Range of 1.0 - 30.0 should be good.  Could use dB:  0.0 - 30.0dB.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_dexp (a);
	a->exp_ratio = ratio;
	calc_dexp (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPHysteresisRatio (int id, double ratio)
{
	// Set Hysteresis Ratio.  Hold_thresh = hysteresis_ratio * Attack_thresh.
	// Expose to operator in dB:  0.0dB - 9.9dB should be good (1.000 - 0.320).
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_dexp (a);
	a->hysteresis_ratio = ratio;
	calc_dexp (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPAttackThreshold (int id, double thresh)
{
	// Set attack threshold.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_dexp (a);
	a->attack_thresh = thresh;
	calc_dexp (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPFilterTaps (int id, int taps)
{
	// Set number of taps.  Must be a power of two and an even multiple of 'size'.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_filter (a);
	a->nc = taps;
	calc_filter (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPWindowType (int id, int type)
{
	// Set filter window type.
	//   * 0 - 4-term Blackman-Harris.
	//   * 1 - 7-term Blackman-Harris.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_filter (a);
	a->wintype = type;
	calc_filter (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPLowCut (int id, double lowcut)
{
	// Set side-channel filter low_cut (Hertz).
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_filter (a);
	a->low_cut = lowcut;
	calc_filter (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPHighCut (int id, double highcut)
{
	// Set side-channel filter high_cut (Hertz).
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_filter (a);
	a->high_cut = highcut;
	calc_filter (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPRunSideChannelFilter (int id, int run)
{
	// Turn OFF/ON the side-channel filter and its compensating delay.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	a->run_filt = run;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPRunVox (int id, int run)
{
	// Turn OFF/ON calls to 'pushvox(...)'.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	a->run_vox = run;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPRunAudioDelay (int id, int run)
{
	// Turn OFF/ON audio delay line.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	a->run_audelay = run;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetDEXPAudioDelay (int id, double delay)
{
	// Set the audio delay, seconds.
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_dexp (a);
	a->audelay = delay;
	calc_dexp (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void GetDEXPPeakSignal (int id, double* peak)
{
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	*peak = a->peak;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetAntiVOXRun (int id, int run)
{
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	a->antivox_run = run;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetAntiVOXSize (int id, int size)
{
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_antivox(a);
	a->antivox_size = size;
	calc_antivox(a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetAntiVOXRate (int id, double rate)
{
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_antivox(a);
	a->antivox_rate = rate;
	calc_antivox(a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetAntiVOXGain (int id, double gain)
{
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	a->antivox_gain = gain;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetAntiVOXDetectorTau (int id, double tau)
{
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	decalc_antivox (a);
	a->antivox_tau = tau;
	calc_antivox (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT 
void SendAntiVOXData (int id, int nsamples, double* data)
{
	// note:  'nsamples' is not used as it has been previously specified
	DEXP a = pdexp[id];
	EnterCriticalSection (&a->cs_update);
	memcpy (a->antivox_data, data, a->antivox_size * sizeof (complex));
	a->antivox_new = 1;
	LeaveCriticalSection (&a->cs_update);
}
