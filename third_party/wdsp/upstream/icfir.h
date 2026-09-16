/*  icfir.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2018, 2026 Warren Pratt, NR0V

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

#ifndef _icfir_h
#define _icfir_h

typedef struct _icfir *ICFIR;

extern ICFIR create_icfir (int run, int size, int nc, int mp, double* in, double* out, int runrate, int cicrate, 
	int DD, int R, int Pairs, double cutoff, int xtype, double xbw, int wintype);

extern void destroy_icfir (ICFIR a);

extern void flush_icfir (ICFIR a);

extern void xicfir (ICFIR a);

extern void setBuffers_icfir (ICFIR a, double* in, double* out);

extern void setSamplerate_icfir (ICFIR a, int rate);

extern void setSize_icfir (ICFIR a, int size);

extern void setOutRate_icfir (ICFIR a, int rate);

#endif