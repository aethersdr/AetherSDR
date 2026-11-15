/*  cfcomp.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2017, 2021, 2026 Warren Pratt, NR0V 

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

#ifndef _cfcomp_h
#define _cfcomp_h

#include "nurbs.h"

typedef struct _cfcomp
{
	int run;
	int position;
	int bsize;
	double* in;
	double* out;
	int fsize;
	int ovrlp;
	int incr;
	double* window;
	int iasize;
	double* inaccum;
	double* forfftin;
	double* forfftout;
	int msize;
	double* cmask;
	double* mask;
	int mask_ready;
	double* cfc_gain;
	double* revfftin;
	double* revfftout;
	double** save;
	int oasize;
	double* outaccum;
	double rate;
	int wintype;
	double pregain;
	double postgain;
	int nsamps;
	int iainidx;
	int iaoutidx;
	int init_oainidx;
	int oainidx;
	int oaoutidx;
	int saveidx;
	fftw_plan Rfor;
	fftw_plan Rrev;

	// G/g refer to compressor; E/e refer to equalizer
	int comp_method;
	int max_freqs;
	int nfreqsG;
	int nfreqsE;
	double* Fg;
	double* Fe;
	double* G;
	double* E;
	double* fpG;
	double* fpE;
	double* gp;
	double* ep;
	double* comp;
	double precomp;
	double precomplin;
	double* peq;
	int peq_run;
	double prepeq;
	double prepeqlin;
	double winfudge;
	double* saryG;
	double* saryE;

	double gain;
	double mtau;
	double mmult;
	// display stuff
	double dtau;
	double dmult;
	double* delta;
	double* delta_copy;
	double* cfc_gain_copy;

	// nurbs stuff
	int gdeg;
	int edeg;
	NURBS png;
	NURBS pne;

}cfcomp, *CFCOMP;

extern CFCOMP create_cfcomp (int run, int position, int peq_run, int size, double* in, double* out, int fsize, int ovrlp, 
	int rate, int wintype, int comp_method, int nfreqsG, int nfreqsE, double precomp, double prepeq, 
	double* Fg, double* G, double* Fe, double* E, double mtau, double dtau);

extern void destroy_cfcomp (CFCOMP a);

extern void flush_cfcomp (CFCOMP a);

extern void xcfcomp (CFCOMP a, int pos);

extern void setBuffers_cfcomp (CFCOMP a, double* in, double* out);

extern void setSamplerate_cfcomp (CFCOMP a, int rate);

extern void setSize_cfcomp (CFCOMP a, int size);

#endif