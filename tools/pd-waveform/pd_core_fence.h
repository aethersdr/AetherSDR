#pragma once

/* Embedded waveform hooks. A request is tagged before its worker is woken.
 * Publication is checked under WDSP's existing csDSP lock. */
void pd_fit_request(int channel);
/* 0 rejects; 1 replaces the current model; 2 begins a fresh generation. */
int pd_fit_publish(int channel);

void pd_fit_thread_start(void);

struct _iqc;
/* Caller holds csDSP; the destination owns no spline pointers. */
int pd_iqc_snapshot(struct _iqc* destination, const struct _iqc* source);
void pd_iqc_cached_begin(struct _iqc* curve);

/* Packet thread only, serialized with pscc collection; returns full bins. */
int pd_calcc_collection_counts(int channel, int* counts, int capacity);
