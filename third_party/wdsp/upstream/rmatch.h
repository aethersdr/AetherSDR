/*  rmatch.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2017, 2022 Warren Pratt, NR0V

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

#ifndef _rmatch_h
#define _rmatch_h

#include "varsamp.h"

typedef struct _mav
{
	int ringmin;
	int ringmax;		// must be a power of two
	int* ring;
	int mask;
	int i;
	int load;
	int sum;
	double nom_value;
} mav, *MAV;

typedef struct _aamav
{
	int ringmin;
	int ringmax;		// must be a power of two
	int* ring;
	int mask;
	int i;
	int load;
	int pos;
	int neg;
	double nom_ratio;
} aamav, *AAMAV;

typedef struct _rmatch
{
	volatile long run;
	double* in;
	double* out;
	int insize;
	int outsize;
	double* resout;
	int nom_inrate;
	int nom_outrate;
	double nom_ratio;
	double inv_nom_ratio;
	double fc_high;
	double fc_low;
	double gain;
	double startup_delay;
	int auto_ringsize;
	int ringsize;
	int rsize;
	double* ring;
	int n_ring;
	int iin;
	int iout;
	double var;
	int R;
	AAMAV ffmav;
	MAV propmav;
	int ff_ringmin;
	int ff_ringmax;			// must be a power of two
	double ff_alpha;
	double feed_forward;
	int prop_ringmin;
	int prop_ringmax;		// must be a power of two
	double prop_gain;
	double pr_gain;
	double av_deviation;
	VARSAMP v;
	int varmode;
	CRITICAL_SECTION cs_ring;
	CRITICAL_SECTION cs_var;
	// blend / slew
	double tslew;
	int ntslew;
	double* cslew;
	double* baux;
	double dlast[2];
	int ucnt;
	// variables to check start-up time for control to become active
	unsigned int readsamps;
	unsigned int writesamps;
	unsigned int read_startup;
	unsigned int write_startup;
	int control_flag;
	// diagnostics
	volatile long underflows;
	volatile long overflows;
	int force;
	double fvar;
} rmatch, *RMATCH;

extern __declspec (dllexport) void* create_rmatchV(int in_size, int out_size, int nom_inrate, int nom_outrate, int ringsize, double var);

extern __declspec (dllexport) void destroy_rmatchV (void* ptr);

extern __declspec (dllexport) void xrmatchOUT (void* b, double* out);

extern __declspec (dllexport) void xrmatchIN (void* b, double* in);

extern __declspec (dllexport) void setRMatchInsize (void* ptr, int insize);

extern __declspec (dllexport) void setRMatchOutsize (void* ptr, int outsize);

extern __declspec (dllexport) void setRMatchNomInrate (void* ptr, int nom_inrate);

extern __declspec (dllexport) void setRMatchNomOutrate (void* ptr, int nom_outrate);

extern __declspec (dllexport) void setRMatchRingsize (void* ptr, int ringsize);

extern __declspec (dllexport) void getRMatchDiags (void* b, int* underflows, int* overflows, double* var, int* ringsize, int* nring);

extern __declspec (dllexport) void resetRMatchDiags (void* b);

extern __declspec (dllexport) void forceRMatchVar (void* b, int force, double fvar);

extern __declspec (dllexport) void setRMatchFeedbackGain(void* b, double feedback_gain);

extern __declspec (dllexport) void setRMatchSlewTime(void* b, double slew_time);

extern __declspec (dllexport) void setRMatchSlewTime1(void* b, double slew_time);

extern __declspec (dllexport) void setRMatchPropRingMin(void* ptr, int prop_min);

extern __declspec (dllexport) void setRMatchPropRingMax(void* ptr, int prop_max);

extern __declspec (dllexport) void setRMatchFFRingMin(void* ptr, int ff_ringmin);

extern __declspec (dllexport) void setRMatchFFRingMax(void* ptr, int ff_ringmax);

extern __declspec (dllexport) void setRMatchFFAlpha(void* ptr, double ff_alpha);

extern __declspec (dllexport) void getControlFlag(void* ptr, int* control_flag);

#endif
