#include "pd_core.h"

#include "comm.h"
#include "pd_core_fence.h"
#include <stdatomic.h>
#include <limits.h>
#include <sys/resource.h>
#include <string.h>

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
    double* bypass;
    IQC cached_curve;
    int cache_valid, cache_used, transition_left;
    int transport_pending;
    int transition_samples;
    atomic_uint generation, requested, published, rejected;
};

static pd_core* core_slots[MAX_CHANNELS];

void pd_fit_thread_start(void)
{
    /* Lower only this Linux worker's priority; no elevated scheduling needed. */
    (void)setpriority(PRIO_PROCESS,0,10);
}

void pd_fit_request(int channel)
{
    if (channel >= 0 && channel < MAX_CHANNELS && core_slots[channel]) {
        pd_core* c = core_slots[channel];
        atomic_store_explicit(&c->requested, atomic_load_explicit(&c->generation, memory_order_acquire), memory_order_release);
    }
}

int pd_fit_publish(int channel)
{
    if (channel < 0 || channel >= MAX_CHANNELS || !core_slots[channel]) {
        return 0;
    }
    pd_core* c = core_slots[channel];
    unsigned requested = atomic_exchange_explicit(&c->requested, 0, memory_order_acq_rel);
    unsigned current = atomic_load_explicit(&c->generation, memory_order_acquire);
    if (!current || requested != current) {
        atomic_fetch_add_explicit(&c->rejected, 1, memory_order_relaxed);
        return 0;
    }
    unsigned previous = atomic_exchange_explicit(&c->published, current, memory_order_acq_rel);
    return previous == current ? 1 : 2;
}

int pd_core_current_solution(const pd_core* c)
{
    unsigned current = atomic_load_explicit(&c->generation, memory_order_acquire);
    return current && atomic_load_explicit(&c->published, memory_order_acquire) == current;
}

unsigned pd_core_rejected_fits(const pd_core* c)
{
    return atomic_load_explicit(&c->rejected, memory_order_relaxed);
}

pd_core* pd_core_create(int rate, int block, double peak)
{
    if (rate <= 0 || block <= 0 || !(peak > 0))
        return NULL;
    pd_core* c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->block = block;
    c->peak = peak;
    c->transition_samples = (int)(rate * PD_CHANGEOVER);
    c->bypass = calloc((size_t)block * 2, sizeof(double));
    atomic_init(&c->generation, 1); atomic_init(&c->requested, 0);
    atomic_init(&c->published, 0); atomic_init(&c->rejected, 0);
    if (!c->bypass || !atomic_is_lock_free(&c->generation)) {
        free(c->bypass); free(c); return NULL;
    }
    core_slots[PD_CHANNEL] = c;
    InitializeCriticalSectionAndSpinCount(&ch[PD_CHANNEL].csDSP, 2500);
    // iqc first: calcc's worker thread hands its fits to txa[].iqc.p.
    txa[PD_CHANNEL].iqc.p = create_iqc(0, block, NULL, NULL, (double)rate, PD_CHANGEOVER);
    c->cached_curve = create_iqc(0, block, NULL, NULL, (double)rate, PD_CHANGEOVER);
    txa[PD_CHANNEL].calcc.p = create_calcc(PD_CHANNEL, 1, block, rate, 1.0 / peak,
                                           PD_MOX_DELAY, PD_LOOP_DELAY, 0);
    if (!txa[PD_CHANNEL].iqc.p || !txa[PD_CHANNEL].calcc.p || !c->cached_curve) {
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
    if (c && c->cached_curve) {
        destroy_iqc(c->cached_curve);
    }
    if (c)
        DeleteCriticalSection(&ch[PD_CHANNEL].csDSP);
    core_slots[PD_CHANNEL] = NULL;
    if (c) { free(c->bypass); }
    free(c);
}

static void correct(pd_core* c, double* iq, int n, int use_cache)
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
        int approved = pd_core_current_solution(c);
        int cached = use_cache && c->keyed && c->cache_valid;
        int transition = !cached && approved && c->cache_valid && c->cache_used;
        if (!approved || cached || transition) {
            memcpy(c->bypass, iq, (size_t)m * 2 * sizeof(double));
        }
        setBuffers_iqc(q, iq, iq);
        setSize_iqc(q, m);
        xiqc(q);
        if (cached || transition) {
            setBuffers_iqc(c->cached_curve, c->bypass, c->bypass);
            setSize_iqc(c->cached_curve, m);
            xiqc(c->cached_curve);
            if (cached) {
                memcpy(iq, c->bypass, (size_t)m * 2 * sizeof(double));
                c->cache_used = 1;
                c->transition_left = c->transition_samples;
            } else {
                for (int k = 0; k < m && c->transition_left > 0; ++k) {
                    int index = c->transition_samples - c->transition_left;
                    double weight = c->cached_curve->cup[index];
                    iq[2*k] = (1-weight)*c->bypass[2*k] + weight*iq[2*k];
                    iq[2*k+1] = (1-weight)*c->bypass[2*k+1] + weight*iq[2*k+1];
                    --c->transition_left;
                }
                if (!c->transition_left) { c->cache_used = 0; }
            }
        } else if (!approved) {
            memcpy(iq, c->bypass, (size_t)m * 2 * sizeof(double));
        }
        LeaveCriticalSection(&ch[PD_CHANNEL].csDSP);
        for (int k = 0; k < 2 * m; k++)
            iq[k] *= out;
        iq += 2 * m;
        n -= m;
    }
}

void pd_core_correct(pd_core* c, double* iq, int n)
{
    correct(c, iq, n, 0);
}
void pd_core_correct_cached(pd_core* c, double* iq, int n)
{
    correct(c, iq, n, 1);
}
int pd_core_cached_solution(const pd_core* c)
{
    return c->cache_valid;
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
    if (keyed && !c->keyed && c->cache_valid) {
        pd_iqc_cached_begin(c->cached_curve);
    }
    c->keyed = keyed != 0;
    SetPSMox(PD_CHANNEL, c->keyed);
}

static void retire(pd_core* c, int retain)
{
    EnterCriticalSection(&ch[PD_CHANNEL].csDSP);
    unsigned current = atomic_load_explicit(&c->generation, memory_order_relaxed);
    c->transport_pending = 0;
    if (retain && pd_core_current_solution(c)) {
        c->cache_valid = pd_iqc_snapshot(c->cached_curve, txa[PD_CHANNEL].iqc.p);
    }
    if (!retain || current == UINT_MAX || !current) {
        c->cache_valid = 0;
    }
    if (retain == 2 && c->cache_valid) {
        c->transport_pending = 1;
        c->cache_valid = 0;
    }
    c->cache_used = c->transition_left = 0;
    atomic_store_explicit(&c->generation, current == UINT_MAX || !current ? 0 : current + 1, memory_order_release);
    atomic_store_explicit(&c->published, 0, memory_order_release);
    LeaveCriticalSection(&ch[PD_CHANNEL].csDSP);
    SetPSControl(PD_CHANNEL, 1, 0, 1, 0);
}

void pd_core_pause(pd_core* c, int retain)
{
    pd_core_set_keyed(c, 0);
    retire(c, retain);
}
void pd_core_invalidate(pd_core* c)
{
    retire(c, 0);
}

int pd_core_suspend_transport(pd_core* c)
{
    if (!c->keyed || c->transport_pending) {
        pd_core_invalidate(c);
        return 0;
    }
    retire(c, 2);
    return c->transport_pending;
}

int pd_core_transport_pending(const pd_core* c)
{
    return c->transport_pending;
}

int pd_core_resume_transport(pd_core* c)
{
    if (!c->keyed || !c->transport_pending) {
        return 0;
    }
    c->transport_pending = 0;
    c->cache_valid = 1;
    pd_iqc_cached_begin(c->cached_curve);
    return 1;
}

void pd_core_reset(pd_core* c)
{
    pd_core_invalidate(c);
}

static int info_at(int i)
{
    int info[16];
    GetPSInfo(PD_CHANNEL, info);
    return info[i];
}

int pd_core_correcting(const pd_core* c) { return pd_core_current_solution(c) && info_at(14); }
int pd_core_state(const pd_core* c) { (void)c; return info_at(15); }
int pd_core_fit_flags(const pd_core* c) { (void)c; return info_at(0) | (info_at(6) << 8); }
int pd_core_swap_pending(const pd_core* c)
{
    (void)c;
    return (int)(_InterlockedAnd(&txa[PD_CHANNEL].iqc.p->busy, 1) != 0);
}

int pd_core_fits(const pd_core* c) { (void)c; return info_at(7); }

int pd_core_collection_counts(const pd_core* c, int counts[16])
{
    (void)c;
    return pd_calcc_collection_counts(PD_CHANNEL, counts, 16);
}
