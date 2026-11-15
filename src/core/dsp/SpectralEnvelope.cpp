#include "core/dsp/SpectralEnvelope.h"

#include "core/dsp/FftwPlannerLock.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <deque>
#include <map>
#include <mutex>
#include <numbers>
#include <vector>

namespace AetherSDR {

namespace {

// Converged when the envelope is within this of every input point. A steep
// filter skirt is finer than the lifter can follow, so on a real signal the
// iteration cap is usually what ends it.
constexpr double kConvergedDb = 1.0;
constexpr int kMaxIterations = 12;
// Over-relaxation: A = V + alpha * (max(X, V) - V). Plain true envelope
// (alpha = 1) still sits ~4 dB under an 18 dB voice comb's peaks after 6
// passes and needs ~50 to close in; alpha = 3 is within ~1-1.5 dB in 12.
constexpr double kRelaxation = 3.0;
// The trace length follows the panel width, so a drag-resize walks through
// many lengths. Keep only the most recent few; older plans are destroyed
// while the lock is already held to create the new one.
constexpr std::size_t kMaxCachedLengths = 4;
// Raised-cosine lifter taper, as a fraction of the cutoff coefficient, so a
// steep filter edge does not ring.
constexpr double kTaperStart = 0.7;

// Next length FFTW transforms fast (2^a 3^b 5^c): a prime length is 5x
// slower (1913 points: 43 us vs 5.6 us at 2048).
int smoothLength(int n)
{
    for (int m = n;; ++m) {
        int r = m;
        for (int p : {2, 3, 5}) {
            while (r % p == 0) {
                r /= p;
            }
        }
        if (r == 1) {
            return m;
        }
    }
}

struct Plans {
    fftw_plan forward{nullptr};  // DCT-II  (REDFT10)
    fftw_plan inverse{nullptr};  // DCT-III (REDFT01), scaled by 1/(2M)
};

} // namespace

struct SpectralEnvelope::Impl {
    std::map<int, Plans> plans;
    std::deque<int> planOrder;  // oldest first
    std::vector<double> x;      // padded input, dB
    std::vector<double> a;      // max(x, v)
    std::vector<double> coeff;
    std::vector<double> v;      // current envelope
    std::vector<double> lifter;

    ~Impl()
    {
        if (plans.empty()) {
            return;
        }
        // Destroyed on the UI thread when a pan closes. A blocking lock here
        // would freeze the window for as long as another thread plans (tens
        // of seconds for a cold WdspChannel::open(), #5895), so if the lock
        // is busy the plans -- a few KB -- are leaked instead.
        std::unique_lock<std::mutex> lock(fftwPlannerMutex(), std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }
        for (auto& [length, p] : plans) {
            fftw_destroy_plan(p.forward);
            fftw_destroy_plan(p.inverse);
        }
    }

    const Plans* plansFor(int m)
    {
        if (auto it = plans.find(m); it != plans.end()) {
            return &it->second;
        }
        std::unique_lock<std::mutex> lock(fftwPlannerMutex(), std::try_to_lock);
        if (!lock.owns_lock()) {
            return nullptr;
        }
        // FFTW_UNALIGNED + the new-array execute: the buffers are plain
        // vectors, so no fftw_malloc/fftw_free needs the lock either.
        std::vector<double> in(static_cast<std::size_t>(m)), out(static_cast<std::size_t>(m));
        Plans p;
        p.forward = fftw_plan_r2r_1d(m, in.data(), out.data(), FFTW_REDFT10,
                                     FFTW_ESTIMATE | FFTW_UNALIGNED);
        p.inverse = fftw_plan_r2r_1d(m, in.data(), out.data(), FFTW_REDFT01,
                                     FFTW_ESTIMATE | FFTW_UNALIGNED);
        if (!p.forward || !p.inverse) {
            if (p.forward) fftw_destroy_plan(p.forward);
            if (p.inverse) fftw_destroy_plan(p.inverse);
            return nullptr;
        }
        while (plans.size() >= kMaxCachedLengths && !planOrder.empty()) {
            auto old = plans.find(planOrder.front());
            planOrder.pop_front();
            if (old != plans.end()) {
                fftw_destroy_plan(old->second.forward);
                fftw_destroy_plan(old->second.inverse);
                plans.erase(old);
            }
        }
        planOrder.push_back(m);
        return &plans.emplace(m, p).first->second;
    }
};

SpectralEnvelope::SpectralEnvelope()
    : m_impl(std::make_unique<Impl>())
{
}

SpectralEnvelope::~SpectralEnvelope() = default;

bool SpectralEnvelope::compute(std::span<const float> in, std::span<float> out,
                               double cutoffPoints)
{
    const int n = static_cast<int>(std::min(in.size(), out.size()));
    if (n < 4 || !(cutoffPoints >= 2.0)) {
        return false;
    }
    const int m = smoothLength(n);
    Impl& s = *m_impl;
    const Plans* plans = s.plansFor(m);
    if (!plans) {
        return false;
    }

    // Pad by holding the last value: DCT-II is even-symmetric at both ends,
    // so a flat extension adds no edge of its own. Non-finite points take the
    // lowest finite value so one bad bin cannot poison every coefficient.
    float lowest = std::numeric_limits<float>::max();
    for (int i = 0; i < n; ++i) {
        if (std::isfinite(in[static_cast<std::size_t>(i)])) {
            lowest = std::min(lowest, in[static_cast<std::size_t>(i)]);
        }
    }
    if (lowest == std::numeric_limits<float>::max()) {
        return false;
    }
    const auto M = static_cast<std::size_t>(m);
    s.x.resize(M);
    s.a.resize(M);
    s.coeff.resize(M);
    s.v.resize(M);
    for (int i = 0; i < n; ++i) {
        const float db = in[static_cast<std::size_t>(i)];
        s.x[static_cast<std::size_t>(i)] = std::isfinite(db) ? db : lowest;
    }
    std::fill(s.x.begin() + n, s.x.end(), s.x[static_cast<std::size_t>(n - 1)]);

    // DCT-II coefficient k is a cosine of period 2M/k points; a comb of
    // spacing `cutoffPoints` sits at k = 2M/cutoffPoints. Keep everything
    // coarser, taper into the cutoff, drop the rest. The inverse's 1/(2M)
    // normalization is folded in here.
    const double kc = 2.0 * static_cast<double>(m) / cutoffPoints;
    s.lifter.assign(M, 0.0);
    const double norm = 1.0 / (2.0 * static_cast<double>(m));
    for (std::size_t k = 0; k < M; ++k) {
        const double f = static_cast<double>(k) / kc;
        double g = 0.0;
        if (f <= kTaperStart) {
            g = 1.0;
        } else if (f < 1.0) {
            g = 0.5 * (1.0 + std::cos(std::numbers::pi * (f - kTaperStart) / (1.0 - kTaperStart)));
        }
        s.lifter[k] = g * norm;
    }

    s.a = s.x;
    for (int iter = 0; iter < kMaxIterations; ++iter) {
        fftw_execute_r2r(plans->forward, s.a.data(), s.coeff.data());
        for (std::size_t k = 0; k < M; ++k) {
            s.coeff[k] *= s.lifter[k];
        }
        fftw_execute_r2r(plans->inverse, s.coeff.data(), s.v.data());

        double worst = 0.0;
        for (int i = 0; i < n; ++i) {
            worst = std::max(worst, s.x[static_cast<std::size_t>(i)] - s.v[static_cast<std::size_t>(i)]);
        }
        if (worst < kConvergedDb) {
            break;
        }
        for (std::size_t i = 0; i < M; ++i) {
            s.a[i] = s.v[i] + kRelaxation * (std::max(s.x[i], s.v[i]) - s.v[i]);
        }
    }

    for (int i = 0; i < n; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<float>(s.v[static_cast<std::size_t>(i)]);
    }
    return true;
}

} // namespace AetherSDR
