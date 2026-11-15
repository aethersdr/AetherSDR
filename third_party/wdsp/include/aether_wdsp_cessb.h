#pragma once

/* WDSP's SSB speech processor as a standalone block, with no TXA channel
   behind it: the bandpass that makes the one-sided SSB signal (bp0), the
   envelope clipper (compress.c, "COMP"), the bandpass that removes its
   splatter (bp1), CESSB overshoot control (osctrl.c), the last bandpass
   (bp2) and create_txa()'s ALC (wcpAGC.c) as a look-ahead backstop on the
   envelope, in xtxa()'s order. Its output is the real part, which is ordinary
   audio whose SSB envelope the clipper and CESSB have bounded.

   Threading: create and destroy allocate and build FFTW plans, so the caller
   holds the process FFTW planner lock and never calls them on the audio
   thread. The setters are allocation-free and belong to the thread that
   processes the block. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AetherCessb AetherCessb;

/* Passband in Hz (positive, low < high). nc is each bandpass's length in
   taps, a power of two >= blockSize; each adds nc / 2 samples of delay. */
AetherCessb* aether_cessb_create(int blockSize, int sampleRate, int nc, double lowHz,
                                 double highHz);
void aether_cessb_destroy(AetherCessb* c);

double* aether_cessb_buffer(AetherCessb* c);  /* blockSize (re, im) pairs */
int aether_cessb_block_size(const AetherCessb* c);
void aether_cessb_process(AetherCessb* c);
void aether_cessb_flush(AetherCessb* c);

/* Drive into the envelope clipper, in dB (COMP's "gain"). */
void aether_cessb_set_clip_gain_db(AetherCessb* c, double db);
/* CESSB overshoot control on or off; the last bandpass runs either way. */
void aether_cessb_set_overshoot_control(AetherCessb* c, int enabled);

/* Samples of delay the three bandpasses and the ALC look-ahead add. */
int aether_cessb_delay(const AetherCessb* c);

#ifdef __cplusplus
}
#endif
