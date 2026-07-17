/*  eer.h

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

#ifndef _eer_h
#define _eer_h

typedef struct _eer
{
	int run;
	int amiq;
	int size;
	double* in;
	double* out;
	double* outM;
	int rate;
	double mgain;
	double pgain;
	int rundelays;
	double mdelay;
	double pdelay;
	DELAY mdel;
	DELAY pdel;
	CRITICAL_SECTION cs_update;
	double *legacy;																										////////////  legacy interface - remove
	double *legacyM;																									////////////  legacy interface - remove
} eer, *EER;

__declspec (dllexport) EER create_eer (int run, int size, double* in, double* out, double* outM, int rate, double mgain, double pgain, int rundelays, double mdelay, double pdelay, int amiq);

__declspec (dllexport) void destroy_eer (EER a);

__declspec (dllexport) void flush_eer (EER a);

__declspec (dllexport) void xeer (EER a);

__declspec (dllexport) void pSetEERRun (EER a, int run);

__declspec (dllexport) void pSetEERAMIQ (EER a, int amiq);

__declspec (dllexport) void pSetEERMgain (EER a, double gain);

__declspec (dllexport) void pSetEERPgain (EER a, double gain);

__declspec (dllexport) void pSetEERRunDelays (EER a, int run);

__declspec (dllexport) void pSetEERMdelay (EER a, double delay);

__declspec (dllexport) void pSetEERPdelay (EER a, double delay);

__declspec (dllexport) void pSetEERSize (EER a, int size);

__declspec (dllexport) void pSetEERSamplerate (EER a, int rate);

#endif