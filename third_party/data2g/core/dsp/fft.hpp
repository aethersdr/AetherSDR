// FFT, via the vendored pocketfft header.
//
// Wrapped rather than used directly so the rest of the core sees one
// small interface, and so swapping the backend later touches one file.
// See native/third_party/pocketfft/README.md for why FFT-derived values
// are tolerance-class against the Python reference rather than bitwise
// (SciPy's backend is ducc0, pocketfft's successor -- shared lineage,
// no guarantee of identical bits, and an FFT could not be bitwise
// across platforms anyway).

#pragma once

#include <algorithm>
#include <complex>
#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

// Cache a handful of FFT plans (twiddle factors) instead of rebuilding
// one on every call. Must be defined before pocketfft_hdronly.h is
// first included, which happens right below -- this is that header's
// only include site in the codebase.
//
// Off (0) by default upstream. A live-decode-loop perf capture showed
// 5.45% of total CPU time in `cfftp<double>::cfftp` -- the plan
// *constructor*, not the transform itself -- because every FFT call
// anywhere in the native code, including 67 back-to-back same-length
// calls inside a single (SSTVAE) acquire_blind() invocation, was rebuilding its
// twiddle-factor table from scratch. The cache is a thread-safe,
// mutex-protected LRU keyed by transform length (pocketfft_hdronly.h's
// own `get_plan`), so this only removes redundant setup work; the
// transform math, and so every golden vector's tolerance, is unchanged.
// Sized generously above the small, fixed set of distinct lengths this
// codebase's hot paths actually use, not tuned to it exactly -- eviction
// churn from an undersized cache would be worse than the memory a few
// extra unused slots cost.
#ifndef POCKETFFT_CACHE_SIZE
#define POCKETFFT_CACHE_SIZE 16
#endif

#include "pocketfft_hdronly.h"

namespace data2g::dsp {

using cdouble = std::complex<double>;

// Capped rather than handed hardware_concurrency() outright: this runs
// on the live decode loop's thread, alongside audio and rig I/O that
// also want CPU, so unbounded fan-out would rob those of headroom for
// a transform too small to need it. 4 is enough to matter on the
// small, fixed set of lengths this codebase's hot paths use without
// claiming a whole big machine for one FFT.
//
// Only worth passing to a *batched* pocketfft call (fft_pair/r2c_pair
// below). pocketfft parallelizes across the independent transforms
// sharing one call, not within a single one -- its own thread_count()
// divides the batch size by the transform length, so a lone vector
// (batch of 1) always collapses back to 1 thread no matter what this
// returns. Measured against pocketfft_hdronly.h's util::thread_count:
// confirmed by inspection, not by profiling a change that cannot show
// up in a profile.
inline std::size_t fft_thread_count() {
    static const std::size_t n = std::min<std::size_t>(
        4, std::max<std::size_t>(1, std::thread::hardware_concurrency()));
    return n;
}

// fft() into a caller's buffer: `in` and `out` n long, not overlapping.
inline void fft_into(const cdouble* in, cdouble* out, std::size_t n, bool forward) {
    if (n == 0) return;
    const pocketfft::shape_t shape{n};
    const pocketfft::stride_t stride{
        static_cast<std::ptrdiff_t>(sizeof(cdouble))};
    const double scale = forward ? 1.0 : 1.0 / static_cast<double>(n);
    pocketfft::c2c(shape, stride, stride, pocketfft::shape_t{0}, forward,
                   in, out, scale);
}

// `rows` transforms of length n, one batched pocketfft call (rows contiguous, n apart): the
// lines are processed several to a vector op, which a lone fft_into can't.
inline void fft_rows_into(const cdouble* in, cdouble* out, std::size_t rows, std::size_t n, bool forward) {
    if (n == 0 || rows == 0) return;
    const pocketfft::shape_t shape{rows, n};
    const pocketfft::stride_t stride{
        static_cast<std::ptrdiff_t>(n * sizeof(cdouble)), static_cast<std::ptrdiff_t>(sizeof(cdouble))};
    const double scale = forward ? 1.0 : 1.0 / static_cast<double>(n);
    pocketfft::c2c(shape, stride, stride, pocketfft::shape_t{1}, forward, in, out, scale);
}

// In-place-free complex FFT. `forward` selects the sign convention;
// the inverse is scaled by 1/n so that ifft(fft(x)) == x, matching
// numpy and scipy. A lone transform, so single-threaded by construction
// (see fft_thread_count) -- nthreads=1 here is not a missed opportunity.
inline std::vector<cdouble> fft(const std::vector<cdouble>& x, bool forward) {
    std::vector<cdouble> out(x.size());
    fft_into(x.data(), out.data(), x.size(), forward);
    return out;
}

// Two same-length complex transforms (both forward or both inverse) as
// one batched pocketfft call, so there are two independent lines for
// fft_thread_count()'s threads to actually split across -- see the note
// there. `a` and `b` must be the same length. Used by fftconvolve_valid,
// which always computes fft(a) and fft(b) together.
inline std::pair<std::vector<cdouble>, std::vector<cdouble>> fft_pair(
    const std::vector<cdouble>& a, const std::vector<cdouble>& b, bool forward) {
    const std::size_t n = a.size();
    std::pair<std::vector<cdouble>, std::vector<cdouble>> result{
        std::vector<cdouble>(n), std::vector<cdouble>(n)};
    if (n == 0) return result;
    std::vector<cdouble> buf_in(2 * n), buf_out(2 * n);
    std::copy(a.begin(), a.end(), buf_in.begin());
    std::copy(b.begin(), b.end(), buf_in.begin() + static_cast<std::ptrdiff_t>(n));
    const pocketfft::shape_t shape{2, n};
    const pocketfft::stride_t stride{
        static_cast<std::ptrdiff_t>(n * sizeof(cdouble)),
        static_cast<std::ptrdiff_t>(sizeof(cdouble))};
    const double scale = forward ? 1.0 : 1.0 / static_cast<double>(n);
    pocketfft::c2c(shape, stride, stride, pocketfft::shape_t{1}, forward,
                   buf_in.data(), buf_out.data(), scale, fft_thread_count());
    std::copy(buf_out.begin(), buf_out.begin() + static_cast<std::ptrdiff_t>(n),
              result.first.begin());
    std::copy(buf_out.begin() + static_cast<std::ptrdiff_t>(n), buf_out.end(),
              result.second.begin());
    return result;
}

// Two same-length real->complex forward transforms as one batched
// pocketfft call, for the same reason as fft_pair. `a` and `b` must be
// the same length.
inline std::pair<std::vector<cdouble>, std::vector<cdouble>> r2c_pair(
    const std::vector<double>& a, const std::vector<double>& b) {
    const std::size_t n = a.size();
    const std::size_t nc = n / 2 + 1;
    std::pair<std::vector<cdouble>, std::vector<cdouble>> result{
        std::vector<cdouble>(nc), std::vector<cdouble>(nc)};
    if (n == 0) return result;
    std::vector<double> buf_in(2 * n);
    std::copy(a.begin(), a.end(), buf_in.begin());
    std::copy(b.begin(), b.end(), buf_in.begin() + static_cast<std::ptrdiff_t>(n));
    std::vector<cdouble> buf_out(2 * nc);
    const pocketfft::shape_t shape_in{2, n};
    const pocketfft::stride_t stride_r{
        static_cast<std::ptrdiff_t>(n * sizeof(double)),
        static_cast<std::ptrdiff_t>(sizeof(double))};
    const pocketfft::stride_t stride_c{
        static_cast<std::ptrdiff_t>(nc * sizeof(cdouble)),
        static_cast<std::ptrdiff_t>(sizeof(cdouble))};
    pocketfft::r2c(shape_in, stride_r, stride_c, pocketfft::shape_t{1}, true,
                   buf_in.data(), buf_out.data(), 1.0, fft_thread_count());
    std::copy(buf_out.begin(), buf_out.begin() + static_cast<std::ptrdiff_t>(nc),
              result.first.begin());
    std::copy(buf_out.begin() + static_cast<std::ptrdiff_t>(nc), buf_out.end(),
              result.second.begin());
    return result;
}

}  // namespace data2g::dsp
