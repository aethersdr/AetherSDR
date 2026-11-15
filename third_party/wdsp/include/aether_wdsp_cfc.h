#pragma once

/* WDSP's Continuous Frequency Compressor (cfcomp.c) as a standalone block,
   with no TXA channel behind it. cfcomp's own setters reach the object
   through txa[channel]; these take the object itself.

   Threading: create and destroy allocate and build FFTW plans, so the caller
   holds the process FFTW planner lock and never calls them on the audio
   thread. Everything else is allocation-free and belongs to the one thread
   that processes the block. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AetherCfc AetherCfc;

/* Real audio in, real audio out, one block at a time. The block's delay is
   fftSize - blockSize samples. Returns NULL if a size is unusable. */
AetherCfc* aether_cfc_create(int fftSize, int blockSize, int sampleRate);
void aether_cfc_destroy(AetherCfc* cfc);

int aether_cfc_block_size(const AetherCfc* cfc);
int aether_cfc_fft_size(const AetherCfc* cfc);

/* The block's buffer: blockSize interleaved (re, im) pairs. Write the real
   input into the re slots, call aether_cfc_process(), read the re slots. */
double* aether_cfc_buffer(AetherCfc* cfc);
void aether_cfc_process(AetherCfc* cfc);
void aether_cfc_flush(AetherCfc* cfc);

/* Per-frequency compression and post-EQ, in dB, at `count` frequencies in Hz
   (1..256). Returns 0 and changes nothing if the arguments are unusable. */
int aether_cfc_set_profile(AetherCfc* cfc, int count, const double* hz,
                           const double* compDb, const double* eqDb);
void aether_cfc_set_precomp_db(AetherCfc* cfc, double db);
void aether_cfc_set_post_eq(AetherCfc* cfc, int enabled);
void aether_cfc_set_post_gain_db(AetherCfc* cfc, double db);

/* Compression being applied, in dB per frequency, at `points` frequencies
   spaced evenly from 0 Hz to Nyquist. Returns 1 if a new analysis frame was
   ready and `out` was written, 0 otherwise. */
int aether_cfc_compression_db(AetherCfc* cfc, float* out, int points);

#ifdef __cplusplus
}
#endif
