/*  compress.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2011, 2013 Warren Pratt, NR0V

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

#ifndef _compressor_h
#define _compressor_h

typedef struct _compressor
{
	int run;
	int buffsize;
	double *inbuff;
	double *outbuff;
	double gain;
} compressor, *COMPRESSOR;

extern void xcompressor (COMPRESSOR a);

extern COMPRESSOR create_compressor (
				int run,
				int buffsize,
				double* inbuff,
				double* outbuff,
				double gain );

extern void destroy_compressor (COMPRESSOR a);

extern void flush_compressor (COMPRESSOR a);

extern void setBuffers_compressor (COMPRESSOR a, double* in, double* out);

extern void setSamplerate_compressor (COMPRESSOR a, int rate);

extern void setSize_compressor (COMPRESSOR a, int size);

// TXA Properties

extern __declspec (dllexport) void SetTXACompressorRun (int channel, int run);

extern __declspec (dllexport) void SetTXACompressorGain (int channel, double gain);

#endif