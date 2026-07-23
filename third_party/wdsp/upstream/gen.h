/*  gen.h

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

#ifndef _gen_h
#define _gen_h

typedef struct _gen
{
	int run;					// run
	int size;					// number of samples per buffer
	double* in;					// input buffer (retained in case I want to mix in a generated signal)
	double* out;				// output buffer
	double rate;				// sample rate
	int mode;					
	struct _tone
	{
		double mag;
		double freq;
		double phs;
		double delta;
		double cosdelta;
		double sindelta;
	} tone;
	struct _tt
	{
		double mag1;
		double mag2;
		double f1;
		double f2;
		double phs1;
		double phs2;
		double delta1;
		double delta2;
		double cosdelta1;
		double cosdelta2;
		double sindelta1;
		double sindelta2;
	} tt;
	struct _noise
	{
		double mag;
	} noise;
	struct _sweep
	{
		double mag;
		double f1;
		double f2;
		double sweeprate;
		double phs;
		double dphs;
		double d2phs;
		double dphsmax;
	} sweep;
	struct _saw
	{
		double mag;
		double f;
		double period;
		double delta;
		double t;
	} saw;
	struct _tri
	{
		double mag;
		double f;
		double period;
		double half;
		double delta;
		double t;
		double t1;
	} tri;
	struct _pulse
	{
		double mag;
		double pf;
		double pdutycycle;
		double ptranstime;
		double* ctrans;
		int pcount;
		int pnon;
		int pntrans;
		int pnoff;
		double pperiod;
		double tf;
		double tphs;
		double tdelta;
		double tcosdelta;
		double tsindelta;
		int state;
		int IQout;
	} pulse;
	struct _ttpulse
	{
		double mag1;
		double mag2;
		double pf;
		double pdutycycle;
		double ptranstime;
		double* ctrans;
		int pcount;
		int pnon;
		int pntrans;
		int pnoff;
		double pperiod;
		double tf1;
		double tf2;
		double tphs1;
		double tphs2;
		double tdelta1;
		double tdelta2;
		double tcosdelta1;
		double tcosdelta2;
		double tsindelta1;
		double tsindelta2;
		int state;
		int IQout;
	} ttpulse;
} gen, *GEN;

extern GEN create_gen (int run, int size, double* in, double* out, int rate, int mode);

extern void destroy_gen (GEN a);

extern void flush_gen (GEN a);

extern void xgen (GEN a);

extern void setBuffers_gen (GEN a, double* in, double* out);

extern void setSamplerate_gen (GEN a, int rate);

extern void setSize_gen (GEN a, int size);

// TXA Properties


#endif