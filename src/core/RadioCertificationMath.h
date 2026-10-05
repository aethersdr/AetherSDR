#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace AetherSDR::certmath {

// Measurement primitives for RadioCertification, in a header so they're
// testable. tonePower() reports power at whatever bin the caller's `fs`
// implies; tests pin that a wrong `fs` moves the probe off the tone, which is
// why callers must read the rate from the capture.

inline constexpr double kPi = 3.14159265358979323846;

// Correlate a real audio buffer against one frequency. Used instead of a full
// FFT because we are asking one question about one known frequency.
//
// `fs` MUST be the rate the samples were actually captured at. Passing a
// constant here is the defect documented in docs/HERMES.md 1.9: the probe lands on
// hz*(fsActual/fs), reads the noise floor, and the caller concludes "no signal"
// from what is really "looked in the wrong place".
inline double tonePower(const std::vector<float>& mono, double hz, double fs)
{
    if (mono.empty() || fs <= 0.0)
        return 0.0;
    std::complex<double> acc{0.0, 0.0};
    const double w = -2.0 * kPi * hz / fs;
    for (std::size_t n = 0; n < mono.size(); ++n) {
        const double ph = w * static_cast<double>(n);
        acc += static_cast<double>(mono[n])
             * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return std::abs(acc) / static_cast<double>(mono.size());
}

inline double rms(const std::vector<float>& mono)
{
    if (mono.empty())
        return 0.0;
    double acc = 0.0;
    for (const float v : mono)
        acc += static_cast<double>(v) * static_cast<double>(v);
    return std::sqrt(acc / static_cast<double>(mono.size()));
}

inline double db(double v) { return 20.0 * std::log10(std::max(1e-12, v)); }

// Strongest bin within +/- `spanHz` of `hz`; use instead of tonePower() when
// the tone's exact frequency isn't ours. tonePower() is coherent over the whole
// buffer (1.5 s ≈ 0.67 Hz bins), so a 1 ppm dial error at 10 MHz (~10 Hz)
// puts an off-air reference like WWV fifteen bins away and it reads as noise.
inline double tonePowerNear(const std::vector<float>& mono, double hz,
                            double fs, double spanHz, double stepHz = 1.0)
{
    if (mono.empty() || fs <= 0.0 || stepHz <= 0.0)
        return 0.0;
    double best = 0.0;
    for (double f = hz - spanHz; f <= hz + spanHz; f += stepHz)
        best = std::max(best, tonePower(mono, f, fs));
    return best;
}

// ---- control-domain probes (CERTIFICATION.md 1.41) ----

// Values to write across a published 0..maximum control. Every step when the
// range is small (COMP 0-10 snapped on two of its eleven), else both ends, their
// neighbours and the quarters. The ends are always in: the on-at-0 squelch
// defect lived only at 0.
inline std::vector<int> domainProbeValues(int maximum)
{
    if (maximum <= 0)
        return {0};
    std::vector<int> v;
    if (maximum <= 10) {
        for (int i = 0; i <= maximum; ++i)
            v.push_back(i);
        return v;
    }
    v = {0, 1, maximum / 4, maximum / 2, (3 * maximum) / 4, maximum - 1, maximum};
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// Where a written value showed up in the model, sampled from the write on.
// A model that confirms from the radio agrees only after the readback; an
// optimistic one agrees at once and a wrong readback then moves it. Held means
// it agreed and never left: one landing elsewhere is enough, because the next
// write is built from it. -1 for "never".
struct Readback {
    int agreedAt = -1;
    int departedAt = -1;
    [[nodiscard]] bool held() const { return agreedAt >= 0 && departedAt < 0; }
};

inline Readback readbackOf(const std::vector<int>& samples, int written)
{
    Readback r;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (r.agreedAt < 0) {
            if (samples[i] == written)
                r.agreedAt = static_cast<int>(i);
        } else if (samples[i] != written) {
            r.departedAt = static_cast<int>(i);
            break;
        }
    }
    return r;
}

// ---- coupled-control convergence (CERTIFICATION.md 1.42) ----

struct TimedSample {
    int ms = 0;     // since the write
    int value = 0;
};

// How long a coupled control may take to show what the radio did to it. The
// IC-7300MK2 answers a read queued 60 ms behind the write in ~0.2 s, and polls
// its front end every 3 s; 0.5 s sits between them. A poll can land early, so
// one transition proves little (CERTIFICATION.md 1.42).
inline constexpr int kCoupledSettleBudgetMs = 500;

// When a control coupled to the one written reached the value it finally held:
// the time of the first sample from which every later sample agrees with the
// last. -1 for no samples. A radio that changes the other control without
// reporting it converges at the next periodic poll; a read after the write
// converges in a round trip.
inline int settledAtMs(const std::vector<TimedSample>& samples)
{
    if (samples.empty())
        return -1;
    const int last = samples.back().value;
    int settled = samples.back().ms;
    for (std::size_t i = samples.size(); i-- > 0;) {
        if (samples[i].value != last)
            break;
        settled = samples[i].ms;
    }
    return settled;
}

// ---- squelch gate (CERTIFICATION.md 1.43) ----

// A gate is closed when audio that was audible with it open drops this far.
// Far inside the measured gap: an Icom closes to digital silence, about
// -200 dBFS against -17 dBFS open.
inline constexpr double kGateClosedDropDb = 20.0;

inline bool gateClosed(double openDb, double sampleDb)
{
    return sampleDb < openDb - kGateClosedDropDb;
}

// How far the SQL line may sit from the carrier the radio's gate closed on.
// The MK2's measured record fits within 1.6 dB below S9; Flex's scale missed the
// same gates by 7-10 dB at low levels (#6180). Above S9 the MK2 sits 6-10 dB
// under its own record, so a strong carrier is reported, not trusted.
inline constexpr double kSquelchLineToleranceDb = 6.0;

// S9 on HF, 50 µV into 50 Ω. Above it the MK2's measured line and Flex's
// -160 + level miss the gate by similar amounts, so a carrier there cannot
// tell a right scale from a wrong one.
inline constexpr double kS9Dbm = -73.0;

// The lowest level in [lo, hi] at which closedAt() is true, assuming the gate is
// monotonic in level; -1 when it is still open at hi. closedAt is a live
// measurement, called once per probe.
template <typename ClosedAt>
int lowestClosedLevel(int lo, int hi, ClosedAt&& closedAt)
{
    if (lo > hi || !closedAt(hi))
        return -1;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (closedAt(mid))
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

// Strongest bin within +/- spanHz of targetHz, on a pan whose bins spread
// evenly over [lowHz, highHz). NaN when the window is off the pan or holds no
// finite bin. The bins are on the pan's own dBm axis, where the SQL line is drawn.
inline double panPeakNear(const std::vector<float>& bins, double lowHz, double highHz,
                          double targetHz, double spanHz)
{
    double best = std::nan("");
    if (bins.empty() || !(highHz > lowHz))
        return best;
    const double binHz = (highHz - lowHz) / static_cast<double>(bins.size());
    const auto toBin = [&](double hz) {
        return static_cast<long long>(std::floor((hz - lowHz) / binHz));
    };
    const long long first = std::max<long long>(0, toBin(targetHz - spanHz));
    const long long last = std::min<long long>(static_cast<long long>(bins.size()) - 1,
                                               toBin(targetHz + spanHz));
    for (long long i = first; i <= last; ++i) {
        const double v = bins[static_cast<std::size_t>(i)];
        if (std::isfinite(v) && !(v <= best))
            best = v;
    }
    return best;
}

// NaN when nothing finite is left.
inline double median(std::vector<double> values)
{
    values.erase(std::remove_if(values.begin(), values.end(),
                                [](double v) { return !std::isfinite(v); }),
                 values.end());
    if (values.empty())
        return std::nan("");
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n % 2 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

}  // namespace AetherSDR::certmath
