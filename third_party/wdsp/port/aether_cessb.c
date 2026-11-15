/* aether_cessb.c -- WDSP's SSB speech processor (compress.c + osctrl.c
between bandpass.c filters, as xtxa() runs them) as a standalone block.

upstream/ is unchanged. This file is part of AetherSDR. GPL-2.0-or-later, as
WDSP. */

#include "aether_wdsp_cessb.h"

#include "comm.h"

struct AetherCessb {
    double* buffer;
    int blockSize;
    int nc;
    BANDPASS bp0;
    COMPRESSOR comp;
    BANDPASS bp1;
    OSCTRL osctrl;
    BANDPASS bp2;
};

AetherCessb* aether_cessb_create(int blockSize, int sampleRate, int nc, double lowHz,
                                 double highHz)
{
    if (blockSize < 1 || sampleRate <= 0 || nc < blockSize || (nc & (nc - 1)) != 0
        || lowHz < 0.0 || highHz <= lowHz || highHz >= sampleRate / 2.0) {
        return NULL;
    }
    AetherCessb* a = (AetherCessb*)malloc0(sizeof(AetherCessb));
    if (!a) {
        return NULL;
    }
    a->blockSize = blockSize;
    a->nc = nc;
    /* fircore's inverse FFT writes 2 * size complex samples into its output,
       so the shared buffer is twice the block, as TXA's midbuff is. */
    a->buffer = (double*)malloc0(2 * (size_t)blockSize * sizeof(complex));
    /* Gains and the CESSB clipping gain are create_txa()'s and
       TXASetupBPFilters()'s: bp0 and bp1 x2 (the input and the clipper's
       output are real, so half their energy falls outside the one-sided
       passband), bp2 x1, osgain 1.95. Linear phase, as WDSP's guide
       requires for CESSB. */
    a->bp0 = create_bandpass(1, 0, blockSize, nc, 0, a->buffer, a->buffer, lowHz, highHz,
                             sampleRate, 1, 2.0);
    a->comp = create_compressor(1, blockSize, a->buffer, a->buffer, 1.0);
    a->bp1 = create_bandpass(1, 0, blockSize, nc, 0, a->buffer, a->buffer, lowHz, highHz,
                             sampleRate, 1, 2.0);
    a->osctrl = create_osctrl(1, blockSize, a->buffer, a->buffer, sampleRate, 1.95);
    a->bp2 = create_bandpass(1, 0, blockSize, nc, 0, a->buffer, a->buffer, lowHz, highHz,
                             sampleRate, 1, 1.0);
    if (!a->buffer || !a->bp0 || !a->comp || !a->bp1 || !a->osctrl || !a->bp2) {
        aether_cessb_destroy(a);
        return NULL;
    }
    return a;
}

void aether_cessb_destroy(AetherCessb* a)
{
    if (!a) {
        return;
    }
    if (a->bp2) destroy_bandpass(a->bp2);
    if (a->osctrl) destroy_osctrl(a->osctrl);
    if (a->bp1) destroy_bandpass(a->bp1);
    if (a->comp) destroy_compressor(a->comp);
    if (a->bp0) destroy_bandpass(a->bp0);
    if (a->buffer) _aligned_free(a->buffer);
    _aligned_free(a);
}

double* aether_cessb_buffer(AetherCessb* a)
{
    return a ? a->buffer : NULL;
}

int aether_cessb_block_size(const AetherCessb* a)
{
    return a ? a->blockSize : 0;
}

void aether_cessb_process(AetherCessb* a)
{
    if (!a) {
        return;
    }
    xbandpass(a->bp0, 0);
    xcompressor(a->comp);
    xbandpass(a->bp1, 0);
    xosctrl(a->osctrl);
    xbandpass(a->bp2, 0);
}

void aether_cessb_flush(AetherCessb* a)
{
    if (!a) {
        return;
    }
    flush_bandpass(a->bp0);
    flush_bandpass(a->bp1);
    flush_osctrl(a->osctrl);
    flush_bandpass(a->bp2);
    memset(a->buffer, 0, 2 * (size_t)a->blockSize * sizeof(complex));
}

void aether_cessb_set_clip_gain_db(AetherCessb* a, double db)
{
    if (a) {
        a->comp->gain = pow(10.0, db / 20.0);
    }
}

void aether_cessb_set_overshoot_control(AetherCessb* a, int enabled)
{
    if (a) {
        a->osctrl->run = enabled ? 1 : 0;
    }
}

int aether_cessb_delay(const AetherCessb* a)
{
    /* Three linear-phase bandpasses of nc taps, nc / 2 each. */
    return a ? 3 * (a->nc / 2) : 0;
}
