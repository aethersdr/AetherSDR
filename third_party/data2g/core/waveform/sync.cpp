#include "waveform/sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <tuple>

#include "dsp/dsp.hpp"
#include "dsp/fft.hpp"
#include "util/pool.hpp"
#include "util/simd.hpp"

namespace data2g::waveform {
namespace {

using config::FS;
using config::M;
using config::PREAMBLE_CP;

constexpr double TWO_PI = 2.0 * std::numbers::pi;
// FS / STEP_HZ: every grid frequency is a whole number of bins of an FFT
// whose length is a multiple of this.
constexpr std::int64_t PER = 640;
static_assert(PER * STEP_HZ == FS);

// fn(bin) for each CFO grid point, on the pool for signals of at least
// PARALLEL_MIN samples. Below it the wakeups cost more CPU than they save
// time: a StreamDetector hop (3343 samples, every 250 ms per band) went 1.2
// -> 0.55 ms at +15% CPU. Head-limited receives (6.5-8.4k) gain 2 ms of
// burst-end latency, whole-buffer acquires 2x.
constexpr std::size_t PARALLEL_MIN = 6000;

// S[k] = |sum_{r>=1} c[k + CP + rM] conj(c[k + CP + (r-1)M])| for k < n:
// summed over repeats in Python's order, repeat-major so the loop over
// starts vectorizes; the products spelled out (no std::complex NaN
// fallback). Most of a StreamDetector hop after its FFTs: SIMD clones.
DATA2G_SIMD_CLONES void repeat_stat(const cdouble* __restrict c, int repeats, std::size_t n, double* __restrict S,
                                   double* __restrict pr, double* __restrict pi) {  // pr, pi: n zeros
    for (int rr = 1; rr < repeats; ++rr) {
        const cdouble* x = c + PREAMBLE_CP + static_cast<std::size_t>(rr) * M;
        const cdouble* y = x - M;
        for (std::size_t k = 0; k < n; ++k) {
            pr[k] += x[k].real() * y[k].real() + x[k].imag() * y[k].imag();
            pi[k] += x[k].imag() * y[k].real() - x[k].real() * y[k].imag();
        }
    }
    for (std::size_t k = 0; k < n; ++k) S[k] = std::sqrt(pr[k] * pr[k] + pi[k] * pi[k]);
}
// out[j] = a[j] * b[j] with the products spelled out: std::complex's operator* carries a NaN
// fallback that stops this vectorizing, and for finite input the values are the same.
DATA2G_SIMD_CLONES void cmul(const cdouble* __restrict a, const cdouble* __restrict b, cdouble* __restrict out, std::size_t n) {
    const double* x = reinterpret_cast<const double*>(a);
    const double* y = reinterpret_cast<const double*>(b);
    double* o = reinterpret_cast<double*>(out);
    for (std::size_t j = 0; j < n; ++j) {
        const double ar = x[2 * j], ai = x[2 * j + 1], br = y[2 * j], bi = y[2 * j + 1];
        o[2 * j] = ar * br - ai * bi;
        o[2 * j + 1] = ar * bi + ai * br;
    }
}

// out[j] = a[j] * w, w one complex factor
DATA2G_SIMD_CLONES void cscale(const cdouble* __restrict a, cdouble w, cdouble* __restrict out, std::size_t n) {
    const double* x = reinterpret_cast<const double*>(a);
    double* o = reinterpret_cast<double*>(out);
    const double br = w.real(), bi = w.imag();
    for (std::size_t j = 0; j < n; ++j) {
        const double ar = x[2 * j], ai = x[2 * j + 1];
        o[2 * j] = ar * br - ai * bi;
        o[2 * j + 1] = ar * bi + ai * br;
    }
}

// Per-thread work buffers, kept between calls: a live hop is a few thousand samples and
// ~27 bins, so allocating them per bin cost more than it saved. A whole-buffer acquire's
// (megabytes) are given back after each task instead of held.
struct Scratch {
    std::vector<cdouble> prod, c, row;
    std::vector<double> p, pn, re, im;
};
constexpr std::size_t SCRATCH_KEEP = 1 << 16;  // elements per buffer worth holding

Scratch& scratch() {
    thread_local Scratch s;
    return s;
}

void release_big(Scratch& s) {
    if (s.prod.capacity() > SCRATCH_KEEP || s.row.capacity() > SCRATCH_KEEP) s = Scratch{};
}

// A serial call that would take longer than this goes on the pool whatever its size: PARALLEL_MIN is a
// desktop's break-even, but a hop that costs a fifth of a second serially (a Raspberry Pi) keeps the engine
// thread nearly all busy just listening, and the pool's wakeups are cheap beside that.
// DATA2G_PARALLEL_SERIAL_MS overrides the 40 ms.
double parallel_serial_ns() {
    static const double ns = [] {
        const char* v = std::getenv("DATA2G_PARALLEL_SERIAL_MS");
        const double ms = v ? std::atof(v) : 40.0;
        return (ms > 0 ? ms : 40.0) * 1e6;
    }();
    return ns;
}

// What a bin costs serially per sample, from the serial calls so far (an average; 0 until the first), used to
// predict the next one. Only serial calls update it, so a machine that turned out to be slow stays "slow".
std::atomic<double> serial_ns_per_unit{0.0};
std::atomic<int> per_bin_calls{0};  // the first few are cold (plans, scratch, page faults) and don't inform the estimate

void per_bin(std::size_t samples, std::size_t bins, const std::function<void(std::size_t)>& fn) {
    const double units = static_cast<double>(samples) * static_cast<double>(bins);
    if (samples >= PARALLEL_MIN || serial_ns_per_unit.load(std::memory_order_relaxed) * units > parallel_serial_ns()) {
        pool::parallel_for(bins, fn);
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < bins; ++i) fn(i);
    if (units > 0 && per_bin_calls.fetch_add(1, std::memory_order_relaxed) >= 4) {
        const double per = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / units;
        const double old = serial_ns_per_unit.load(std::memory_order_relaxed);
        serial_ns_per_unit.store(old == 0.0 ? per : 0.8 * old + 0.2 * per, std::memory_order_relaxed);
    }
}

std::size_t argmax(const double* v, std::size_t n) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < n; ++i)
        if (v[i] > v[best]) best = i;
    return best;
}

std::size_t argmax_col(const Mat<double>& S, std::size_t col) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < S.rows; ++i)
        if (S[i][col] > S[best][col]) best = i;
    return best;
}

// numpy's float64 remainder (npy_divmod): the sign of the divisor.
double py_mod(double a, double b) {
    double m = std::fmod(a, b);
    if (m != 0) {
        if ((b < 0) != (m < 0)) m += b;
    } else {
        m = std::copysign(0.0, b);
    }
    return m;
}

// |c|^2. Python's np.abs(c) ** 2 goes through hypot, 40% of this
// module's CPU in libm; c is FFT output, so tolerance-class either way.
double power(cdouble c) { return std::norm(c); }

// Python's max(0, ...) slice start, clamped into [0, n].
std::size_t clamp_to(std::int64_t i, std::size_t n) {
    return static_cast<std::size_t>(std::clamp<std::int64_t>(i, 0, static_cast<std::int64_t>(n)));
}

}  // namespace

std::vector<double> cfo_grid(double reach) {
    const double start = -reach, stop = reach + STEP_HZ / 2;
    const auto n = static_cast<std::size_t>(std::ceil((stop - start) / STEP_HZ));
    std::vector<double> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = start + static_cast<double>(i) * STEP_HZ;
    return out;
}

std::vector<cdouble> unit_template(const Band& band) {
    std::vector<cdouble> t(band.preamble_template.begin() + PREAMBLE_CP, band.preamble_template.begin() + PREAMBLE_CP + M);
    // np.linalg.norm: re.dot(re) + im.dot(im)
    double re = 0, im = 0;
    for (const auto& v : t) {
        re += v.real() * v.real();
        im += v.imag() * v.imag();
    }
    // numpy divides a complex array by a real as by complex(r, 0): x * (1 / r)
    const double inv = 1.0 / std::sqrt(re + im);
    for (auto& v : t) v = {v.real() * inv, v.imag() * inv};
    return t;
}

std::vector<cdouble> repeat_corr(std::span<const cdouble> z, std::span<const cdouble> t, double f) {
    // the template shifted by f, reversed and conjugated; the phase reduced
    // to one turn before exp (Python does not reduce: |theta| <= 19 rad)
    std::vector<cdouble> g(t.size());
    for (std::size_t k = 0; k < t.size(); ++k) {
        const double c = f * static_cast<double>(k) / FS;
        g[t.size() - 1 - k] = std::conj(t[k] * std::polar(1.0, TWO_PI * (c - std::floor(c))));
    }
    return dsp::fftconvolve_valid(z, g);
}

namespace {

// FFT(conj(reversed t)) zero-padded to L: the same for every call of a given length, and a
// StreamDetector calls at a handful of lengths, so the last few are kept.
std::shared_ptr<const std::vector<cdouble>> template_spectrum(std::span<const cdouble> t, std::size_t L) {
    struct Entry {
        std::vector<cdouble> t;
        std::size_t L;
        std::shared_ptr<const std::vector<cdouble>> G;
    };
    static std::mutex mu;
    static std::vector<Entry> cache;
    constexpr std::size_t KEEP = 8;
    {
        std::lock_guard lock(mu);
        for (const Entry& e : cache)
            if (e.L == L && std::equal(e.t.begin(), e.t.end(), t.begin(), t.end())) return e.G;
    }
    std::vector<cdouble> gp(L);
    for (std::size_t k = 0; k < t.size(); ++k) gp[k] = std::conj(t[t.size() - 1 - k]);
    auto G = std::make_shared<const std::vector<cdouble>>(dsp::fft(gp, true));
    std::lock_guard lock(mu);
    cache.push_back({std::vector<cdouble>(t.begin(), t.end()), L, G});
    if (cache.size() > KEEP) cache.erase(cache.begin());
    return G;
}

// Each f's repeat_corr row: dest(i) is where bin i's row (z.size() - t.size() + 1 long) goes, or
// null for a scratch row; use(i, row) then sees it while it is still in cache. One FFT of z and
// the cached spectrum of t, then per f an inverse FFT of the product with t's spectrum rolled by
// whole bins (sync.repeat_corrs).
void corr_rows(std::span<const cdouble> z, std::span<const cdouble> t, std::span<const double> freqs,
               const std::function<cdouble*(std::size_t)>& dest,
               const std::function<void(std::size_t, const cdouble*)>& use) {
    const std::size_t m = t.size();
    if (z.size() < m) throw std::invalid_argument("repeat_corrs: signal shorter than the template");
    const std::size_t n = z.size() + m - 1, n_out = z.size() - m + 1;
    const std::size_t L = PER * dsp::next_fast_len((n + PER - 1) / PER), bins_per_step = L / PER;
    std::vector<cdouble> zp(L);
    std::copy(z.begin(), z.end(), zp.begin());
    const std::vector<cdouble> Z = dsp::fft(zp, true);
    const auto Gp = template_spectrum(t, L);
    const std::vector<cdouble>& G0 = *Gp;
    // Bins in tasks of B: the inverse FFTs of a task are one batched call. Only while a task's
    // buffers stay cache-sized; a whole-buffer acquire does one bin per task, as before.
    // (Measured: 4 bins per call is 15-30% faster up to ~2 s of signal; above that the pool has too few
    // tasks and the buffers fall out of cache, 1.3-1.5x slower on 4 threads.)
    constexpr std::size_t BATCH = 4, BATCH_MAX_L = 20480;
    const std::size_t B = L <= BATCH_MAX_L ? BATCH : 1, nb = freqs.size(), n_tasks = (nb + B - 1) / B;
    per_bin(z.size(), n_tasks, [&](std::size_t task) {
        Scratch& s = scratch();
        const std::size_t i0 = task * B, rows = std::min(B, nb - i0);
        s.prod.resize(rows * L);
        s.c.resize(rows * L);
        std::int64_t ks[BATCH];
        for (std::size_t r = 0; r < rows; ++r) {
            const auto k = static_cast<std::int64_t>(std::llround(freqs[i0 + r] / STEP_HZ));
            ks[r] = k;
            const auto shift = static_cast<std::size_t>(((k * static_cast<std::int64_t>(bins_per_step)) % static_cast<std::int64_t>(L)
                                                         + static_cast<std::int64_t>(L)) % static_cast<std::int64_t>(L));
            cdouble* prod = s.prod.data() + r * L;
            cmul(Z.data(), G0.data() + (L - shift), prod, shift);  // np.roll(G0, shift)
            cmul(Z.data() + shift, G0.data(), prod + shift, L - shift);
        }
        dsp::fft_rows_into(s.prod.data(), s.c.data(), rows, L, false);
        for (std::size_t r = 0; r < rows; ++r) {
            // exp(-2j pi f (M-1) / FS) = exp(-2j pi k (M-1) / PER): an exact turn
            std::int64_t q = (ks[r] * static_cast<std::int64_t>(m - 1)) % PER;
            if (q < 0) q += PER;
            const cdouble ph = std::polar(1.0, -TWO_PI * static_cast<double>(q) / PER);
            cdouble* row = dest(i0 + r);
            if (!row) {
                s.row.resize(n_out);
                row = s.row.data();
            }
            cscale(s.c.data() + r * L + (m - 1), ph, row, n_out);
            if (use) use(i0 + r, row);
        }
        release_big(s);
    });
}

}  // namespace

Mat<cdouble> repeat_corrs(std::span<const cdouble> z, std::span<const cdouble> t, std::span<const double> freqs) {
    Mat<cdouble> out(freqs.size(), z.size() >= t.size() ? z.size() - t.size() + 1 : 0);
    corr_rows(z, t, freqs, [&](std::size_t i) { return out[i]; }, nullptr);
    return out;
}

double noise_level(const Mat<cdouble>& C) {
    double lv = INFINITY;
    for (std::size_t i = 0; i < C.rows; ++i) {
        // re^2 + im^2: np.abs(C) ** 2 to an ulp, without a hypot per output
        std::vector<double> p(C.cols);
        for (std::size_t j = 0; j < p.size(); ++j) p[j] = std::norm(C[i][j]);
        lv = std::min(lv, dsp::quantile(std::move(p), NOISE_QUANTILE) / -std::log(1 - NOISE_QUANTILE));
    }
    return lv;
}

RawStat raw_stat(std::span<const cdouble> z, const Band& band, double reach, int repeats,
                 std::optional<std::size_t> levels_from, bool keep_outs) {
    if (repeats <= 0) repeats = band.spec->preamble_repeats;
    const std::vector<cdouble> t = unit_template(band);
    RawStat r;
    r.freqs = cfo_grid(reach);
    const auto n_out_s = static_cast<std::int64_t>(z.size()) - PREAMBLE_CP - static_cast<std::int64_t>(repeats) * M + 1;
    if (n_out_s < 1) throw std::invalid_argument("raw_stat: signal shorter than a preamble");
    const auto n_out = static_cast<std::size_t>(n_out_s);
    std::vector<double> all(r.freqs);
    const auto& refs = band.spec->name == "w48" ? NOISE_REF_HZ_W48 : NOISE_REF_HZ;  // sync.noise_ref_hz
    all.insert(all.end(), refs.begin(), refs.end());
    const std::size_t nf = r.freqs.size(), nc = z.size() - t.size() + 1;
    r.S = Mat<double>(nf, n_out);
    r.q.resize(all.size());
    if (keep_outs) r.outs = Mat<cdouble>(nf, nc);
    const double expo_mean = -std::log(1 - NOISE_QUANTILE);  // quantile -> mean of an exponential
    // Per bin, in the pass that makes the row (still in cache): its noise level, and for the
    // searched bins the statistic. A searched bin's row is written straight into outs.
    corr_rows(
        z, t, all, [&](std::size_t i) { return keep_outs && i < nf ? r.outs[i] : nullptr; },
        [&](std::size_t i, const cdouble* c) {
            Scratch& s = scratch();
            s.p.resize(nc);
            std::vector<double>& p = s.p;
            for (std::size_t j = 0; j < nc; ++j) p[j] = power(c[j]);
            double q;
            if (!levels_from) {
                q = dsp::quantile(p, NOISE_QUANTILE) / expo_mean;
            } else {
                s.pn.assign(p.begin() + static_cast<std::ptrdiff_t>(std::min(*levels_from, p.size())), p.end());
                std::vector<double>& pn = s.pn;
                const auto k = static_cast<std::size_t>(NOISE_QUANTILE * (static_cast<double>(pn.size()) - 1));
                std::nth_element(pn.begin(), pn.begin() + static_cast<std::ptrdiff_t>(k), pn.end());
                q = pn[k] / expo_mean;
            }
            r.q[i] = std::max(q, 1e-12 * (dsp::pairwise_sum(p) / static_cast<double>(p.size())) + 1e-300);  // silence (tests)
            if (i >= nf) return;
            // each window against the one before it, summed over repeats in
            // Python's order; repeat-major so the loop over starts vectorizes.
            // c[a + M] * conj(c[a]) spelled out: the same products without
            // std::complex's NaN fallback
            s.re.assign(n_out, 0.0);
            s.im.assign(n_out, 0.0);
            repeat_stat(c, repeats, n_out, r.S[i], s.re.data(), s.im.data());
        });
    return r;
}

// sync._level: the lowest bin's level, but not under the NOISE_REF_HZ
// bins' (last) geometric mean (noise that isn't white: FM audio)
static double level_of(std::span<const double> q) {
    const std::size_t n = q.size();
    return std::max(*std::min_element(q.begin(), q.end()),
                    std::sqrt(q[n - 1] * q[n - 2]) * std::pow(10.0, -LEVEL_FLOOR_DB / 10));
}

std::pair<Mat<double>, std::vector<double>> detection_stat(std::span<const cdouble> z, const Band& band, double reach,
                                                           int repeats) {
    RawStat r = raw_stat(z, band, reach, repeats);
    const double q = level_of(r.q);
    for (double& v : r.S.data) v /= q;
    return {std::move(r.S), std::move(r.freqs)};
}

std::size_t first_path(std::span<const double> power, std::size_t peak, int search, double frac, bool cyclic) {
    const auto n = static_cast<std::int64_t>(power.size());
    const double thr = frac * power[peak];
    for (int d = search; d > 0; --d) {
        std::int64_t i = static_cast<std::int64_t>(peak) - d;
        if (cyclic) {
            i = ((i % n) + n) % n;
        } else if (i < 1 || i + 1 >= n) {
            continue;
        }
        const auto u = static_cast<std::size_t>(i);
        const auto lo = static_cast<std::size_t>(((i - 1) % n + n) % n), hi = static_cast<std::size_t>((i + 1) % n);
        if (power[u] >= thr && power[u] >= power[lo] && power[u] >= power[hi]) return u;
    }
    return peak;
}

std::pair<std::int64_t, double> refine(std::span<const cdouble> z, const Band& band, std::int64_t n, double f) {
    const std::vector<cdouble> t = unit_template(band);
    const int R = band.spec->preamble_repeats;
    const std::int64_t n_pre = band.preamble_samples();
    const std::int64_t lo = std::max<std::int64_t>(0, n - config::FIRST_PATH_SEARCH - 8);
    const std::int64_t hi = std::min<std::int64_t>(static_cast<std::int64_t>(z.size()) - n_pre, n + 8);
    if (hi <= lo) return {n, f};
    const std::vector<cdouble> c = repeat_corr(z.subspan(static_cast<std::size_t>(lo), static_cast<std::size_t>(hi + n_pre - lo)), t, f);
    std::vector<double> prof(static_cast<std::size_t>(hi - lo + 1));
    for (std::size_t m = 0; m < prof.size(); ++m) {
        double acc = 0;
        for (int r = 0; r < R; ++r) acc += power(c[m + PREAMBLE_CP + static_cast<std::size_t>(r) * M]);
        prof[m] = acc;
    }
    const std::int64_t start = lo + static_cast<std::int64_t>(first_path(prof, argmax(prof.data(), prof.size())));
    std::vector<cdouble> d(static_cast<std::size_t>(R - 1));
    const auto base = static_cast<std::size_t>(start - lo + PREAMBLE_CP);
    for (std::size_t r = 1; r < static_cast<std::size_t>(R); ++r) d[r - 1] = c[base + r * M] * std::conj(c[base + (r - 1) * M]);
    const cdouble s = dsp::pairwise_sum(d);
    if (std::abs(s) == 0) return {start, f};
    // c_f removes f within a window only, so successive outputs advance by
    // the whole CFO, measured modulo FS / M; the grid picks the alias
    const double period = static_cast<double>(FS) / M;
    const double res = py_mod(std::arg(s) / TWO_PI * period - f + period / 2, period) - period / 2;
    return {start, f + res};
}

std::vector<std::size_t> crossings(std::span<const double> D, double threshold, std::size_t span, std::size_t limit) {
    std::vector<std::size_t> out;
    std::size_t n = 0;
    while (out.size() < limit) {
        std::size_t c = n;
        while (c < D.size() && !(D[c] >= threshold)) ++c;
        if (c >= D.size()) break;
        out.push_back(c + argmax(D.data() + c, std::min(span, D.size() - c)));
        n = c + span;
    }
    return out;
}

Acquisition acquire(std::span<const cdouble> z, const Band& band, std::optional<double> threshold_in, double reach,
                    std::optional<std::pair<std::int64_t, std::int64_t>> search, const Mat<double>* S_in) {
    const double threshold = threshold_in ? *threshold_in : band.preamble_threshold();
    const std::size_t n_pre = static_cast<std::size_t>(band.preamble_samples());
    if (z.size() < n_pre + 2 * M) throw SyncError("signal too short");
    Mat<double> S_own;  // a caller's S is only copied if the search window masks it
    const Mat<double>* Sp = &S_own;
    std::vector<double> freqs;
    if (S_in) {
        Sp = S_in;
        freqs = cfo_grid(reach);
        if (S_in->rows != freqs.size()) throw std::invalid_argument("acquire: S has rows for a different CFO grid");
    } else {
        std::tie(S_own, freqs) = detection_stat(z, band, reach);
    }
    if (search) {
        if (Sp != &S_own) {
            S_own = *Sp;
            Sp = &S_own;
        }
        Mat<double>& S = S_own;
        const auto cols = static_cast<std::int64_t>(S.cols);
        const std::int64_t s0 = std::max<std::int64_t>(0, search->first), s1 = std::min(cols, search->second);
        if (s1 - s0 < 1)
            throw SyncError("empty search window (" + std::to_string(search->first) + ", " + std::to_string(search->second) + ")");
        for (std::size_t i = 0; i < S.rows; ++i)
            for (std::int64_t j = 0; j < cols; ++j)
                if (j < s0 || j >= s1) S[i][static_cast<std::size_t>(j)] = -1.0;
    }
    const Mat<double>& S = *Sp;
    std::vector<double> D(S.cols, -INFINITY);
    for (std::size_t i = 0; i < S.rows; ++i)
        for (std::size_t j = 0; j < S.cols; ++j) D[j] = std::max(D[j], S[i][j]);
    const double best = D.empty() ? -INFINITY : *std::max_element(D.begin(), D.end());
    if (best < threshold) {
        char msg[96];
        std::snprintf(msg, sizeof msg, "no preamble found (peak %.1f < %g)", best, threshold);
        throw SyncError(msg);
    }
    const std::vector<std::size_t> peaks = crossings(D, threshold, n_pre, 1 + TIME_ALTERNATIVES);
    std::size_t n = peaks[0];
    const std::size_t i = argmax_col(S, n);
    Acquisition acq;
    acq.metric = S[i][n];
    std::tie(acq.preamble_start, acq.freq_offset) = refine(z, band, static_cast<std::int64_t>(n), freqs[i]);

    // runner-ups: other CFO bins at this start more than a grid step either
    // side of the winner's, then the later crossings
    std::vector<double> col(S.rows);
    for (std::size_t r = 0; r < S.rows; ++r) col[r] = S[r][n];
    const auto blank = [&](std::size_t j) {
        for (std::size_t r = j < 2 ? 0 : j - 2; r < std::min(col.size(), j + 3); ++r) col[r] = -1;
    };
    blank(i);
    for (int a = 0; a < ALTERNATIVES; ++a) {
        const std::size_t j = argmax(col.data(), col.size());
        if (col[j] < threshold) break;
        acq.alternatives.push_back(refine(z, band, static_cast<std::int64_t>(n), freqs[j]));
        blank(j);
    }
    for (std::size_t p = 1; p < peaks.size(); ++p) {
        n = peaks[p];
        acq.alternatives.push_back(refine(z, band, static_cast<std::int64_t>(n), freqs[argmax_col(S, n)]));
    }
    return acq;
}

StreamDetector::StreamDetector(const Band& b, double r)
    : band(b), reach(r), span(PREAMBLE_CP + static_cast<std::int64_t>(b.spec->preamble_repeats) * M) {
    reset();
}

void StreamDetector::reset() {
    const std::size_t n = cfo_grid(reach).size();
    tail.clear();
    S_.assign(n, {});
    C_.assign(n, {});
    s_off_ = c_off_ = 0;
    s0 = c0 = fed = 0;
    levels.clear();
}

void StreamDetector::restart_at(std::int64_t pos) {
    reset();
    fed = pos;
}

StreamDetector::Outputs StreamDetector::outputs_from(std::int64_t start) const {
    Outputs o{std::max(c0, start), {}, 0};
    const std::size_t k = clamp_to(o.start - c0, C(0).size());
    o.cols = C(0).size() - k;
    o.rows.reserve(bins());
    for (std::size_t i = 0; i < bins(); ++i) o.rows.push_back(C(i).data() + k);
    return o;
}

void StreamDetector::skip_to(std::int64_t pos) {
    tail.clear();
    for (auto& row : S_) row.clear();
    for (auto& row : C_) row.clear();
    s_off_ = c_off_ = 0;
    s0 = c0 = fed = pos;
}

void StreamDetector::feed(std::span<const cdouble> z_new) {
    if (S(0).empty() && tail.empty()) s0 = fed;
    fed += static_cast<std::int64_t>(z_new.size());
    const auto added = static_cast<std::int64_t>(z_new.size());
    std::vector<cdouble> z(std::move(tail));
    z.insert(z.end(), z_new.begin(), z_new.end());
    const auto len = static_cast<std::int64_t>(z.size());
    if (len < span + M) {
        tail = std::move(z);
        return;
    }
    // the level from the new outputs, at least 2000 (0.25 s) of the latest
    const auto levels_from = static_cast<std::size_t>(std::max<std::int64_t>(0, len - M + 1 - std::max<std::int64_t>(added, 2000)));
    RawStat r = raw_stat(z, band, reach, 0, levels_from, true);
    const std::int64_t zs = fed - len;  // stream index of z[0], so of c[0]
    if (C(0).empty()) c0 = zs;
    const std::int64_t have = c0 + static_cast<std::int64_t>(C(0).size());  // outputs overlap the last chunk's
    const std::size_t skip = clamp_to(have - zs, r.outs.cols);
    for (std::size_t i = 0; i < bins(); ++i) {
        C_[i].insert(C_[i].end(), r.outs[i] + skip, r.outs[i] + r.outs.cols);
        S_[i].insert(S_[i].end(), r.S[i], r.S[i] + r.S.cols);
    }
    levels.push_back(std::move(r.q));
    if (levels.size() > static_cast<std::size_t>(CHUNKS)) levels.erase(levels.begin());
    tail.assign(z.begin() + static_cast<std::ptrdiff_t>(r.S.cols), z.end());
}

namespace {
template <typename T>
void drop_front(std::vector<std::vector<T>>& rows, std::size_t& off, std::size_t k) {
    off += k;
    if (off <= rows[0].size() / 2) return;
    for (auto& row : rows) row.erase(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(off));
    off = 0;
}
}  // namespace

void StreamDetector::trim(std::int64_t start) {
    const std::size_t k = clamp_to(start - s0, S(0).size());
    drop_front(S_, s_off_, k);
    s0 += static_cast<std::int64_t>(k);
    const std::size_t kc = clamp_to(start - c0, C(0).size());
    drop_front(C_, c_off_, kc);
    c0 += static_cast<std::int64_t>(kc);
}

std::optional<double> StreamDetector::level() const {
    if (levels.empty()) return std::nullopt;
    std::vector<double> v(levels.size()), med(levels[0].size());
    for (std::size_t b = 0; b < levels[0].size(); ++b) {
        for (std::size_t k = 0; k < levels.size(); ++k) v[k] = levels[k][b];
        std::sort(v.begin(), v.end());
        const std::size_t h = v.size() / 2;
        med[b] = v.size() % 2 ? v[h] : (v[h - 1] + v[h]) / 2;
    }
    return level_of(med);
}

Mat<double> StreamDetector::stat(std::int64_t lo, std::int64_t hi) const {
    Mat<double> out(bins(), static_cast<std::size_t>(std::max<std::int64_t>(0, hi - lo)), -1.0);
    const std::int64_t a = std::max(lo, s0), b = std::min(hi, s0 + static_cast<std::int64_t>(S(0).size()));
    const auto q = level();
    if (b > a && q) {
        for (std::size_t i = 0; i < bins(); ++i) {
            const double* src = S(i).data() + (a - s0);
            double* dst = out[i] + (a - lo);
            for (std::int64_t j = 0; j < b - a; ++j) dst[j] = src[j] / *q;
        }
    }
    return out;
}

}  // namespace data2g::waveform
