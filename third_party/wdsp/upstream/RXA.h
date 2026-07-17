/*  RXA.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2014, 2015, 2016, 2025, 2026 Warren Pratt, NR0V

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

#ifndef _rxa_h
#define _rxa_h
#include "comm.h"

enum rxaMode
{
	RXA_LSB,
	RXA_USB,
	RXA_DSB,
	RXA_CWL,
	RXA_CWU,
	RXA_FM,
	RXA_AM,
	RXA_DIGU,
	RXA_SPEC,
	RXA_DIGL,
	RXA_SAM,
	RXA_DRM,
	RXA_WBFM = 12
};

enum rxaMeterType
{
	RXA_S_PK,
	RXA_S_AV,
	RXA_ADC_PK,
	RXA_ADC_AV,
	RXA_AGC_GAIN,
	RXA_AGC_PK,
	RXA_AGC_AV,
	RXA_METERTYPE_LAST
};

struct _rxa
{
	double* inbuff;
	double* outbuff;
	double* midbuff;
	int mode;
	double meter[RXA_METERTYPE_LAST];
	CRITICAL_SECTION* pmtupdate[RXA_METERTYPE_LAST];
	struct
	{
		METER p;
	} smeter, adcmeter, agcmeter;
	struct
	{
		SHIFT p;
	} shift;
	struct
	{
		HBResampler p;
	} rsmpin;
	struct
	{
		RESAMPLE p;
	} rsmpout;
	struct
	{
		GEN p;
	} gen0;
	struct
	{
		BANDPASS p;
	} bp1;
	struct
	{
		NOTCHDB p;
	} ndb;
	struct
	{
		NBP p;
	} nbp0;
	struct
	{
		BPSNBA p;
	} bpsnba;
	struct
	{
		SNBA p;
	} snba;
	struct
	{
		SENDER p;
	} sender;
	struct
	{
		AMSQ p;
	} amsq;
	struct
	{
		AMD p;
	} amd;
	struct
	{
		WBFM p;
	} wbfm;
	struct
	{
		FMD p;
	} fmd;
	struct
	{
		FMSQ p;
	} fmsq;
	struct
	{
		EQP p;
	} eqp;
	struct
	{
		ANF p;
	} anf;
	struct
	{
		ANR p;
	} anr;
	struct
	{
		EMNR p;
	} emnr;
	struct
	{
		WCPAGC p;
	} agc;
	struct
	{
		APFSHADOW p;
	} apfshadow;
	struct
	{
		DOUBLEPOLE p;
	} doublepole;
	struct
	{
		MATCHED p;
	} matched;
	struct
	{
		GAUSSIAN p;
	} gaussian;
	struct
	{
		SPEAK p;
	} speak;
	struct
	{
		MPEAK p;
	} mpeak;
	struct
	{
		PANEL p;
	} panel;
	struct
	{
		SIPHON p;
	} sip1;
	struct
	{
		CBL p;
	} cbl;
	struct
	{
		SSQL p;
	} ssql;
};

extern struct _rxa rxa[];

extern void create_rxa (int channel);

extern void destroy_rxa (int channel);

extern void flush_rxa (int channel);

extern void xrxa (int channel);

extern void setInputSamplerate_rxa (int channel);

extern void setOutputSamplerate_rxa (int channel);

extern void setDSPSamplerate_rxa (int channel);

extern void setDSPBuffsize_rxa (int channel);

// RXA Properties

extern __declspec (dllexport) void SetRXAMode (int channel, int mode);

extern void RXAResCheck (int channel);

extern void RXAbp1Check (int channel, int amd_run, int snba_run, int emnr_run, int anf_run, int anr_run);

extern void RXAbp1Set (int channel);

extern void RXAbpsnbaCheck (int channel, int mode, int notch_run);

extern void RXAbpsnbaSet (int channel);

#endif
