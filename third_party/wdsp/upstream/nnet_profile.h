/*  nnet_profile.h

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

// #define NNET_PROFILE

#ifndef _nnet_profile_h
#define _nnet_profile_h

#ifdef NNET_PROFILE

#include <stdio.h>

#ifndef NNP_WARMUP_FRAMES
#define NNP_WARMUP_FRAMES   312
#endif
#ifndef NNP_MEASURE_FRAMES
#define NNP_MEASURE_FRAMES  3750
#endif

#ifndef NNP_OUTFILE
#define NNP_OUTFILE  "nnr_profile.txt"
#endif

enum {
    NNP_ENC0 = 0, NNP_ENC1, NNP_ENC2, NNP_ENC3,
    NNP_DP0, NNP_DP1,
    NNP_DEC0, NNP_DEC1, NNP_DEC2, NNP_DEC3,
    NNP_COND, NNP_DFHEAD, NNP_TOTAL,
    NNP_NSTAGE
};

extern double  nnp_sec[NNP_NSTAGE];
extern long long nnp_cnt[NNP_NSTAGE];

extern double nnp_now (void);

#define NNP_START(id)   double _nnp_t##id = nnp_now()
#define NNP_STOP(id)    do {                                             \
                            nnp_sec[id] += nnp_now() - _nnp_t##id;       \
                            nnp_cnt[id] += 1;                            \
                        } while (0)

extern void report_profile_nnet (void);
extern void reset_profile_nnet (void);

#else

#define NNP_START(id)   ((void)0)
#define NNP_STOP(id)    ((void)0)

#endif

#endif
