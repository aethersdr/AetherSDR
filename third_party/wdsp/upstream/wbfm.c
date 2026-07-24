/*  wbfm.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2026 Warren Pratt, NR0V

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

#define _CRT_SECURE_NO_WARNINGS
#include "comm.h"

/********************************************************************************************************
*																										*
*                                     STEREO ACTIVATION & INDICATOR                                     *
*																										*
********************************************************************************************************/


typedef struct _indy
{
	int size;
	double rate;
	double* pilot;
	double phs_delta;
	double phs;
	double filt_cutoff;
	double alpha;
	double oneMinusAlpha;
	double filtIstable;
	double filtQstable;
	double low_thresh;
	double high_thresh;
	int on_delay_frames;
	int off_delay_frames;
	int consecutive_high_frames;
	int consecutive_low_frames;
	int stereo;
}indy, * INDY;

static INDY create_indy(int size, double rate, double* pilot)
{
	INDY a = (INDY)malloc0(sizeof(indy));
	a->size = size;
	a->rate = rate;
	a->pilot = pilot;
	a->phs_delta = TWOPI * 19000.0 / a->rate;
	a->phs = 0.0;
	a->filt_cutoff = 4.0;									// Tune this!
	a->alpha = (TWOPI * a->filt_cutoff) / a->rate;
	a->oneMinusAlpha = 1.0 - a->alpha;
	a->filtIstable = 0.0;
	a->filtQstable = 0.0;
	a->low_thresh  = 0.03;									// Tune this!
	a->high_thresh = 0.06;									// Tune this!
	a->on_delay_frames  = (int)((0.5 * a->rate)/ a->size);
	a->off_delay_frames = (int)((1.0 * a->rate)/ a->size);
	a->consecutive_high_frames = 0;
	a->consecutive_low_frames  = 0;
	a->stereo = 0;
	return a;
}

static void destroy_indy(INDY a)
{
	_aligned_free(a);
}

static void flush_indy(INDY a)
{
	a->phs = 0.0;
	a->filtIstable = 0.0;
	a->filtQstable = 0.0;
	a->consecutive_high_frames = 0;
	a->consecutive_low_frames = 0;
	a->stereo = 0;
}

static void xindy(INDY a, int* stereo, double* magnitude)
{
	double mag  = 0.0;
	double mixI = 0.0, mixQ = 0.0;
	double oscI = 0.0, oscQ = 0.0;
	for (int i = 0; i < a->size; i++)
	{
		oscI = +cos(a->phs);
		oscQ = +sin(a->phs);
		a->phs += a->phs_delta;
		if (a->phs >= TWOPI) a->phs -= TWOPI;
		mixI = a->pilot[2 * i + 0] * oscI - a->pilot[2 * i + 1] * oscQ;
		mixQ = a->pilot[2 * i + 0] * oscQ + a->pilot[2 * i + 1] * oscI;
		a->filtIstable = a->alpha * mixI + a->oneMinusAlpha * a->filtIstable;
		a->filtQstable = a->alpha * mixQ + a->oneMinusAlpha * a->filtQstable;
	}
	mag = sqrt(a->filtIstable * a->filtIstable + a->filtQstable * a->filtQstable);
	if (mag > a->high_thresh)
	{
		a->consecutive_low_frames = 0;
		if (a->consecutive_high_frames < INT_MAX) a->consecutive_high_frames++;
		if (a->consecutive_high_frames >= a->on_delay_frames)
			a->stereo = 1;
	}
	else if (mag < a->low_thresh)
	{
		a->consecutive_high_frames = 0;
		if (a->consecutive_low_frames < INT_MAX) a->consecutive_low_frames++;
		if (a->consecutive_low_frames >= a->off_delay_frames)
			a->stereo = 0;
	}
	else
	{
		a->consecutive_low_frames = 0;
		a->consecutive_high_frames = 0;
	}
	*stereo = a->stereo;
	*magnitude = mag;
	//printf("Pilot Mag = %.4e\n", mag);
}


/********************************************************************************************************
*																										*
*                                               DE-EMPHASIS                                             *
*																										*
********************************************************************************************************/

typedef struct _dmph
{
	int run;
	double rate;
	int size;
	double* in;
	double* out;
	double tau;				// de-emphasis time-constant
	double b[2];
	double a[2];
	double x_z1;
	double y_z1;
}dmph, * DMPH;

static void calc_dmph(DMPH a)
{
	double c = 1.0 / tan(1.0 / (2.0 * a->rate * a->tau));
	double denominator = 1.0 + c;
	a->b[0] = 1.0 / denominator;
	a->b[1] = 1.0 / denominator;
	a->a[0] = 1.0;
	a->a[1] = (1.0 - c) / denominator;
	a->x_z1 = 0.0;
	a->y_z1 = 0.0;
}

static DMPH create_dmph(int run, double rate, int dmph_type, int size, double* in, double* out)
{
	DMPH a = (DMPH)malloc0(sizeof(dmph));
	a->run = run;
	a->rate = rate;
	a->size = size;
	a->in = in;
	a->out = out;
	if (!dmph_type) a->tau = 75.0e-6;
	else            a->tau = 50.0e-6;
	calc_dmph(a);
	return a;
}

static void destroy_dmph(DMPH a)
{
	_aligned_free(a);
}

static void xdmph(DMPH a)
{
	if (a->run)
	{
		double x = 0.0, y = 0.0;
		for (int i = 0; i < a->size; i++)
		{
			x = a->in[i];
			y = (a->b[0] * x) + (a->b[1] * a->x_z1) - (a->a[1] * a->y_z1);
			a->x_z1 = x;
			a->y_z1 = y;
			a->out[i] = y;
		}
	}
	else if (a->out != a->in)
		memcpy(a->out, a->in, a->size * sizeof(double));
}

static void flush_dmph(DMPH a)
{
	a->x_z1 = 0.0;
	a->y_z1 = 0.0;
}


/************************************************************************************************
*																								*
*							                  SQUELCH          			    					*
*																								*
************************************************************************************************/

typedef struct _bq_section
{
	double b0, b1, b2;
	double a1, a2;
	double x_z1, x_z2;
	double y_z1, y_z2;
}bqs, *BQS;

typedef struct _noisepower
{
	double rate;
	int size;
	double* in;
	double* buff;
	double alpha_attack;
	double alpha_release;
	double pwr;
	bqs bqsec[2];
}nopwr, *NOPWR;

static void calc_bqs(double fs, double f_low, double f_high, bqs bqsec[])
{
	double w_low = 2.0 * fs * tan(PI * f_low / fs);
	double w_high = 2.0 * fs * tan(PI * f_high / fs);
	double BW = w_high - w_low;
	double W0 = sqrt(w_low * w_high);
	double pole_re[2] = { -sin(PI / 4.0), -sin(3.0 * PI / 4.0) };
	double pole_im[2] = {  cos(PI / 4.0),  cos(3.0 * PI / 4.0) };
	double W0sq = W0 * W0;
	double T = 1.0 / fs;
	for (int k = 0; k < 2; k++) 
	{
		double a1_a = -BW * pole_re[k];
		double a2_a = W0sq + BW * BW * (pole_im[k] * pole_im[k]
			- pole_re[k] * pole_re[k]);
		double K = 2.0 / T;
		double Ksq = K * K;
		double d0 = Ksq + a1_a * K + a2_a;
		double d1 = -2.0 * Ksq + 2.0 * a2_a;
		double d2 = Ksq - a1_a * K + a2_a;
		double n0 = BW * K;
		double n1 = 0.0;
		double n2 = -BW * K;
		bqsec[k].b0 = n0 / d0;
		bqsec[k].b1 = n1 / d0;
		bqsec[k].b2 = n2 / d0;
		bqsec[k].a1 = d1 / d0;   /* stored as positive; subtract in   */
		bqsec[k].a2 = d2 / d0;   /* the difference equation           */
	}
	for (int k = 0; k < 2; k++)
	{
		bqsec[k].x_z1 = 0.0;
		bqsec[k].x_z2 = 0.0;
		bqsec[k].y_z1 = 0.0;
		bqsec[k].y_z2 = 0.0;
	}

	/*printf("4th-order Butterworth Bandpass Filter Coefficients\n");
	printf("  fs       = %.1f Hz\n", fs);
	printf("  f_low    = %.1f Hz\n", f_low);
	printf("  f_high   = %.1f Hz\n", f_high);
	printf("\n");

	for (int k = 0; k < 2; k++) 
	{
		printf("Section %d:\n", k);
		printf("  b0 = % .10f\n", bqsec[k].b0);
		printf("  b1 = % .10f\n", bqsec[k].b1);
		printf("  b2 = % .10f\n", bqsec[k].b2);
		printf("  a1 = % .10f\n", bqsec[k].a1);
		printf("  a2 = % .10f\n", bqsec[k].a2);
		printf("\n");
	}*/

}

static void init_nopwr(double rate, int size, double* in, nopwr* npwr)
{
	npwr->rate = rate;
	npwr->size = size;
	npwr->in = in;
	npwr->buff = (double*)malloc0(npwr->size * sizeof(double));
	calc_bqs(npwr->rate, 58000.0, 75000.0, npwr->bqsec);
	npwr->alpha_attack  = exp(-1.0 / (rate * 0.050));	//  50 ms
	npwr->alpha_release = exp(-1.0 / (rate * 0.500));	// 500 ms
	npwr->pwr = 0.0;
}

static void bqsecRun(BQS a, int size, double* buff)
{
	double x = 0.0, y = 0.0;
	for (int i = 0; i < size; i++)
	{
		x = buff[i];
		y =   a->b0 * x
			+ a->b1 * a->x_z1
			+ a->b2 * a->x_z2
			- a->a1 * a->y_z1
			- a->a2 * a->y_z2;
		a->x_z2 = a->x_z1;
		a->x_z1 = x;
		a->y_z2 = a->y_z1;
		a->y_z1 = y;
		buff[i] = y;
	}
}

static double nopwrRun(NOPWR a)
{
	memcpy(a->buff, a->in, a->size * sizeof(double));
	for (int i = 0; i < 2; i++)
	{
		bqsecRun(&(a->bqsec[i]), a->size, a->buff);
	}
	for (int i = 0; i < a->size; i++)
	{
		double new_sample = a->buff[i] * a->buff[i];
		double alpha = (new_sample > a->pwr) ? a->alpha_attack : a->alpha_release;
		a->pwr = alpha * a->pwr + (1.0 - alpha) * new_sample;
	}
	return a->pwr;
}

typedef struct _wbsql
{
	double* in;
	double rate;
	int size;
	nopwr npwr;
	double mag19;			
	double pwrNoise;
	double pilot_true_thresh;
	double pilot_good_thresh;
	double noise_high_thresh;
	double noise_low_thresh;
	double slew;
	double gain_min;
	double gain;
	int sqstate;
} wbsql, *WBSQL;

static WBSQL create_wbsquelch(double* in, double rate, int size)
{
	WBSQL s = (WBSQL)malloc0(sizeof(wbsql));
	s->in = in;
	s->rate = rate;
	s->size = size;
	init_nopwr(s->rate, s->size, s->in, &s->npwr);
	s->mag19 = 0.0;
	s->pwrNoise = 0.0;
	s->pilot_true_thresh = 0.03;					// Tune this!
	s->pilot_good_thresh = 0.06;					// Tune this!
	s->noise_high_thresh = 0.50;					// Tune this!
	s->noise_low_thresh  = 0.15;					// Tune this!
	s->slew = (double)s->size / (0.05 * s->rate);	// 50ms
	s->gain_min = 0.050;							// Tune this!
	s->gain = s->gain_min;
	s->sqstate = 0;
	return s;
}

static void xwbsquelch(WBSQL s, double mag19, double* gain)
{
	s->mag19 = mag19;
	s->pwrNoise = nopwrRun(&(s->npwr));
	int noise_high = (s->pwrNoise > s->noise_high_thresh);
	int noise_low  = (s->pwrNoise < s->noise_low_thresh);
	int pilot_present = (s->mag19 > s->pilot_true_thresh);
	int pilot_good    = (s->mag19 > s->pilot_good_thresh);
	double gain_target = 0.0;
	switch (s->sqstate)
	{
	case 0:
		if (noise_low && (pilot_good || !pilot_present))
		{
			s->sqstate = 1;
		}
		break;
	case 1:
		if (noise_high || (pilot_present && !pilot_good))
		{
			s->sqstate = 0;
		}
		break;
	default:
		break;
	}
	if (s->sqstate) gain_target = 1.0;
	else            gain_target = s->gain_min;
	if (s->gain < gain_target)
		s->gain = fmin(s->gain + s->slew, 1.0);
	else
		s->gain = fmax(s->gain - s->slew, s->gain_min);
	*gain = s->gain;
}

static void destroy_wbsquelch(WBSQL s)
{
	_aligned_free(s->npwr.buff);
	_aligned_free(s);
}

/************************************************************************************************
*																								*
*							                DELAY           									*
*																								*
************************************************************************************************/

typedef struct _sdelay
{
	uint32_t size;
	uint32_t inpos;
	uint32_t outpos;
	uint32_t samp_delay;
	double* in;
	double* out;
	double* buff;
	uint32_t bsize;
}sdelay, * SDELAY;

static void calc_delay(SDELAY a)
{
	a->outpos = 0;
	a->inpos = a->outpos + a->samp_delay;
	a->buff = (double*)malloc0(a->size * sizeof(complex));
}

// for 'double', not 'complex' data
static SDELAY create_sdelay(uint32_t dl_size, uint32_t s_delay, uint32_t buff_size, double* in, double* out)
{
	SDELAY a = (SDELAY)malloc0(sizeof(sdelay));
	a->samp_delay = s_delay;
	a->size = dl_size;
	a->bsize = buff_size;
	a->in = in;
	a->out = out;
	calc_delay(a);
	return a;
}

static void decalc_delay(SDELAY a)
{
	_aligned_free(a->buff);
}

static void destroy_sdelay(SDELAY a)
{
	decalc_delay(a);
	_aligned_free(a);
}

static void xsdelay(SDELAY a)
{
	for (uint32_t i = 0; i < a->bsize; i++)
	{
		a->buff[2 * a->inpos + 0] = a->in[2 * i + 0];
		a->buff[2 * a->inpos + 1] = a->in[2 * i + 1];
		a->out[2 * i + 0] = a->buff[2 * a->outpos + 0];
		a->out[2 * i + 1] = a->buff[2 * a->outpos + 1];
		if (++(a->inpos) >= a->size) a->inpos = 0;
		if (++(a->outpos) >= a->size) a->outpos = 0;
	}
}

static void flush_sdelay(SDELAY a)
{
	memset(a->buff, 0, a->size * sizeof(complex));
	a->outpos = 0;
	a->inpos = a->outpos + a->samp_delay;
}


/********************************************************************************************************
*																										*
*                                     STRUCTURE & BASIC DEMODULATION									*
*																										*
********************************************************************************************************/

static void discriminator (WBFM a)
{
	double Inew = 0.0;
	double Qnew = 0.0;
	double Iold = 0.0;
	double Qold = 0.0;
	double phase_diff = 0.0;
	double out_val = 0.0;
	for (int i = 0; i < a->size; i++)
	{
		Inew = a->in[2 * i + 0];
		Qnew = a->in[2 * i + 1];
		Iold = a->discSave_I;
		Qold = a->discSave_Q;
		phase_diff = atan2(Iold * Qnew - Inew * Qold, Inew * Iold + Qnew * Qold);
		out_val = a->disc_gain_comp * phase_diff;
		a->discSave_I = Inew;
		a->discSave_Q = Qnew;
		a->disc_out[2 * i + 0] = 2.0 * out_val;
		a->disc_out[2 * i + 1] = 0.0;
	}
}

static void dc_block(WBFM a)
{
	double x   = 0.0;
	double xm1 = 0.0;
	double y   = 0.0;
	double ym1 = 0.0;
	double R   = 0.9995;
	for (int i = 0; i < a->size; i++)
	{
		x = a->disc_out[2 * i + 0];
		xm1 = a->dcbSave_x;
		ym1 = a->dcbSave_y;
		y = x - xm1 + R * ym1;
		a->dcbSave_x = x;
		a->dcbSave_y = y;
		a->disc_out[2 * i + 0] = y;
	}
}

static void dsb_demod (int size, double* inPilot, double* inDSB, double* outLmR)
{
	double square[2] = { 0.0 };
	double temp = 0;
	for (int i = 0; i < 2 * size; i += 2)
	{
		square[0] = inPilot[i + 0] * inPilot[i + 0] - inPilot[i + 1] * inPilot[i + 1];
		square[1] = 2.0 * inPilot[i + 0] * inPilot[i + 1];
		temp = square[0];
		square[0] = +square[1];
		square[1] = +temp;
		outLmR[i + 0] = square[0] * inDSB[i + 0] - square[1] * inDSB[i + 1];
		outLmR[i + 1] = square[0] * inDSB[i + 1] + square[1] * inDSB[i + 0];
	}
}

static void mx(int size, int stereo, double* LpR_in, double* LmR_in, double* L, double* R, double sqgain)
{
	if(stereo)
		for (int i = 0; i < size; i++)
		{
			L[i] = sqgain * (LpR_in[2 * i + 0] + LmR_in[2 * i + 0]);
			R[i] = sqgain * (LpR_in[2 * i + 0] - LmR_in[2 * i + 0]);
		}
	else
		for (int i = 0; i < size; i++)
		{
			L[i] = sqgain * LpR_in[2 * i + 0];
			R[i] = sqgain * LpR_in[2 * i + 0];
		}
}

static void combine (int size, double* L, double* R, double* out)
{
	for (int i = 0; i < size; i++)
	{
		out[2 * i + 0] = L[i];
		out[2 * i + 1] = R[i];
	}
}

static void calc_wbfm (WBFM a)
{
	double* impulse = NULL;

	// internal buffers
	a->disc_out  = (double*)malloc0(    a->size * sizeof(complex));
	a->fil19_out = (double*)malloc0(2 * a->size * sizeof(complex));
	a->fil53_out = (double*)malloc0(2 * a->size * sizeof(complex));
	a->LmR       = (double*)malloc0(    a->size * sizeof(complex));
	a->LpR       = (double*)malloc0(2 * a->size * sizeof(complex));
	a->L         = (double*)malloc0(    a->size * sizeof(double));
	a->R         = (double*)malloc0(    a->size * sizeof(double));

	// discriminator & dcb
	a->discSave_I = 0.0;
	a->discSave_Q = 0.0;
	a->dcbSave_x  = 0.0;
	a->dcbSave_y  = 0.0;
	a->disc_gain_comp = a->rate / (TWOPI * 75000.0);

	// filter:  0-15 kHz
	a->flow_fil0_15 = 0.0;
	a->fhigh_fil0_15 = 15200.0;
	a->nc_fil0_15 = (4096 > a->size) ? 4096 : a->size;
	impulse = fir_bandpass(a->nc_fil0_15, a->flow_fil0_15, a->fhigh_fil0_15, a->rate, 0, 1, 1.0 / (2.0 * a->size));
	a->pfil0_15 = create_fircore(a->size, a->disc_out, a->LpR, a->nc_fil0_15, 0, 8, impulse);
	_aligned_free(impulse);

	// filter:  19 kHz
	a->flow_fil19 = 18975.0;
	a->fhigh_fil19 = 19025.0;
	a->nc_fil19 = (8192 > a->size) ? 8192 : a->size;
	a->magComp_fil19 = 1.495;
	impulse = fir_bandpass(a->nc_fil19, a->flow_fil19, a->fhigh_fil19, a->rate, 0, 1, a->magComp_fil19 / (2.0 * a->size));
	a->pfil19 = create_fircore(a->size, a->disc_out, a->fil19_out, a->nc_fil19, 0, 8, impulse);
	_aligned_free(impulse);

	// filter:  23-53 kHz
	a->flow_fil23_53 = 22800.0;
	a->fhigh_fil23_53 = 53200.0;
	a->nc_fil23_53 = (4096 > a->size) ? 4096 : a->size;
	impulse = fir_bandpass(a->nc_fil23_53, a->flow_fil23_53, a->fhigh_fil23_53, a->rate, 0, 1, 1.0 / (2.0 * a->size));
	a->pfil23_53 = create_fircore(a->size, a->disc_out, a->fil53_out, a->nc_fil23_53, 0, 8, impulse);
	_aligned_free(impulse);

	// de-emphasis filters
	a->dmphL = create_dmph(a->dmph, a->rate, a->dmph_type, a->size, a->L, a->L);
	a->dmphR = create_dmph(a->dmph, a->rate, a->dmph_type, a->size, a->R, a->R);

	// Pilot AGC
	a->tau_attack = 0.001;
	a->n_tau = 4.0;
	a->targ_mult = 0.9999;
	a->out_targ = 1.0 / (a->targ_mult * (1.0 - exp(-a->n_tau)));
	a->agc_delay = (int)ceil(a->rate * a->n_tau * a->tau_attack);
	a->pAGC_Pilot = create_wcpagc(
		1,											// run - always ON
		5,											// mode
		1,											// 0 for max(I,Q), 1 for envelope
		a->fil19_out,								// input buff pointer
		a->fil19_out,								// output buff pointer
		a->size,									// io_buffsize
		(int)a->rate,								// sample rate
		a->tau_attack,								// tau_attack
		2.000,										// tau_decay
		(int)a->n_tau,								// n_tau
		1000.0,										// max_gain (sets threshold, initial value)
		1.0,										// var_gain / slope
		1.0,										// fixed_gain
		1.0,										// max_input
		a->out_targ,								// out_targ
		0.250,										// tau_fast_backaverage
		0.004,										// tau_fast_decay
		4.0,										// pop_ratio
		0,											// hang_enable
		0.500,										// tau_hang_backmult
		0.500,										// hangtime
		2.000,										// hang_thresh
		0.100);										// tau_hang_decay

	// Indicator
	a->pIndy = create_indy(a->size, a->rate, a->fil19_out);

	// Squelch
	a->psql = create_wbsquelch(a->disc_out, a->rate, a->size);

	// Matching Delays
	int delsamps15 = a->agc_delay + (a->nc_fil19 - a->nc_fil0_15) / 2;
	int delsize15 = delsamps15 + a->size;
	int delsamps53 = a->agc_delay + (a->nc_fil19 - a->nc_fil23_53) / 2;
	int delsize53 = delsamps53 + a->size;
	a->del15 = create_sdelay(delsize15, delsamps15, a->size, a->LpR, a->LpR);
	a->del53 = create_sdelay(delsize53, delsamps53, a->size, a->fil53_out, a->fil53_out);
}

static void decalc_wbfm (WBFM a)
{
	destroy_sdelay(a->del53);
	destroy_sdelay(a->del15);
	destroy_wbsquelch(a->psql);
	destroy_indy(a->pIndy);
	destroy_wcpagc(a->pAGC_Pilot);
	destroy_dmph(a->dmphL);
	destroy_dmph(a->dmphR);
	destroy_fircore(a->pfil23_53);
	destroy_fircore(a->pfil19);
	destroy_fircore(a->pfil0_15);
	_aligned_free(a->R);
	_aligned_free(a->L);
	_aligned_free(a->LpR);
	_aligned_free(a->LmR);
	_aligned_free(a->fil53_out);
	_aligned_free(a->fil19_out);
	_aligned_free(a->disc_out);
}

WBFM create_wbfm (int run, int size, double* in, double* out, double rate)
{
	WBFM a = (WBFM)malloc0(sizeof(wbfm));
	// the basics
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = rate;
	a->stereo = 0;
	a->sqgain = 0.0;
	a->dmph = 1;
	a->dmph_type = 0;
	calc_wbfm(a);
	return a;
}

void destroy_wbfm(WBFM a)
{
	decalc_wbfm(a);
	_aligned_free(a);
}

void flush_wbfm(WBFM a)
{
	memset(a->disc_out,  0,     a->size * sizeof(complex));
	memset(a->fil19_out, 0, 2 * a->size * sizeof(complex));
	memset(a->fil53_out, 0, 2 * a->size * sizeof(complex));
	memset(a->LmR,       0,     a->size * sizeof(complex));
	memset(a->LpR,       0, 2 * a->size * sizeof(complex));
	memset(a->L,         0,     a->size * sizeof(double));
	memset(a->R,         0,     a->size * sizeof(double));
	flush_dmph(a->dmphL);
	flush_dmph(a->dmphR);
	flush_sdelay(a->del15);
	flush_sdelay(a->del53);
	a->discSave_I = 0.0;
	a->discSave_Q = 0.0;
	a->dcbSave_x  = 0.0;
	a->dcbSave_y  = 0.0;
	flush_fircore(a->pfil0_15);
	flush_fircore(a->pfil19);
	flush_fircore(a->pfil23_53);
	flush_wcpagc(a->pAGC_Pilot);
	flush_indy(a->pIndy);
}

void xwbfm(WBFM a)
{
	if (a->run)
	{
		discriminator(a);
		dc_block(a);
		xfircore(a->pfil0_15);
		xfircore(a->pfil19);
		xfircore(a->pfil23_53);
		xindy(a->pIndy, &a->stereo, &a->mag19);					// EXECUTE BEFORE AGC ADJUSTS AMPLITUDE!
		xwbsquelch(a->psql, a->mag19, &a->sqgain);
		xwcpagc(a->pAGC_Pilot);
		xsdelay(a->del15);
		xsdelay(a->del53);
		dsb_demod(a->size, a->fil19_out, a->fil53_out, a->LmR);
		mx(a->size, a->stereo, a->LpR, a->LmR, a->L, a->R, a->sqgain);
		xdmph(a->dmphL);
		xdmph(a->dmphR);
		combine(a->size, a->L, a->R, a->out);
	}
	else if (a->in != a->out)
		memcpy(a->out, a->in, a->size * sizeof(complex));
}

void setBuffers_wbfm(WBFM a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_wbfm(WBFM a, int rate)
{
	decalc_wbfm(a);
	a->rate = rate;
	calc_wbfm(a);
}

void setSize_wbfm(WBFM a, int size)
{
	decalc_wbfm(a);
	a->size = size;
	calc_wbfm(a);
}


/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/


PORT
void SetRXAWBFMdmph(int channel, int dmph_run, int dmph_continent)
{
	WBFM a = rxa[channel].wbfm.p;
	if ((a->dmph != dmph_run) || (a->dmph_type != dmph_continent))
	{
		EnterCriticalSection(&ch[channel].csDSP);
		a->dmph = dmph_run;
		a->dmph_type = dmph_continent;
		LeaveCriticalSection(&ch[channel].csDSP);
	}
}

PORT
int GetRXAWBFMStereoIndicator(int channel)
{
	WBFM a = rxa[channel].wbfm.p;
	int stereo = 0;
	EnterCriticalSection(&ch[channel].csDSP);
	stereo = a->stereo;
	LeaveCriticalSection(&ch[channel].csDSP);
	return stereo;
}