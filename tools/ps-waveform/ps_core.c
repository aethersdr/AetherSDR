#include "ps_core.h"

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

#define PS_CHANNEL 0
// Thetis' values: skip the PA's first 0.1 s after keying, and recalibrate
// continuously with no pause between fits.
#define PS_MOX_DELAY 0.1
#define PS_LOOP_DELAY 0.0
// Cross-fade time between correction sets, seconds.
#define PS_CHANGEOVER 0.005

struct ps_core {
    int block;
    int keyed;
    double peak;
};

ps_core* ps_core_create(int rate, int block, double peak)
{
    if (rate <= 0 || block <= 0 || !(peak > 0))
        return NULL;
    ps_core* c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->block = block;
    c->peak = peak;
    InitializeCriticalSectionAndSpinCount(&ch[PS_CHANNEL].csDSP, 2500);
    // iqc first: calcc's worker thread hands its fits to txa[].iqc.p.
    txa[PS_CHANNEL].iqc.p = create_iqc(0, block, NULL, NULL, (double)rate, PS_CHANGEOVER);
    txa[PS_CHANNEL].calcc.p = create_calcc(PS_CHANNEL, 1, block, rate, 1.0 / peak,
                                           PS_MOX_DELAY, PS_LOOP_DELAY, 0);
    if (!txa[PS_CHANNEL].iqc.p || !txa[PS_CHANNEL].calcc.p) {
        ps_core_destroy(c);
        return NULL;
    }
    // Recalibrate automatically while keyed, as Thetis' "Auto" does.
    SetPSControl(PS_CHANNEL, 0, 0, 1, 0);
    return c;
}

void ps_core_destroy(ps_core* c)
{
    if (txa[PS_CHANNEL].calcc.p) {
        destroy_calcc(txa[PS_CHANNEL].calcc.p);
        txa[PS_CHANNEL].calcc.p = NULL;
    }
    if (txa[PS_CHANNEL].iqc.p) {
        destroy_iqc(txa[PS_CHANNEL].iqc.p);
        txa[PS_CHANNEL].iqc.p = NULL;
    }
    if (c)
        DeleteCriticalSection(&ch[PS_CHANNEL].csDSP);
    free(c);
}

void ps_core_correct(ps_core* c, double* iq, int n)
{
    // iqc indexes its curves by the clean signal's amplitude on WDSP's
    // full scale, where 1.0 is the transmit peak; calcc measures the same
    // axis through hw_scale = 1/peak. Scale into that range and back.
    IQC q = txa[PS_CHANNEL].iqc.p;
    const double in = 1.0 / c->peak, out = c->peak;
    while (n > 0) {
        int m = n < c->block ? n : c->block;
        for (int k = 0; k < 2 * m; k++)
            iq[k] *= in;
        // WDSP runs xiqc under the channel's csDSP, the lock calcc's worker
        // takes to swap correction sets; without it a swap can free a set
        // xiqc is reading.
        EnterCriticalSection(&ch[PS_CHANNEL].csDSP);
        setBuffers_iqc(q, iq, iq);
        setSize_iqc(q, m);
        xiqc(q);
        LeaveCriticalSection(&ch[PS_CHANNEL].csDSP);
        for (int k = 0; k < 2 * m; k++)
            iq[k] *= out;
        iq += 2 * m;
        n -= m;
    }
}

void ps_core_calibrate(ps_core* c, const double* tx, const double* rx, int n)
{
    (void)c;
    // pscc takes non-const pointers but only reads them when no extra
    // alignment delay is configured, which this core never sets.
    if (n > 0)
        pscc(PS_CHANNEL, n, (double*)tx, (double*)rx);
}

void ps_core_set_keyed(ps_core* c, int keyed)
{
    c->keyed = keyed != 0;
    SetPSMox(PS_CHANNEL, c->keyed);
}

void ps_core_reset(ps_core* c)
{
    (void)c;
    SetPSControl(PS_CHANNEL, 1, 0, 1, 0);
}

static int info_at(int i)
{
    int info[16];
    GetPSInfo(PS_CHANNEL, info);
    return info[i];
}

int ps_core_correcting(const ps_core* c) { (void)c; return info_at(14); }
int ps_core_state(const ps_core* c) { (void)c; return info_at(15); }
int ps_core_fit_flags(const ps_core* c) { (void)c; return info_at(0) | (info_at(6) << 8); }
int ps_core_swap_pending(const ps_core* c)
{
    (void)c;
    return (int)(_InterlockedAnd(&txa[PS_CHANNEL].iqc.p->busy, 1) != 0);
}

int ps_core_fits(const ps_core* c) { (void)c; return info_at(7); }
