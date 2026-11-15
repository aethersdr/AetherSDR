/*  rmatch.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2017, 2018, 2022 Warren Pratt, NR0V

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

MAV create_mav (int ringmin, int ringmax, double nom_value)
{
	MAV a = (MAV) malloc0 (sizeof (mav));
	a->ringmin = ringmin;
	a->ringmax = ringmax;
	a->nom_value = nom_value;
	a->ring = (int *) malloc0 (a->ringmax * sizeof (int));
	a->mask = a->ringmax - 1;
	a->i = 0;
	a->load = 0;
	a->sum = 0;
	return a;
}

void destroy_mav (MAV a)
{
	_aligned_free (a->ring);
	_aligned_free (a);
}

void flush_mav (MAV a)
{
	memset (a->ring, 0, a->ringmax * sizeof (int));
	a->i = 0;
	a->load = 0;
	a->sum = 0;
}
void xmav (MAV a, int input, double* output)
{
	if (a->load >= a->ringmax)
		a->sum -= a->ring[a->i];
    if (a->load < a->ringmax) a->load++;
    a->ring[a->i] = input;
	a->sum += a->ring[a->i];
    
	if (a->load >= a->ringmin)
		*output = (double)a->sum / (double)a->load;
	else
		*output = a->nom_value;
    a->i = (a->i + 1) & a->mask;
}

AAMAV create_aamav (int ringmin, int ringmax, double nom_ratio)
{
	AAMAV a = (AAMAV) malloc0 (sizeof (aamav));
	a->ringmin = ringmin;
	a->ringmax = ringmax;
	a->nom_ratio = nom_ratio;
	a->ring = (int *) malloc0 (a->ringmax * sizeof (int));
	a->mask = a->ringmax - 1;
	a->i = 0;
	a->load = 0;
	a->pos = 0;
	a->neg = 0;
	return a;
}

void destroy_aamav (AAMAV a)
{
	_aligned_free (a->ring);
	_aligned_free (a);
}

void flush_aamav (AAMAV a)
{
	memset (a->ring, 0, a->ringmax * sizeof (int));
	a->i = 0;
	a->load = 0;
	a->pos = 0;
	a->neg = 0;
}

void xaamav (AAMAV a, int input, double* output)
{
	if (a->load >= a->ringmax)
    {
        if (a->ring[a->i] >= 0)
            a->pos -= a->ring[a->i];
        else
            a->neg += a->ring[a->i];
    }
    if (a->load <= a->ringmax) a->load++;
    a->ring[a->i] = input;
    if (a->ring[a->i] >= 0)
        a->pos += a->ring[a->i];
    else
        a->neg -= a->ring[a->i];
	if (a->load >= a->ringmin)
		*output = (double)a->neg / (double)a->pos;
	else if (a->neg > 0 && a->pos > 0)
	{
		double frac = (double)a->load / (double)a->ringmin;
		*output = (1.0 - frac) * a->nom_ratio + frac * ((double)a->neg / (double)a->pos);
	}
	else
		*output = a->nom_ratio;
    a->i = (a->i + 1) & a->mask;
}

void calc_rmatch (RMATCH a)
{
	int m;
	double theta, dtheta;
	int max_ring_insize;
	a->nom_ratio = (double)a->nom_outrate / (double)a->nom_inrate;
	max_ring_insize = (int)(1.0 + (double)a->insize * (1.05 * a->nom_ratio));
	if (a->ringsize < 2 * max_ring_insize)  a->ringsize = 2 * max_ring_insize;
	if (a->ringsize < 2 * a->outsize) a->ringsize = 2 * a->outsize;
	a->ring = (double *) malloc0 (a->ringsize * sizeof (complex));
	a->rsize = a->ringsize;
	a->n_ring = a->rsize / 2;
	a->iin = a->rsize / 2;
	a->iout = 0;
	a->resout = (double *) malloc0 (max_ring_insize * sizeof (complex));
	a->v = create_varsamp (1, a->insize, a->in, a->resout, a->nom_inrate, a->nom_outrate, 
		a->fc_high, a->fc_low, a->R, a->gain, a->var, a->varmode);
	a->ffmav = create_aamav (a->ff_ringmin, a->ff_ringmax, a->nom_ratio);
	a->propmav = create_mav (a->prop_ringmin, a->prop_ringmax, 0.0);
	a->pr_gain = a->prop_gain * 48000.0 / (double)a->nom_outrate;	// adjust gain for rate
	a->inv_nom_ratio = (double)a->nom_inrate / (double)a->nom_outrate;
	a->feed_forward = 1.0;
	a->av_deviation = 0.0;
	InitializeCriticalSectionAndSpinCount (&a->cs_ring, 2500);
	InitializeCriticalSectionAndSpinCount (&a->cs_var,  2500);
	a->ntslew = (int)(a->tslew * a->nom_outrate);
	if (a->ntslew + 1 > a->rsize / 2) a->ntslew = a->rsize / 2 - 1;
	a->cslew = (double *) malloc0 ((a->ntslew + 1) * sizeof (double));
	dtheta = PI / (double)a->ntslew;
	theta = 0.0;
	for (m = 0; m <= a->ntslew; m++)
	{
		a->cslew[m] = 0.5 * (1.0 - cos (theta));
		theta += dtheta;
	}
	a->baux = (double *) malloc0 (a->ringsize / 2 * sizeof (complex));
	a->readsamps = 0;
	a->writesamps = 0;
	a->read_startup = (unsigned int)((double)a->nom_outrate * a->startup_delay);
	a->write_startup = (unsigned int)((double)a->nom_inrate * a->startup_delay);
	a->control_flag = 0;
	// diagnostics
	a->underflows = 0;
	a->overflows = 0;
}

void decalc_rmatch (RMATCH a)
{
	_aligned_free (a->baux);
	_aligned_free (a->cslew);
	DeleteCriticalSection (&a->cs_var);
	DeleteCriticalSection (&a->cs_ring);
	destroy_mav (a->propmav);
	destroy_aamav (a->ffmav);
	destroy_varsamp (a->v);
	_aligned_free (a->resout);
	_aligned_free (a->ring);
}

RMATCH create_rmatch (
	int run,				// 0 - input and output calls do nothing; 1 - operates normally
	double* in,				// pointer to input buffer
	double* out,			// pointer to output buffer
	int insize,				// size of input buffer
	int outsize,			// size of output buffer
	int nom_inrate,			// nominal input samplerate
	int nom_outrate,		// nominal output samplerate
	double fc_high,			// high cutoff frequency if lower than max
	double fc_low,			// low cutoff frequency if higher than zero
	double gain,			// gain to be applied during this process
	double startup_delay,	// time (seconds) to delay before beginning measurements to control variable resampler
	int auto_ringsize,		// 0 specified ringsize is used; 1 ringsize is auto-optimized - FEATURE NOT IMPLEMENTED!!
	int ringsize,			// specified ringsize; max ringsize if 'auto' is enabled
	int R,					// density factor for varsamp coefficients
	double var,				// initial value of variable resampler ratio (value of ~1.0)
	int ffmav_min,			// minimum feed-forward moving average size to put full weight on data in the ring
	int ffmav_max,			// maximum feed-forward moving average size - MUST BE A POWER OF TWO!
	double ff_alpha,		// feed-forward exponential averaging multiplier
	int prop_ringmin,		// proportional feedback min moving average ringsize
	int prop_ringmax,		// proportional feedback max moving average ringsize - MUST BE A POWER OF TWO!
	double prop_gain,		// proportional feedback gain factor
	int varmode,			// 0 - use same var for all samples of the buffer; 1 - interpolate from old_var to this var
	double tslew			// slew/blend time (seconds)
	)
{
	RMATCH a = (RMATCH) malloc0 (sizeof (rmatch));
	a->run = run;
	a->in = in;
	a->out = out;
	a->insize = insize;
	a->outsize = outsize;
	a->nom_inrate = nom_inrate;
	a->nom_outrate = nom_outrate;
	a->fc_high = fc_high;
	a->fc_low = fc_low;
	a->gain = gain;
	a->startup_delay = startup_delay;
	a->auto_ringsize = auto_ringsize;
	a->ringsize = ringsize;
	a->R = R;
	a->var = var;
	a->ff_ringmin = ffmav_min;
	a->ff_ringmax = ffmav_max;		// must be a power of two
	a->ff_alpha = ff_alpha;
	a->prop_ringmin = prop_ringmin;
	a->prop_ringmax = prop_ringmax;	// must be a power of two
	a->prop_gain = prop_gain;
	a->varmode = varmode;
	a->tslew = tslew;
	calc_rmatch(a);
	return a;
}

void destroy_rmatch (RMATCH a)
{
	decalc_rmatch (a);
	_aligned_free (a);
}

void reset_rmatch (RMATCH a)
{
	InterlockedBitTestAndReset (&a->run, 0);
	Sleep (10);
	decalc_rmatch(a);
	calc_rmatch (a);
	InterlockedBitTestAndSet (&a->run, 0);
}

void control (RMATCH a, int change)
{
	{
		double current_ratio;
		xaamav (a->ffmav, change, &current_ratio);
		current_ratio *= a->inv_nom_ratio;
		a->feed_forward = a->ff_alpha * current_ratio + (1.0 - a->ff_alpha) * a->feed_forward;
	}
	{
		int deviation = a->n_ring - a->rsize / 2;
		xmav (a->propmav, deviation, &a->av_deviation);
	}
	EnterCriticalSection (&a->cs_var);
	a->var = a->feed_forward - a->pr_gain * a->av_deviation;
	if (a->var > 1.04) a->var = 1.04;
	if (a->var < 0.96) a->var = 0.96;
	LeaveCriticalSection (&a->cs_var);
}

void blend (RMATCH a)
{
	int i, j;
	for (i = 0, j = a->iout; i <= a->ntslew; i++, j = (j + 1) % a->rsize)
	{
		a->ring[2 * j + 0] = a->cslew[i] * a->ring[2 * j + 0] + (1.0 - a->cslew[i]) * a->baux[2 * i + 0];
		a->ring[2 * j + 1] = a->cslew[i] * a->ring[2 * j + 1] + (1.0 - a->cslew[i]) * a->baux[2 * i + 1];
	}
}

void upslew (RMATCH a, int newsamps)
{
	int i, j;
	i = 0;
	j = a->iin;
	while (a->ucnt >= 0 && i < newsamps)
	{
		a->ring[2 * j + 0] *= a->cslew[a->ntslew - a->ucnt];
		a->ring[2 * j + 1] *= a->cslew[a->ntslew - a->ucnt];
		a->ucnt--;
		i++;
		j = (j + 1) % a->rsize;
	}
}

PORT
void xrmatchIN (void* b, double* in)
{
	RMATCH a = (RMATCH)b;
	if (InterlockedAnd (&a->run, 1))
	{
		int newsamps, first, second, ovfl;
		double var;
		a->v->in = a->in = in;
		EnterCriticalSection (&a->cs_var);
		if (!a->force)
			var = a->var;
		else
			var = a->fvar;
		LeaveCriticalSection (&a->cs_var);
		newsamps = xvarsamp (a->v, var);
		EnterCriticalSection (&a->cs_ring);
		a->n_ring += newsamps;
		if ((ovfl = a->n_ring - a->rsize) > 0)
		{	
			InterlockedIncrement (&a->overflows);
			// a->n_ring = a->rsize / 2;
			a->n_ring = a->rsize; //
			if ((a->ntslew + 1) > (a->rsize - a->iout))
			{
				first = a->rsize - a->iout;
				second = (a->ntslew + 1) - first;
			}
			else
			{
				first = a->ntslew + 1;
				second = 0;
			}
			memcpy (a->baux, a->ring + 2 * a->iout, first * sizeof (complex));
			memcpy (a->baux + 2 * first, a->ring, second * sizeof (complex));
			// a->iout = (a->iout + ovfl + a->rsize / 2) % a->rsize;
			a->iout = (a->iout + ovfl) % a->rsize; //
		}
		if (newsamps > (a->rsize - a->iin))
		{
			first = a->rsize - a->iin;
			second = newsamps - first;
		}
		else
		{
			first = newsamps;
			second = 0;
		}
		memcpy (a->ring + 2 * a->iin, a->resout, first * sizeof (complex));
		memcpy (a->ring, a->resout + 2 * first, second * sizeof (complex));
		if (a->ucnt >= 0) upslew(a, newsamps);
		a->iin = (a->iin + newsamps) % a->rsize;
		if (ovfl > 0) blend (a);
		if (!a->control_flag)
		{
			a->writesamps += a->insize;
			if ((a->readsamps >= a->read_startup) && (a->writesamps >= a->write_startup))
				a->control_flag = 1;
		}
		if (a->control_flag) control (a, a->insize);
		LeaveCriticalSection (&a->cs_ring);
	}
}

void dslew (RMATCH a)
{
	int i, j, k, n;
	int zeros, first, second;
	if (a->n_ring > a->ntslew + 1)
	{
		i = (a->iout + (a->n_ring - (a->ntslew + 1))) % a->rsize;
		j = a->ntslew;
		k = a->ntslew + 1;
		n = a->n_ring - (a->ntslew + 1);
	}
	else
	{
		i = a->iout;
		j = a->ntslew;
		k = a->n_ring;
		n = 0;
	}
	while (k > 0 && j >= 0)
	{
		if (k == 1)
		{
			a->dlast[0] = a->ring[2 * i + 0];
			a->dlast[1] = a->ring[2 * i + 1];
		}
		a->ring[2 * i + 0] *= a->cslew[j];
		a->ring[2 * i + 1] *= a->cslew[j];
		i = (i + 1) % a->rsize;
		j--;
		k--;
		n++;
	}
	while (j >= 0)
	{
		a->ring[2 * i + 0] = a->dlast[0] * a->cslew[j];
		a->ring[2 * i + 1] = a->dlast[1] * a->cslew[j];
		i = (i + 1) % a->rsize;
		j--;
		n++;
	}
	// zeros = a->outsize + a->rsize / 2 - n;
	if ((zeros = a->outsize - n) > 0) //
	{ //
		if (zeros > a->rsize - i)
		{
			first = a->rsize - i;
			second = zeros - first;
		}
		else
		{
			first = zeros;
			second = 0;
		}
		memset (a->ring + 2 * i, 0, first  * sizeof (complex));
		memset (a->ring,         0, second * sizeof (complex));
		n += zeros; //
	} //
	// a->n_ring = a->outsize + a->rsize / 2;
	a->n_ring = n; //
	// a->iin = (a->iout + a->outsize + a->rsize/2) % a->rsize;
	a->iin = (a->iout + a->n_ring) % a->rsize; //
}

PORT
void xrmatchOUT (void* b, double* out)
{
	RMATCH a = (RMATCH)b;
	if (InterlockedAnd (&a->run, 1))
	{
		int first, second;
		a->out = out;
		EnterCriticalSection (&a->cs_ring);
		if (a->n_ring < a->outsize) 
		{
			dslew (a);
			a->ucnt = a->ntslew;
			InterlockedIncrement (&a->underflows);
		}
		if (a->outsize > (a->rsize - a->iout))
		{
			first = a->rsize - a->iout;
			second = a->outsize - first;
		}
		else
		{
			first = a->outsize;
			second = 0;
		}
		memcpy (a->out, a->ring + 2 * a->iout, first * sizeof (complex));
		memcpy (a->out + 2 * first, a->ring, second * sizeof (complex));
		a->iout = (a->iout + a->outsize) % a->rsize;
		a->n_ring -= a->outsize;
		a->dlast[0] = a->out[2 * (a->outsize - 1) + 0];
		a->dlast[1] = a->out[2 * (a->outsize - 1) + 1];
		if (!a->control_flag)
		{
			a->readsamps += a->outsize;
			if ((a->readsamps >= a->read_startup) && (a->writesamps >= a->write_startup))
				a->control_flag = 1;
		}
		if (a->control_flag) control (a, -(a->outsize));
		LeaveCriticalSection (&a->cs_ring);
	}
}

PORT
void getRMatchDiags (void* b, int* underflows, int* overflows, double* var, int* ringsize, int* nring)
{
	RMATCH a = (RMATCH)b;
	*underflows = InterlockedAnd (&a->underflows, 0xFFFFFFFF);
	*overflows  = InterlockedAnd (&a->overflows,  0xFFFFFFFF);
	EnterCriticalSection (&a->cs_var);
	*var = a->var;
	*ringsize = a->ringsize;
	*nring = a->n_ring;
	LeaveCriticalSection (&a->cs_var);
}

PORT
void resetRMatchDiags (void* b)
{
	RMATCH a = (RMATCH)b;
	InterlockedExchange (&a->underflows, 0);
	InterlockedExchange (&a->overflows,  0);
}

PORT
void forceRMatchVar (void* b, int force, double fvar)
{
	RMATCH a = (RMATCH)b;
	EnterCriticalSection (&a->cs_var);
	a->force = force;
	a->fvar = fvar;
	LeaveCriticalSection (&a->cs_var);
}

PORT
void* create_rmatchV(int in_size, int out_size, int nom_inrate, int nom_outrate, int ringsize, double var)
{
	return (void*)create_rmatch (
		1,						// run
		0,						// input buffer, stuffed in other calls
		0,						// output buffer, stuffed in other calls
		in_size,				// input buffer size (complex samples)
		out_size,				// output buffer size (complex samples)
		nom_inrate,				// nominal input sample-rate
		nom_outrate,			// nominal output sample-rate
		0.0,					// fc_high (0.0 -> automatic)
		-1.0,					// fc_low  (-1.0 -> no low cutoff)
		1.0,					// gain
		3.0,					// startup delay (seconds)
		1,						// automatic ring-size [not implemented yet]
		ringsize,				// ringsize
		1024,					// R, coefficient density
		var,					// initial variable ratio
		4096,					// feed-forward moving average min size
		262144,					// feed-forward moving average max size - POWER OF TWO!
		0.01,					// feed-forward exponential smoothing
		4096,					// proportional feedback min moving av ringsize
		16384,					// proportional feedback max moving av ringsize - POWER OF TWO!
		4.0e-06,				// proportional feedback gain
		1,						// linearly interpolate cvar by sample
		0.003 );				// slew time (seconds)
}

PORT
void destroy_rmatchV (void* ptr)
{
	RMATCH a = (RMATCH)ptr;
	destroy_rmatch (a);
}

PORT
void setRMatchInsize (void* ptr, int insize)
{
	RMATCH a = (RMATCH)ptr;
	InterlockedBitTestAndReset (&a->run, 0);
	Sleep (10);
	decalc_rmatch(a);
	a->insize = insize;
	calc_rmatch (a);
	InterlockedBitTestAndSet (&a->run, 0);
}

PORT
void setRMatchOutsize (void* ptr, int outsize)
{
	RMATCH a = (RMATCH)ptr;
	InterlockedBitTestAndReset (&a->run, 0);
	Sleep (10);
	decalc_rmatch(a);
	a->outsize = outsize;
	calc_rmatch (a);
	InterlockedBitTestAndSet (&a->run, 0);
}

PORT
void setRMatchNomInrate (void* ptr, int nom_inrate)
{
	RMATCH a = (RMATCH)ptr;
	InterlockedBitTestAndReset (&a->run, 0);
	Sleep (10);
	decalc_rmatch(a);
	a->nom_inrate = nom_inrate;
	calc_rmatch (a);
	InterlockedBitTestAndSet (&a->run, 0);
}

PORT
void setRMatchNomOutrate (void* ptr, int nom_outrate)
{
	RMATCH a = (RMATCH)ptr;
	InterlockedBitTestAndReset (&a->run, 0);
	Sleep (10);
	decalc_rmatch(a);
	a->nom_outrate = nom_outrate;
	calc_rmatch (a);
	InterlockedBitTestAndSet (&a->run, 0);
}

PORT
void setRMatchRingsize (void* ptr, int ringsize)
{
	RMATCH a = (RMATCH)ptr;
	InterlockedBitTestAndReset (&a->run, 0);
	Sleep (10);
	decalc_rmatch(a);
	a->ringsize = ringsize;
	calc_rmatch (a);
	InterlockedBitTestAndSet (&a->run, 0);
}

PORT
void setRMatchFeedbackGain (void* b, double feedback_gain)
{
	RMATCH a = (RMATCH)b;
	EnterCriticalSection(&a->cs_var);
	a->prop_gain = feedback_gain;
	a->pr_gain = a->prop_gain * 48000.0 / (double)a->nom_outrate;
	LeaveCriticalSection(&a->cs_var);
}

PORT
void setRMatchSlewTime (void* b, double slew_time)
{
	RMATCH a = (RMATCH)b;
	InterlockedBitTestAndReset(&a->run, 0);		// turn OFF new data coming into the rmatch
	Sleep(10);									// wait for processing to cease
	decalc_rmatch(a);							// deallocate all memory EXCEPT the data structure holding all current parameters
	a->tslew = slew_time;						// change the value of 'slew_time'
	calc_rmatch(a);								// recalculate/reallocate everything in the RMATCH
	InterlockedBitTestAndSet(&a->run, 0);		// turn ON the dataflow
}

PORT
void setRMatchSlewTime1(void* b, double slew_time)
{
	RMATCH a = (RMATCH)b;
	double theta, dtheta;
	int m;
	InterlockedBitTestAndReset(&a->run, 0);
	Sleep(10);
	_aligned_free(a->cslew);
	a->tslew = slew_time;
	a->ntslew = (int)(a->tslew * a->nom_outrate);
	if (a->ntslew + 1 > a->rsize / 2) a->ntslew = a->rsize / 2 - 1;
	a->cslew = (double*)malloc0((a->ntslew + 1) * sizeof(double));
	dtheta = PI / (double)a->ntslew;
	theta = 0.0;
	for (m = 0; m <= a->ntslew; m++)
	{
		a->cslew[m] = 0.5 * (1.0 - cos(theta));
		theta += dtheta;
	}
	InterlockedBitTestAndSet(&a->run, 0);
}

PORT
void setRMatchPropRingMin(void* ptr, int prop_min)
{
	RMATCH a = (RMATCH)ptr;
	InterlockedBitTestAndReset(&a->run, 0);
	Sleep(10);
	decalc_rmatch(a);
	a->prop_ringmin = prop_min;
	calc_rmatch(a);
	InterlockedBitTestAndSet(&a->run, 0);
}

PORT
void setRMatchPropRingMax(void* ptr, int prop_max)
{
	RMATCH a = (RMATCH)ptr;
	InterlockedBitTestAndReset(&a->run, 0);
	Sleep(10);
	decalc_rmatch(a);
	a->prop_ringmax = prop_max;	// must be a power of two
	calc_rmatch(a);
	InterlockedBitTestAndSet(&a->run, 0);
}

PORT
void setRMatchFFRingMin(void* ptr, int ff_ringmin)
{
	RMATCH a = (RMATCH)ptr;
	InterlockedBitTestAndReset(&a->run, 0);
	Sleep(10);
	decalc_rmatch(a);
	a->ff_ringmin = ff_ringmin;
	calc_rmatch(a);
	InterlockedBitTestAndSet(&a->run, 0);
}

PORT
void setRMatchFFRingMax(void* ptr, int ff_ringmax)
{
	RMATCH a = (RMATCH)ptr;
	InterlockedBitTestAndReset(&a->run, 0);
	Sleep(10);
	decalc_rmatch(a);
	a->ff_ringmax = ff_ringmax; // must be a power of two
	calc_rmatch(a);
	InterlockedBitTestAndSet(&a->run, 0);
}

PORT
void setRMatchFFAlpha(void* ptr, double ff_alpha)
{
	RMATCH a = (RMATCH)ptr;
	InterlockedBitTestAndReset(&a->run, 0);
	Sleep (10);
	a->ff_alpha = ff_alpha;
	InterlockedBitTestAndSet(&a->run, 0);
}

PORT
void getControlFlag(void* ptr, int* control_flag)
{
	RMATCH a = (RMATCH)ptr;
	EnterCriticalSection(&a->cs_ring);
	*control_flag = a->control_flag;
	LeaveCriticalSection(&a->cs_ring);
}

// the following function is DEPRECATED
// it is intended for Legacy PowerSDR use only
PORT
void* create_rmatchLegacyV(int in_size, int out_size, int nom_inrate, int nom_outrate, int ringsize)
{
	return (void*)create_rmatch(
		1,						// run
		0,						// input buffer, stuffed in other calls
		0,						// output buffer, stuffed in other calls
		in_size,				// input buffer size (complex samples)
		out_size,				// output buffer size (complex samples)
		nom_inrate,				// nominal input sample-rate
		nom_outrate,			// nominal output sample-rate
		0.0,					// fc_high (0.0 -> automatic)
		-1.0,					// fc_low  (-1.0 -> no low cutoff)
		1.0,					// gain
		3.0,					// startup delay (seconds)
		1,						// automatic ring-size [not implemented yet]
		ringsize,				// ringsize
		1024,					// R, coefficient density
		1.0,					// initial variable ratio
		4096,					// feed-forward moving average min size
		262144,					// feed-forward moving average max size - POWER OF TWO!
		0.01,					// feed-forward exponential smoothing
		4096,					// proportional feedback min moving av ringsize
		16384,					// proportional feedback max moving av ringsize - POWER OF TWO!
		1.0e-06,				// proportional feedback gain  ***W4WMT - reduce loop gain a bit for PowerSDR to help Primary buffers > 512
		0,						// linearly interpolate cvar by sample  ***W4WMT - set varmode = 0 for PowerSDR (doesn't work otherwise!?!)
		0.003);					// slew time (seconds)
}