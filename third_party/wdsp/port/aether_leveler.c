/* aether_leveler.c -- WDSP's TX leveler (upstream/wcpAGC.c) as a standalone
block.

The setters re-express wcpAGC.c's SetTXALeveler* properties on the object.
upstream/ is unchanged.

This file is part of AetherSDR. GPL-2.0-or-later, as WDSP. */

#include "aether_wdsp_leveler.h"

#include "comm.h"

/* External in wcpAGC.c, not declared in wcpAGC.h. */
extern void loadWcpAGC(WCPAGC a);

struct AetherLeveler {
    WCPAGC agc;
    double* buffer;
    int blockSize;
};

AetherLeveler* aether_leveler_create(int blockSize, int sampleRate)
{
    if (blockSize < 1 || sampleRate <= 0 || sampleRate > MAX_SAMPLE_RATE) {
        return NULL;
    }
    AetherLeveler* a = (AetherLeveler*)malloc0(sizeof(AetherLeveler));
    if (!a) {
        return NULL;
    }
    a->blockSize = blockSize;
    a->buffer = (double*)malloc0((size_t)blockSize * sizeof(complex));
    if (!a->buffer) {
        _aligned_free(a);
        return NULL;
    }
    /* create_txa()'s leveler arguments, with run 1. */
    a->agc = create_wcpagc(1, 5, 0, a->buffer, a->buffer, blockSize, sampleRate, 0.001, 0.500, 6,
                           1.778, 1.0, 1.0, 1.0, 1.05, 0.250, 0.005, 5.0, 0, 0.500, 0.500, 2.000,
                           0.100);
    if (!a->agc) {
        _aligned_free(a->buffer);
        _aligned_free(a);
        return NULL;
    }
    return a;
}

void aether_leveler_destroy(AetherLeveler* a)
{
    if (!a) {
        return;
    }
    destroy_wcpagc(a->agc);
    _aligned_free(a->buffer);
    _aligned_free(a);
}

double* aether_leveler_buffer(AetherLeveler* a)
{
    return a ? a->buffer : NULL;
}

int aether_leveler_block_size(const AetherLeveler* a)
{
    return a ? a->blockSize : 0;
}

void aether_leveler_process(AetherLeveler* a)
{
    if (a) {
        xwcpagc(a->agc);
    }
}

void aether_leveler_flush(AetherLeveler* a)
{
    if (a) {
        flush_wcpagc(a->agc);
        memset(a->buffer, 0, (size_t)a->blockSize * sizeof(complex));
    }
}

void aether_leveler_set_max_gain_db(AetherLeveler* a, double db)
{
    if (a) {
        a->agc->max_gain = pow(10.0, db / 20.0);
        loadWcpAGC(a->agc);
    }
}

void aether_leveler_set_attack_ms(AetherLeveler* a, double ms)
{
    if (!a) {
        return;
    }
    double tau = ms / 1000.0;
    if (tau > MAX_TAU_ATTACK) {
        tau = MAX_TAU_ATTACK;
    }
    a->agc->tau_attack = tau;
    loadWcpAGC(a->agc);
}

void aether_leveler_set_decay_ms(AetherLeveler* a, double ms)
{
    if (a) {
        a->agc->tau_decay = ms / 1000.0;
        loadWcpAGC(a->agc);
    }
}

void aether_leveler_set_hang_ms(AetherLeveler* a, double ms)
{
    if (!a) {
        return;
    }
    a->agc->hang_enable = ms > 0.0 ? 1 : 0;
    if (ms > 0.0) {
        a->agc->hangtime = ms / 1000.0;
    }
    loadWcpAGC(a->agc);
}

int aether_leveler_lookahead(const AetherLeveler* a)
{
    return a ? a->agc->attack_buffsize : 0;
}

/* xwcpagc's per-sample multiplier, from its current envelope. */
double aether_leveler_gain_db(const AetherLeveler* a)
{
    if (!a || a->agc->volts <= 0.0) {
        return 0.0;
    }
    const WCPAGC c = a->agc;
    const double l = log10(c->inv_max_input * c->volts);
    const double mult = (c->out_target - c->slope_constant * (l < 0.0 ? l : 0.0)) / c->volts;
    return mult > 0.0 ? 20.0 * log10(mult) : -120.0;
}
