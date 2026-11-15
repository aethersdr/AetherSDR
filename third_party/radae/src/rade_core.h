/* Copyright (c) 2022 Amazon
   Written by Jan Buethe */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentatio
   n and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef RADECORE_H
#define RADECORE_H

#include <stdlib.h>

#include "opus_types.h"
#include "nnet.h"

typedef struct RADEDec RADEDec; 
typedef struct RADEEnc RADEEnc;
typedef struct RADEDecStruct RADEDecState;
typedef struct RADEEncStruct RADEEncState;

void rade_init_encoder(RADEEncState *enc_state);
void rade_core_encoder(RADEEncState *enc_state, const RADEEnc *model, float *z, const float *features, int arch, int bottleneck);

void rade_init_decoder(RADEDecState *dec_state);
void rade_core_decoder(RADEDecState *dec_state, const RADEDec *model, float *features, const float *z_hat, int arch);

extern const WeightArray radeenc_arrays[];
extern const WeightArray radedec_arrays[];

#endif
