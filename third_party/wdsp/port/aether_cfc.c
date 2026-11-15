/* aether_cfc.c -- WDSP's CFC (upstream/cfcomp.c) as a standalone block.

The object-scoped setters below re-express cfcomp.c's TXA properties
(SetTXACFCOMPprofile, SetTXACFCOMPPrecomp, SetTXACFCOMPPeqRun,
SetTXACFCOMPPrePeq, GetTXACFCOMPDisplayCompression) on the object itself.
upstream/ is unchanged.

This file is part of AetherSDR. GPL-2.0-or-later, as WDSP. */

#include "aether_wdsp_cfc.h"

#include "comm.h"

/* Not in cfcomp.h, but external in cfcomp.c. */
extern void calc_compG(CFCOMP a);
extern void calc_compE(CFCOMP a);

struct AetherCfc {
    CFCOMP cfc;
    double* buffer;
    int blockSize;
    int fftSize;
};

/* create_txa()'s profile. The caller sets its own straight after. */
static double kDefaultF[5] = {200.0, 1000.0, 2000.0, 3000.0, 4000.0};
static double kDefaultG[5] = {0.0, 5.0, 10.0, 10.0, 5.0};
static double kDefaultE[5] = {7.0, 7.0, 7.0, 7.0, 7.0};

AetherCfc* aether_cfc_create(int fftSize, int blockSize, int sampleRate)
{
    /* cfcomp's overlap is 4, so the frame hop is fftSize / 4. */
    if (fftSize < 64 || (fftSize & (fftSize - 1)) != 0 || blockSize < 1
        || blockSize > fftSize || sampleRate <= 0) {
        return NULL;
    }
    AetherCfc* a = (AetherCfc*)malloc0(sizeof(AetherCfc));
    if (!a) {
        return NULL;
    }
    a->blockSize = blockSize;
    a->fftSize = fftSize;
    a->buffer = (double*)malloc0((size_t)blockSize * sizeof(complex));
    if (!a->buffer) {
        _aligned_free(a);
        return NULL;
    }
    /* run 1, position 0 and post-EQ off; window, method and the meter time
       constants are create_txa()'s. */
    a->cfc = create_cfcomp(1, 0, 0, blockSize, a->buffer, a->buffer, fftSize, 4,
                           sampleRate, 1, 0, 5, 5, 0.0, 0.0, kDefaultF, kDefaultG,
                           kDefaultF, kDefaultE, 0.25, 0.50);
    if (!a->cfc) {
        _aligned_free(a->buffer);
        _aligned_free(a);
        return NULL;
    }
    return a;
}

void aether_cfc_destroy(AetherCfc* a)
{
    if (!a) {
        return;
    }
    destroy_cfcomp(a->cfc);
    _aligned_free(a->buffer);
    _aligned_free(a);
}

int aether_cfc_block_size(const AetherCfc* a)
{
    return a ? a->blockSize : 0;
}

int aether_cfc_fft_size(const AetherCfc* a)
{
    return a ? a->fftSize : 0;
}

double* aether_cfc_buffer(AetherCfc* a)
{
    return a ? a->buffer : NULL;
}

void aether_cfc_process(AetherCfc* a)
{
    if (a) {
        xcfcomp(a->cfc, 0);
    }
}

void aether_cfc_flush(AetherCfc* a)
{
    if (a) {
        flush_cfcomp(a->cfc);
        memset(a->buffer, 0, (size_t)a->blockSize * sizeof(complex));
    }
}

int aether_cfc_set_profile(AetherCfc* a, int count, const double* hz,
                           const double* compDb, const double* eqDb)
{
    if (!a || count < 1 || count > a->cfc->max_freqs || !hz || !compDb || !eqDb) {
        return 0;
    }
    CFCOMP c = a->cfc;
    c->nfreqsG = count;
    c->nfreqsE = count;
    memcpy(c->Fg, hz, (size_t)count * sizeof(double));
    memcpy(c->Fe, hz, (size_t)count * sizeof(double));
    memcpy(c->G, compDb, (size_t)count * sizeof(double));
    memcpy(c->E, eqDb, (size_t)count * sizeof(double));
    calc_compG(c);
    calc_compE(c);
    return 1;
}

void aether_cfc_set_precomp_db(AetherCfc* a, double db)
{
    if (!a || a->cfc->precomp == db) {
        return;
    }
    CFCOMP c = a->cfc;
    c->precomp = db;
    c->precomplin = pow(10.0, 0.05 * db);
    for (int i = 0; i < c->msize; i++) {
        c->cfc_gain[i] = c->precomplin * c->comp[i];
    }
}

void aether_cfc_set_post_eq(AetherCfc* a, int enabled)
{
    if (a) {
        a->cfc->peq_run = enabled ? 1 : 0;
    }
}

void aether_cfc_set_post_gain_db(AetherCfc* a, double db)
{
    if (a) {
        a->cfc->prepeq = db;
        a->cfc->prepeqlin = pow(10.0, 0.05 * db);
    }
}

/* GetTXACFCOMPDisplayCompression's formula. Its bin step, (msize - 1) / 1024,
   is 0 below a 2048-point FFT, and sampling one bin per point misses a tone
   between points, so each point shows the most compression across the bins
   it covers. */
static double bin_compression_db(CFCOMP c, int j)
{
    const double gain = c->cfc_gain[j];
    const double delta = c->delta[j];
    if (delta <= 0.0 || gain <= 0.0) {
        return 0.0;
    }
    const double denom = gain - delta;
    return denom <= 1e-20 ? 40.0 : 20.0 * mlog10(gain / denom);
}

int aether_cfc_compression_db(AetherCfc* a, float* out, int points)
{
    if (!a || !out || points < 2 || !a->cfc->mask_ready) {
        return 0;
    }
    CFCOMP c = a->cfc;
    c->mask_ready = 0;
    const long long lastBin = c->msize - 1;
    for (int i = 0; i < points; i++) {
        /* Bins nearer this point than its neighbours. */
        int from = (int)(((2LL * i - 1) * lastBin) / (2LL * (points - 1)));
        int to = (int)(((2LL * i + 1) * lastBin) / (2LL * (points - 1)));
        if (from < 0) {
            from = 0;
        }
        if (to > lastBin) {
            to = (int)lastBin;
        }
        double most = 0.0;
        for (int j = from; j <= to; j++) {
            const double db = bin_compression_db(c, j);
            if (db > most) {
                most = db;
            }
        }
        out[i] = (float)most;
    }
    return 1;
}
