#pragma once

/* WDSP's TX leveler (wcpAGC.c in create_txa()'s leveler configuration) as a
   standalone block, with no TXA channel behind it. The SetTXALeveler*
   properties reach the object through txa[channel]; these take the object.

   Threading: create and destroy allocate, so they stay off the audio thread.
   The setters only recompute constants (loadWcpAGC) and belong to the one
   thread that processes the block. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AetherLeveler AetherLeveler;

/* create_txa()'s leveler: max gain 5 dB, attack 1 ms, decay 500 ms, hang
   off, output target 1.05. Returns NULL if a size is unusable. */
AetherLeveler* aether_leveler_create(int blockSize, int sampleRate);
void aether_leveler_destroy(AetherLeveler* lev);

/* The block's buffer: blockSize interleaved (re, im) pairs, processed in
   place by aether_leveler_process(). */
double* aether_leveler_buffer(AetherLeveler* lev);
int aether_leveler_block_size(const AetherLeveler* lev);
void aether_leveler_process(AetherLeveler* lev);
void aether_leveler_flush(AetherLeveler* lev);

void aether_leveler_set_max_gain_db(AetherLeveler* lev, double db);
/* Attack is also the look-ahead; wcpAGC's ring holds at most 10 ms of it. */
void aether_leveler_set_attack_ms(AetherLeveler* lev, double ms);
void aether_leveler_set_decay_ms(AetherLeveler* lev, double ms);
/* 0 turns hang off. */
void aether_leveler_set_hang_ms(AetherLeveler* lev, double ms);

/* Samples of look-ahead delay at the current attack. */
int aether_leveler_lookahead(const AetherLeveler* lev);
/* The gain applied to the most recent sample, in dB. */
double aether_leveler_gain_db(const AetherLeveler* lev);

#ifdef __cplusplus
}
#endif
