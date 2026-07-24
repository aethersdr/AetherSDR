/*  cfir.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2014, 2016 Warren Pratt, NR0V

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

#ifndef _cfir_h
#define _cfir_h
#include "firmin.h"
typedef struct _cfir
{
	int run;
	int size;
	int nc;
	int mp;
	double* in;
	double* out;
	int runrate;
	int cicrate; 
	int DD; 
	int R; 
	int Pairs; 
	double cutoff;
	double scale;
	int xtype;
	double xbw;
	int wintype;
	FIRCORE p;
} cfir, *CFIR;

extern CFIR create_cfir (int run, int size, int nc, int mp, double* in, double* out, int runrate, int cicrate, 
	int DD, int R, int Pairs, double cutoff, int xtype, double xbw, int wintype);

extern void destroy_cfir (CFIR a);

extern void flush_cfir (CFIR a);

extern void xcfir (CFIR a);

extern void setBuffers_cfir (CFIR a, double* in, double* out);

extern void setSamplerate_cfir (CFIR a, int rate);

extern void setSize_cfir (CFIR a, int size);

extern void setOutRate_cfir (CFIR a, int rate);

extern double* cfir_impulse (int N, int DD, int R, int Pairs, double runrate, double cicrate, 
	double cutoff, int xtype, double xbw, int rtype, double scale, int wintype);

extern __declspec (dllexport) void SetTXACFIRRun(int channel, int run);

extern __declspec (dllexport) void SetTXACFIRNC(int channel, int nc);

#endif