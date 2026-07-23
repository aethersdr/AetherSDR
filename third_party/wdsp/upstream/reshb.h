/*  reshb.h

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


#ifndef _reshb_h
#define _reshb_h

typedef struct
{
    double i;
    double q;
} complex_t;

typedef struct _hbres
{
    int N;
    double* h;
    complex_t* ring;
    int ring_ptr;
    complex_t* in;
    complex_t* out;
    int size;
} hbres, * HBRES;

struct HBRESdata 
{
    uint32_t run;
    complex_t* in;
    complex_t* out;
    uint32_t insize;
    uint32_t inrate;
    uint32_t outrate;
    uint32_t nStages;
    uint32_t taps[10];
    uint32_t nBuffs;
    uint32_t bsize[10];
    complex_t* buff[10];
    hbres rsmps[10];
};

typedef struct HBRESdata* HBResampler;

extern HBResampler create_HBResampler(uint32_t inrate, uint32_t outrate, uint32_t insize, 
    complex_t* in, complex_t* out);

extern void xHBResampler(HBResampler h);

extern void destroy_HBResampler(HBResampler h);

extern void flush_HBResampler(HBResampler tData);

extern void setBuffers_HBResampler(HBResampler tData, complex_t* in, complex_t* out);

extern void setSize_HBResampler(HBResampler tData, uint32_t insize);

extern void setInRate_HBResampler(HBResampler tData, int inrate);

extern void setOutRate_HBResampler(HBResampler tData, int outrate);

#endif

