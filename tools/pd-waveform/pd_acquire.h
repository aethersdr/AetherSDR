#pragma once
#include <stdint.h>

typedef struct pd_acquire pd_acquire;
struct pd_acquire_result {
    uint64_t generation;
    int64_t submitted_ns, started_ns, completed_ns;
    double delay, envelope_correlation, phase_correlation, offset_hz;
};

pd_acquire *pd_acquire_create(int max_delay, int max_samples, double rate);
struct pd_eq;
pd_acquire *pd_acquire_create_with_eq(int max_delay, int max_samples, double rate, int taps);
int pd_acquire_submit_eq(pd_acquire *worker, uint64_t generation,
                         const double *tx, const double *rx, int samples);
/* 0=pending/idle, 1=imported FIR, 2=stale/wrong job, 3=unsolved FIR. */
int pd_acquire_take_eq(pd_acquire *worker, uint64_t generation, struct pd_eq *destination);
void pd_acquire_destroy(pd_acquire *worker);
/* Single producer/consumer. One bounded job; busy returns immediately. */
int pd_acquire_submit(pd_acquire *worker, uint64_t generation,
                      const double *tx, const double *rx, int samples);
/* 0=pending/idle, 1=result for current generation, 2=discarded stale result. */
int pd_acquire_take(pd_acquire *worker, uint64_t current_generation,
                    struct pd_acquire_result *result);
