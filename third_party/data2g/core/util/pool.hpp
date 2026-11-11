// The one shared worker pool (docs/native-port-plan.md, "Threads"): a fixed
// set of threads, min(4, cores / 2) by default, settable at run time.
//
// parallel_for hands out indices; callers keep each index's arithmetic
// independent of the thread that runs it, so results are bit-identical for
// any pool size (tests/test_native_pool.py checks 1 against 4).
#pragma once

#include <cstddef>
#include <functional>

namespace data2g::pool {

// min(4, hardware_concurrency / 2), at least 1; DATA2G_THREADS overrides.
int default_threads();
// Threads including the caller; n < 1: default_threads(). 1 runs no pool
// threads. Waits for a running parallel_for.
void set_threads(int n);
int threads();

// fn(i) for each i in [0, n) on up to threads() threads, the caller one of
// them; returns once all are done, rethrowing the first exception. Runs
// inline, in order, on a pool of 1, for n < 2, from inside a pool task, or
// while another thread's parallel_for holds the pool (one job at a time).
void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn);

}  // namespace data2g::pool
