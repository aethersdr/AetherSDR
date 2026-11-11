#include "pd_core.h"

#include "comm.h"

// calcc.c's control entry points. They are exported from WDSP's DLL build
// through wdsp.h, which can't be included next to comm.h, so declare them.
void SetPSControl(int channel, int reset, int mancal, int automode, int turnon);
void SetPSMox(int channel, int mox);
void GetPSInfo(int channel, int* info);

#include <stdlib.h>

// WDSP's calcc and iqc reach each other through txa[channel]: calcc keeps its
// lock there and hands finished fits to txa[channel].iqc.p. The waveform runs
// one transmitter, so it uses slot 0 and supplies the array itself instead of
// linking WDSP's whole transmit channel (TXA.c), which would pull in FFTW and
// every other DSP stage.
struct _txa txa[MAX_CHANNELS];
// iqc's setters take ch[channel].csDSP, and destroy_calcc() reads the DSP
// worker's generation counters there; with no WDSP channel worker they stay
// zero, which destroy_calcc() reads as "worker already gone".
struct _ch ch[MAX_CHANNELS];

#define PD_CHANNEL 0
// Thetis' values: skip the PA's first 0.1 s after keying, and recalibrate
// continuously with no pause between fits.
#define PD_MOX_DELAY 0.1
#define PD_LOOP_DELAY 0.0
// Cross-fade time between correction sets, seconds.
#define PD_CHANGEOVER 0.005

struct pd_core {
    int block;
    int keyed;
    double peak;
};

pd_core* pd_core_create(int rate, int block, double peak)
{
    if (rate <= 0 || block <= 0 || !(peak > 0))
        return NULL;
    pd_core* c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->block = block;
    c->peak = peak;
    InitializeCriticalSectionAndSpinCount(&ch[PD_CHANNEL].csDSP, 2500);
    // iqc first: calcc's worker thread hands its fits to txa[].iqc.p.
    txa[PD_CHANNEL].iqc.p = create_iqc(0, block, NULL, NULL, (double)rate, PD_CHANGEOVER);
    txa[PD_CHANNEL].calcc.p = create_calcc(PD_CHANNEL, 1, block, rate, 1.0 / peak,
                                           PD_MOX_DELAY, PD_LOOP_DELAY, 0);
    if (!txa[PD_CHANNEL].iqc.p || !txa[PD_CHANNEL].calcc.p) {
        pd_core_destroy(c);
        return NULL;
    }
    // Recalibrate automatically while keyed, as Thetis' "Auto" does.
    SetPSControl(PD_CHANNEL, 0, 0, 1, 0);
    return c;
}

void pd_core_destroy(pd_core* c)
{
    if (txa[PD_CHANNEL].calcc.p) {
        destroy_calcc(txa[PD_CHANNEL].calcc.p);
        txa[PD_CHANNEL].calcc.p = NULL;
    }
    if (txa[PD_CHANNEL].iqc.p) {
        destroy_iqc(txa[PD_CHANNEL].iqc.p);
        txa[PD_CHANNEL].iqc.p = NULL;
    }
    if (c)
        DeleteCriticalSection(&ch[PD_CHANNEL].csDSP);
    free(c);
}

void pd_core_correct(pd_core* c, double* iq, int n)
{
    // iqc indexes its curves by the clean signal's amplitude on WDSP's
    // full scale, where 1.0 is the transmit peak; calcc measures the same
    // axis through hw_scale = 1/peak. Scale into that range and back.
    IQC q = txa[PD_CHANNEL].iqc.p;
    const double in = 1.0 / c->peak, out = c->peak;
    while (n > 0) {
        int m = n < c->block ? n : c->block;
        for (int k = 0; k < 2 * m; k++)
            iq[k] *= in;
        // WDSP runs xiqc under the channel's csDSP, the lock calcc's worker
        // takes to swap correction sets; without it a swap can free a set
        // xiqc is reading.
        EnterCriticalSection(&ch[PD_CHANNEL].csDSP);
        setBuffers_iqc(q, iq, iq);
        setSize_iqc(q, m);
        xiqc(q);
        LeaveCriticalSection(&ch[PD_CHANNEL].csDSP);
        for (int k = 0; k < 2 * m; k++)
            iq[k] *= out;
        iq += 2 * m;
        n -= m;
    }
}

void pd_core_calibrate(pd_core* c, const double* tx, const double* rx, int n)
{
    (void)c;
    // pscc takes non-const pointers but only reads them when no extra
    // alignment delay is configured, which this core never sets.
    if (n > 0)
        pscc(PD_CHANNEL, n, (double*)tx, (double*)rx);
}

void pd_core_set_keyed(pd_core* c, int keyed)
{
    c->keyed = keyed != 0;
    SetPSMox(PD_CHANNEL, c->keyed);
}

void pd_core_reset(pd_core* c)
{
    (void)c;
    SetPSControl(PD_CHANNEL, 1, 0, 1, 0);
}

static int info_at(int i)
{
    int info[16];
    GetPSInfo(PD_CHANNEL, info);
    return info[i];
}

int pd_core_correcting(const pd_core* c) { (void)c; return info_at(14); }
int pd_core_state(const pd_core* c) { (void)c; return info_at(15); }
int pd_core_fit_flags(const pd_core* c) { (void)c; return info_at(0) | (info_at(6) << 8); }
int pd_core_swap_pending(const pd_core* c)
{
    (void)c;
    return (int)(_InterlockedAnd(&txa[PD_CHANNEL].iqc.p->busy, 1) != 0);
}

int pd_core_fits(const pd_core* c) { (void)c; return info_at(7); }
