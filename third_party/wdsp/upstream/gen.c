/*  gen.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2025 Warren Pratt, NR0V

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

void calc_tone (GEN a)
{
	a->tone.phs = 0.0;
	a->tone.delta = TWOPI * a->tone.freq / a->rate;
	a->tone.cosdelta = cos (a->tone.delta);
	a->tone.sindelta = sin (a->tone.delta);
}

void calc_tt (GEN a)
{
	a->tt.phs1 = 0.0;
	a->tt.phs2 = 0.0;
	a->tt.delta1 = TWOPI * a->tt.f1 / a->rate;
	a->tt.delta2 = TWOPI * a->tt.f2 / a->rate;
	a->tt.cosdelta1 = cos (a->tt.delta1);
	a->tt.cosdelta2 = cos (a->tt.delta2);
	a->tt.sindelta1 = sin (a->tt.delta1);
	a->tt.sindelta2 = sin (a->tt.delta2);
}

void calc_sweep (GEN a)
{
	a->sweep.phs = 0.0;
	a->sweep.dphs = TWOPI * a->sweep.f1 / a->rate;
	a->sweep.d2phs = TWOPI * a->sweep.sweeprate / (a->rate * a->rate);
	a->sweep.dphsmax = TWOPI * a->sweep.f2 / a->rate;
}

void calc_sawtooth (GEN a)
{
	a->saw.period = 1.0 / a->saw.f;
	a->saw.delta = 1.0 / a->rate;
	a->saw.t = 0.0;
}

void calc_triangle (GEN a)
{
	a->tri.period = 1.0 / a->tri.f;
	a->tri.half = 0.5 * a->tri.period;
	a->tri.delta = 1.0 / a->rate;
	a->tri.t = 0.0;
	a->tri.t1 = 0.0;
}

void calc_pulse (GEN a)
{
	int i;
	double delta, theta;
	a->pulse.pperiod = 1.0 / a->pulse.pf;
	a->pulse.tphs = 0.0;
	a->pulse.tdelta = TWOPI * a->pulse.tf / a->rate;
	a->pulse.tcosdelta = cos (a->pulse.tdelta);
	a->pulse.tsindelta = sin (a->pulse.tdelta);
	a->pulse.pntrans = (int)(a->pulse.ptranstime * a->rate);
	a->pulse.pnon = (int)(a->pulse.pdutycycle * a->pulse.pperiod * a->rate);
	a->pulse.pnoff = (int)(a->pulse.pperiod * a->rate) - a->pulse.pnon - 2 * a->pulse.pntrans;
	if (a->pulse.pnoff < 0) a->pulse.pnoff = 0;
	a->pulse.pcount = a->pulse.pnoff;
	a->pulse.state = 0;
	a->pulse.ctrans = (double *) malloc0 ((a->pulse.pntrans + 1) * sizeof (double));
	delta = PI / (double)a->pulse.pntrans;
	theta = 0.0;
	for (i = 0; i <= a->pulse.pntrans; i++)
	{
		a->pulse.ctrans[i] = 0.5 * (1.0 - cos (theta));
		theta += delta;
	}
}

void calc_ttpulse(GEN a)
{
	int i;
	double delta, theta;
	a->ttpulse.pperiod = 1.0 / a->ttpulse.pf;
	a->ttpulse.tphs1 = 0.0;
	a->ttpulse.tphs2 = 0.0;
	a->ttpulse.tdelta1 = TWOPI * a->ttpulse.tf1 / a->rate;
	a->ttpulse.tdelta2 = TWOPI * a->ttpulse.tf2 / a->rate;
	a->ttpulse.tcosdelta1 = cos(a->ttpulse.tdelta1);
	a->ttpulse.tcosdelta2 = cos(a->ttpulse.tdelta2);
	a->ttpulse.tsindelta1 = sin(a->ttpulse.tdelta1);
	a->ttpulse.tsindelta2 = sin(a->ttpulse.tdelta2);
	a->ttpulse.pntrans = (int)(a->ttpulse.ptranstime * a->rate);
	a->ttpulse.pnon = (int)(a->ttpulse.pdutycycle * a->ttpulse.pperiod * a->rate);
	a->ttpulse.pnoff = (int)(a->ttpulse.pperiod * a->rate) - a->ttpulse.pnon - 2 * a->ttpulse.pntrans;
	if (a->ttpulse.pnoff < 0) a->ttpulse.pnoff = 0;
	a->ttpulse.pcount = a->ttpulse.pnoff;
	a->ttpulse.state = 0;
	a->ttpulse.ctrans = (double*)malloc0((a->ttpulse.pntrans + 1) * sizeof(double));
	delta = PI / (double)a->ttpulse.pntrans;
	theta = 0.0;
	for (i = 0; i <= a->ttpulse.pntrans; i++)
	{
		a->ttpulse.ctrans[i] = 0.5 * (1.0 - cos(theta));
		theta += delta;
	}
}

void calc_gen (GEN a)
{
	calc_tone (a);
	calc_tt (a);
	calc_sweep (a);
	calc_sawtooth (a);
	calc_triangle (a);
	calc_pulse (a);
	calc_ttpulse (a);
}

void decalc_gen (GEN a)
{
	_aligned_free (a->ttpulse.ctrans);
	_aligned_free (a->pulse.ctrans);
}

GEN create_gen (int run, int size, double* in, double* out, int rate, int mode)
{
	GEN a = (GEN) malloc0 (sizeof (gen));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = (double)rate;
	a->mode = mode;
	// tone
	a->tone.mag = 1.0;
	a->tone.freq = 1000.0;
	// two-tone
	a->tt.mag1 = 0.5;
	a->tt.mag2 = 0.5;
	a->tt.f1 = +  900.0;
	a->tt.f2 = + 1700.0;
	// noise
	srand ((unsigned int)time (0));
	a->noise.mag = 1.0;
	// sweep
	a->sweep.mag = 1.0;
	a->sweep.f1 = -20000.0;
	a->sweep.f2 = +20000.0;
	a->sweep.sweeprate = +4000.0;
	// sawtooth
	a->saw.mag = 1.0;
	a->saw.f = 500.0;
	// triangle
	a->tri.mag = 1.0;
	a->tri.f = 500.0;
	// pulse
	a->pulse.mag = 1.0;
	a->pulse.pf = 2.0;
	a->pulse.pdutycycle = 0.25;
	a->pulse.ptranstime = 0.005;
	a->pulse.tf = 600.0;
	a->pulse.IQout = 0;
	// two-tone pulse
	a->ttpulse.mag1 = 0.5;
	a->ttpulse.mag2 = 0.5;
	a->ttpulse.pf = 2.0;
	a->ttpulse.pdutycycle = 0.25;
	a->ttpulse.ptranstime = 0.005;
	a->ttpulse.tf1 = 900.0;
	a->ttpulse.tf2 = 1700.0;
	a->ttpulse.IQout = 0;
	calc_gen (a);
	return a;
}

void destroy_gen (GEN a)
{
	decalc_gen (a);
	_aligned_free (a);
}

void flush_gen (GEN a)
{
	a->pulse.state = 0;
	a->ttpulse.state = 0;
}

enum pstate 
{
	OFF,
	UP,
	ON,
	DOWN
};

void xgen (GEN a)
{
	if (a->run)
	{
		switch (a->mode)
		{
		case 0:	// tone
			{
				int i;
				double t1, t2;
				double cosphase = cos (a->tone.phs);
				double sinphase = sin (a->tone.phs);
				for (i = 0; i < a->size; i++)
				{
					a->out[2 * i + 0] = + a->tone.mag * cosphase;
					a->out[2 * i + 1] = - a->tone.mag * sinphase;
					t1 = cosphase;
					t2 = sinphase;
					cosphase = t1 * a->tone.cosdelta - t2 * a->tone.sindelta;
					sinphase = t1 * a->tone.sindelta + t2 * a->tone.cosdelta;
					a->tone.phs += a->tone.delta;
					if (a->tone.phs >= TWOPI) a->tone.phs -= TWOPI;
					if (a->tone.phs <   0.0 ) a->tone.phs += TWOPI;
				}
				break;
			}
		case 1:	// two-tone
			{
				int i;
				double tcos, tsin;
				double cosphs1 = cos (a->tt.phs1);
				double sinphs1 = sin (a->tt.phs1);
				double cosphs2 = cos (a->tt.phs2);
				double sinphs2 = sin (a->tt.phs2);
				for (i = 0; i < a->size; i++)
				{
					a->out[2 * i + 0] = + a->tt.mag1 * cosphs1 + a->tt.mag2 * cosphs2;
					a->out[2 * i + 1] = - a->tt.mag1 * sinphs1 - a->tt.mag2 * sinphs2;
					tcos = cosphs1;
					tsin = sinphs1;
					cosphs1 = tcos * a->tt.cosdelta1 - tsin * a->tt.sindelta1;
					sinphs1 = tcos * a->tt.sindelta1 + tsin * a->tt.cosdelta1;
					a->tt.phs1 += a->tt.delta1;
					if (a->tt.phs1 >= TWOPI) a->tt.phs1 -= TWOPI;
					if (a->tt.phs1 <   0.0 ) a->tt.phs1 += TWOPI;
					tcos = cosphs2;
					tsin = sinphs2;
					cosphs2 = tcos * a->tt.cosdelta2 - tsin * a->tt.sindelta2;
					sinphs2 = tcos * a->tt.sindelta2 + tsin * a->tt.cosdelta2;
					a->tt.phs2 += a->tt.delta2;
					if (a->tt.phs2 >= TWOPI) a->tt.phs2 -= TWOPI;
					if (a->tt.phs2 <   0.0 ) a->tt.phs2 += TWOPI;
				}
				break;
			}
		case 2: // noise
			{
				int i;
				double r1, r2, c, rad;
				for (i = 0; i < a->size; i++)
				{
					do
					{
						r1 = 2.0 * (double)rand() / (double)RAND_MAX - 1.0;
						r2 = 2.0 * (double)rand() / (double)RAND_MAX - 1.0;
						c = r1 * r1 + r2 * r2;
					} while (c >= 1.0);
					rad = sqrt (-2.0 * log (c) / c);
					a->out[2 * i + 0] = a->noise.mag * rad * r1;
					a->out[2 * i + 1] = a->noise.mag * rad * r2;
				}
				break;
			}
		case 3:  // sweep
			{
				int i;
				for (i = 0; i < a->size; i++)
				{
					a->out[2 * i + 0] = + a->sweep.mag * cos(a->sweep.phs);
					a->out[2 * i + 1] = - a->sweep.mag * sin(a->sweep.phs);
					a->sweep.phs += a->sweep.dphs;
					a->sweep.dphs += a->sweep.d2phs;
					if (a->sweep.phs >= TWOPI) a->sweep.phs -= TWOPI;
					if (a->sweep.phs <   0.0 ) a->sweep.phs += TWOPI;
					if (a->sweep.dphs > a->sweep.dphsmax)
						a->sweep.dphs = TWOPI * a->sweep.f1 / a->rate;
				}
				break;
			}
		case 4:  // sawtooth (audio only)
			{
				int i;
				for (i = 0; i < a->size; i++)
				{
					if (a->saw.t > a->saw.period) a->saw.t -= a->saw.period;
					a->out[2 * i + 0] = a->saw.mag * (a->saw.t * a->saw.f - 1.0);
					a->out[2 * i + 1] = 0.0;
					a->saw.t += a->saw.delta;
				}
			}
			break;
		case 5:  // triangle (audio only)
			{
				int i;
				for (i = 0; i < a->size; i++)
				{
					if (a->tri.t > a->tri.period) a->tri.t1 = a->tri.t -= a->tri.period;
					if (a->tri.t > a->tri.half)	a->tri.t1 -= a->tri.delta;
					else						a->tri.t1 += a->tri.delta;
					a->out[2 * i + 0] = a->tri.mag * (4.0 * a->tri.t1 * a->tri.f - 1.0);
					a->out[2 * i + 1] = 0.0;
					a->tri.t += a->tri.delta;
				}
			}
			break;
		case 6:  // pulse (audio or IQ output)
			{
				int i;
				double t1, t2;
				double cosphase = cos (a->pulse.tphs);
				double sinphase = sin (a->pulse.tphs);
				for (i = 0; i < a->size; i++)
				{
					if (a->pulse.pnoff != 0)
					{
						switch (a->pulse.state)
						{
						case OFF:
							a->out[2 * i + 0] = 0.0;
							a->out[2 * i + 1] = 0.0;
							if (--a->pulse.pcount == 0)
							{
								a->pulse.state = UP;
								a->pulse.pcount = a->pulse.pntrans;
							}
							break;
						case UP:

							if (a->pulse.IQout)
							{
								a->out[2 * i + 0] = +a->pulse.mag * cosphase * a->pulse.ctrans[a->pulse.pntrans - a->pulse.pcount];
								a->out[2 * i + 1] = -a->pulse.mag * sinphase * a->pulse.ctrans[a->pulse.pntrans - a->pulse.pcount];
							}
							else
							{
								a->out[2 * i + 0] = +a->pulse.mag * cosphase * a->pulse.ctrans[a->pulse.pntrans - a->pulse.pcount];
								a->out[2 * i + 1] = 0.0;
							}
							if (--a->pulse.pcount == 0)
							{
								a->pulse.state = ON;
								a->pulse.pcount = a->pulse.pnon;
							}
							break;
						case ON:

							if (a->pulse.IQout)
							{
								a->out[2 * i + 0] = +a->pulse.mag * cosphase;
								a->out[2 * i + 1] = -a->pulse.mag * sinphase;
							}
							else
							{
								a->out[2 * i + 0] = +a->pulse.mag * cosphase;
								a->out[2 * i + 1] = 0.0;
							}
							if (--a->pulse.pcount == 0)
							{
								a->pulse.state = DOWN;
								a->pulse.pcount = a->pulse.pntrans;
							}
							break;
						case DOWN:
							if (a->pulse.IQout)
							{
								a->out[2 * i + 0] = +a->pulse.mag * cosphase * a->pulse.ctrans[a->pulse.pcount];
								a->out[2 * i + 1] = -a->pulse.mag * sinphase * a->pulse.ctrans[a->pulse.pcount];
							}
							else
							{
								a->out[2 * i + 0] = +a->pulse.mag * cosphase * a->pulse.ctrans[a->pulse.pcount];
								a->out[2 * i + 1] = 0.0;
							}
							if (--a->pulse.pcount == 0)
							{
								a->pulse.state = OFF;
								a->pulse.pcount = a->pulse.pnoff;
							}
							break;
						}
					}
					else
					{
						a->out[2 * i + 0] = 0.0;
						a->out[2 * i + 1] = 0.0;
					}
					t1 = cosphase;
					t2 = sinphase;
					cosphase = t1 * a->pulse.tcosdelta - t2 * a->pulse.tsindelta;
					sinphase = t1 * a->pulse.tsindelta + t2 * a->pulse.tcosdelta;
					a->pulse.tphs += a->pulse.tdelta;
					if (a->pulse.tphs >= TWOPI) a->pulse.tphs -= TWOPI;
					if (a->pulse.tphs <   0.0 ) a->pulse.tphs += TWOPI;
				}
			}
			break;
		case 7:		// two-tone pulse (audio or IQ)
			{
				int i;
				double t1a, t1b, t2a, t2b;
				double cosphase1 = cos(a->ttpulse.tphs1);
				double cosphase2 = cos(a->ttpulse.tphs2);
				double sinphase1 = sin(a->ttpulse.tphs1);
				double sinphase2 = sin(a->ttpulse.tphs2);
				for (i = 0; i < a->size; i++)
				{
					if (a->ttpulse.pnoff != 0)
					{
						switch (a->ttpulse.state)
						{
						case OFF:
							a->out[2 * i + 0] = 0.0;
							a->out[2 * i + 1] = 0.0;
							if (--a->ttpulse.pcount == 0)
							{
								a->ttpulse.state = UP;
								a->ttpulse.pcount = a->ttpulse.pntrans;
							}
							break;
						case UP:
							if (a->ttpulse.IQout)
							{
								a->out[2 * i + 0] = +(a->ttpulse.mag1 * cosphase1 + a->ttpulse.mag2 * cosphase2) * a->ttpulse.ctrans[a->ttpulse.pntrans - a->ttpulse.pcount];
								a->out[2 * i + 1] = -(a->ttpulse.mag1 * sinphase1 + a->ttpulse.mag2 * sinphase2) * a->ttpulse.ctrans[a->ttpulse.pntrans - a->ttpulse.pcount];
							}
							else
							{
								a->out[2 * i + 0] = +(a->ttpulse.mag1 * cosphase1 + a->ttpulse.mag2 * cosphase2) * a->ttpulse.ctrans[a->ttpulse.pntrans - a->ttpulse.pcount];
								a->out[2 * i + 1] = 0.0;
							}
							if (--a->ttpulse.pcount == 0)
							{
								a->ttpulse.state = ON;
								a->ttpulse.pcount = a->ttpulse.pnon;
							}
							break;
						case ON:
							if (a->ttpulse.IQout)
							{
								a->out[2 * i + 0] = +(a->ttpulse.mag1 * cosphase1 + a->ttpulse.mag2 * cosphase2);
								a->out[2 * i + 1] = -(a->ttpulse.mag1 * sinphase1 + a->ttpulse.mag2 * sinphase2);
							}
							else
							{
								a->out[2 * i + 0] = +(a->ttpulse.mag1 * cosphase1 + a->ttpulse.mag2 * cosphase2);
								a->out[2 * i + 1] = 0.0;
							}
							if (--a->ttpulse.pcount == 0)
							{
								a->ttpulse.state = DOWN;
								a->ttpulse.pcount = a->ttpulse.pntrans;
							}
							break;
						case DOWN:
							if (a->ttpulse.IQout)
							{
								a->out[2 * i + 0] = +(a->ttpulse.mag1 * cosphase1 + a->ttpulse.mag2 * cosphase2) * a->ttpulse.ctrans[a->ttpulse.pcount];
								a->out[2 * i + 1] = -(a->ttpulse.mag1 * sinphase1 + a->ttpulse.mag2 * sinphase2) * a->ttpulse.ctrans[a->ttpulse.pcount];
							}
							else
							{
								a->out[2 * i + 0] = +(a->ttpulse.mag1 * cosphase1 + a->ttpulse.mag2 * cosphase2) * a->ttpulse.ctrans[a->ttpulse.pcount];
								a->out[2 * i + 1] = 0.0;
							}
							if (--a->ttpulse.pcount == 0)
							{
								a->ttpulse.state = OFF;
								a->ttpulse.pcount = a->ttpulse.pnoff;
							}
							break;
						}
					}
					else
					{
						a->out[2 * i + 0] = 0.0;
						a->out[2 * i + 1] = 0.0;
					}
					t1a = cosphase1;
					t1b = sinphase1;
					cosphase1 = t1a * a->ttpulse.tcosdelta1 - t1b * a->ttpulse.tsindelta1;
					sinphase1 = t1a * a->ttpulse.tsindelta1 + t1b * a->ttpulse.tcosdelta1;
					a->ttpulse.tphs1 += a->ttpulse.tdelta1;
					if (a->ttpulse.tphs1 >= TWOPI) a->ttpulse.tphs1 -= TWOPI;
					if (a->ttpulse.tphs1 <  0.0)   a->ttpulse.tphs1 += TWOPI;
					t2a = cosphase2;
					t2b = sinphase2;
					cosphase2 = t2a * a->ttpulse.tcosdelta2 - t2b * a->ttpulse.tsindelta2;
					sinphase2 = t2a * a->ttpulse.tsindelta2 + t2b * a->ttpulse.tcosdelta2;
					a->ttpulse.tphs2 += a->ttpulse.tdelta2;
					if (a->ttpulse.tphs2 >= TWOPI) a->ttpulse.tphs2 -= TWOPI;
					if (a->ttpulse.tphs2 <  0.0)   a->ttpulse.tphs2 += TWOPI;
				}
			}
			break;
		default:	// silence
			{
				memset (a->out, 0, a->size * sizeof (complex));
				break;
			}
		}
	}
	else if (a->in != a->out)
		memcpy (a->out, a->in, a->size * sizeof (complex));
}

void setBuffers_gen (GEN a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_gen (GEN a, int rate)
{
	decalc_gen (a);
	a->rate = rate;
	calc_gen (a);
}

void setSize_gen (GEN a, int size)
{
	a->size = size;
	flush_gen (a);
}


/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

// 'PreGen', gen0

PORT
void SetRXAPreGenRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].gen0.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPreGenMode (int channel, int mode)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].gen0.p->mode = mode;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPreGenToneMag (int channel, double mag)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].gen0.p->tone.mag = mag;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPreGenToneFreq (int channel, double freq)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].gen0.p->tone.freq = freq;
	calc_tone (rxa[channel].gen0.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPreGenNoiseMag (int channel, double mag)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].gen0.p->noise.mag = mag;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPreGenSweepMag (int channel, double mag)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].gen0.p->sweep.mag = mag;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPreGenSweepFreq (int channel, double freq1, double freq2)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].gen0.p->sweep.f1 = freq1;
	rxa[channel].gen0.p->sweep.f2 = freq2;
	calc_sweep (rxa[channel].gen0.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPreGenSweepRate (int channel, double rate)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].gen0.p->sweep.sweeprate = rate;
	calc_sweep (rxa[channel].gen0.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}


/********************************************************************************************************
*																										*
*											TXA Properties												*
*																										*
********************************************************************************************************/

// 'PreGen', gen0

PORT
void SetTXAPreGenRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenMode (int channel, int mode)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->mode = mode;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenToneMag (int channel, double mag)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->tone.mag = mag;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenToneFreq (int channel, double freq)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->tone.freq = freq;
	calc_tone (txa[channel].gen0.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenNoiseMag (int channel, double mag)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->noise.mag = mag;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenSweepMag (int channel, double mag)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->sweep.mag = mag;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenSweepFreq (int channel, double freq1, double freq2)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->sweep.f1 = freq1;
	txa[channel].gen0.p->sweep.f2 = freq2;
	calc_sweep (txa[channel].gen0.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenSweepRate (int channel, double rate)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->sweep.sweeprate = rate;
	calc_sweep (txa[channel].gen0.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenSawtoothMag (int channel, double mag)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->saw.mag = mag;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenSawtoothFreq (int channel, double freq)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->saw.f = freq;
	calc_sawtooth (txa[channel].gen0.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenTriangleMag (int channel, double mag)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->tri.mag = mag;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenTriangleFreq (int channel, double freq)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->tri.f = freq;
	calc_triangle (txa[channel].gen0.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenPulseMag (int channel, double mag)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->pulse.mag = mag;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenPulseFreq (int channel, double freq)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->pulse.pf = freq;
	calc_pulse (txa[channel].gen0.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenPulseDutyCycle (int channel, double dc)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->pulse.pdutycycle = dc;
	calc_pulse (txa[channel].gen0.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenPulseToneFreq (int channel, double freq)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->pulse.tf = freq;
	calc_pulse (txa[channel].gen0.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPreGenPulseTransition (int channel, double transtime)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen0.p->pulse.ptranstime = transtime;
	calc_pulse (txa[channel].gen0.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

// 'PostGen', gen1

PORT
void SetTXAPostGenRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen1.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPostGenMode (int channel, int mode)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen1.p->mode = mode;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPostGenToneMag (int channel, double mag)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen1.p->tone.mag = mag;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPostGenToneFreq (int channel, double freq)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen1.p->tone.freq = freq;
	calc_tone (txa[channel].gen1.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPostGenTTMag (int channel, double mag1, double mag2)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen1.p->tt.mag1 = mag1;
	txa[channel].gen1.p->tt.mag2 = mag2;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPostGenTTFreq (int channel, double freq1, double freq2)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen1.p->tt.f1 = freq1;
	txa[channel].gen1.p->tt.f2 = freq2;
	calc_tt (txa[channel].gen1.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPostGenSweepMag (int channel, double mag)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen1.p->sweep.mag = mag;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPostGenSweepFreq (int channel, double freq1, double freq2)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen1.p->sweep.f1 = freq1;
	txa[channel].gen1.p->sweep.f2 = freq2;
	calc_sweep (txa[channel].gen1.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPostGenSweepRate (int channel, double rate)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].gen1.p->sweep.sweeprate = rate;
	calc_sweep (txa[channel].gen1.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPostGenPulseMag(int channel, double mag)
{
	EnterCriticalSection(&ch[channel].csDSP);
	txa[channel].gen1.p->pulse.mag = mag;
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXAPostGenPulseFreq(int channel, double freq)
{
	EnterCriticalSection(&ch[channel].csDSP);
	txa[channel].gen1.p->pulse.pf = freq;
	calc_pulse(txa[channel].gen1.p);
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXAPostGenPulseDutyCycle(int channel, double dc)
{
	EnterCriticalSection(&ch[channel].csDSP);
	txa[channel].gen1.p->pulse.pdutycycle = dc;
	calc_pulse(txa[channel].gen1.p);
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXAPostGenPulseToneFreq(int channel, double freq)
{
	EnterCriticalSection(&ch[channel].csDSP);
	txa[channel].gen1.p->pulse.tf = freq;
	calc_pulse(txa[channel].gen1.p);
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXAPostGenPulseTransition(int channel, double transtime)
{
	EnterCriticalSection(&ch[channel].csDSP);
	txa[channel].gen1.p->pulse.ptranstime = transtime;
	calc_pulse(txa[channel].gen1.p);
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXAPostGenPulseIQout(int channel, int IQout)
{
	EnterCriticalSection(&ch[channel].csDSP);
	txa[channel].gen1.p->pulse.IQout = IQout;
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXAPostGenTTPulseMag(int channel, double mag1, double mag2)
{
	// defaults are 0.5/0.5
	// total must not exceed 1.0
	EnterCriticalSection(&ch[channel].csDSP);
	txa[channel].gen1.p->ttpulse.mag1 = mag1;
	txa[channel].gen1.p->ttpulse.mag2 = mag2;
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXAPostGenTTPulseFreq(int channel, double freq)
{
	EnterCriticalSection(&ch[channel].csDSP);
	txa[channel].gen1.p->ttpulse.pf = freq;
	calc_ttpulse(txa[channel].gen1.p);
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXAPostGenTTPulseDutyCycle(int channel, double dc)
{
	EnterCriticalSection(&ch[channel].csDSP);
	txa[channel].gen1.p->ttpulse.pdutycycle = dc;
	calc_ttpulse(txa[channel].gen1.p);
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXAPostGenTTPulseToneFreq(int channel, double freq1, double freq2)
{
	GEN a = txa[channel].gen1.p;
	EnterCriticalSection(&ch[channel].csDSP);
	a->ttpulse.tf1 = freq1;
	a->ttpulse.tf2 = freq2;
	calc_ttpulse(a);
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXAPostGenTTPulseTransition(int channel, double transtime)
{
	EnterCriticalSection(&ch[channel].csDSP);
	txa[channel].gen1.p->ttpulse.ptranstime = transtime;
	calc_ttpulse(txa[channel].gen1.p);
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXAPostGenTTPulseIQout(int channel, int IQout)
{
	EnterCriticalSection(&ch[channel].csDSP);
	txa[channel].gen1.p->ttpulse.IQout = IQout;
	LeaveCriticalSection(&ch[channel].csDSP);
}
