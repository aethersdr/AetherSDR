#include "TciIqDecimator.h"

#include <cmath>
#include <numbers>
#include <utility>

namespace AetherSDR {
namespace {
// Half-band low-pass, Blackman-windowed sinc. 95 taps: flat to 0.22 of the
// input rate, -75 dB from 0.28, so what folds back is -75 dB down everywhere
// but the outer ~6% at each edge of the decimated span.
constexpr int kTaps = 95;
constexpr int kCentre = (kTaps - 1) / 2;

struct Tap {
    int delay;
    float gain;
};

// A half-band filter is zero on every even offset from its centre except the
// centre itself, so only the odd offsets and the centre are kept.
const std::vector<Tap>& halfBandTaps()
{
    static const std::vector<Tap> taps = [] {
        std::vector<double> h(kTaps);
        double sum = 0.0;
        for (int n = 0; n < kTaps; ++n) {
            const int k = n - kCentre;
            const double sinc = k == 0 ? 0.5
                : std::sin(std::numbers::pi * k / 2.0) / (std::numbers::pi * k);
            const double x = 2.0 * std::numbers::pi * n / (kTaps - 1);
            const double window = 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
            h[n] = sinc * window;
            sum += h[n];
        }
        std::vector<Tap> kept;
        for (int n = 0; n < kTaps; ++n) {
            const int k = n - kCentre;
            if (k == 0 || (k % 2) != 0) {
                kept.push_back({n, static_cast<float>(h[n] / sum)});   // unity DC gain
            }
        }
        return kept;
    }();
    return taps;
}
} // namespace

int TciIqDecimator::outputRate(int inputRate, int targetRate)
{
    int rate = inputRate;
    while (targetRate > 0 && rate % 2 == 0 && rate / 2 >= targetRate) {
        rate /= 2;
    }
    return rate;
}

void TciIqDecimator::configure(int inputRate, int targetRate)
{
    const int out = outputRate(inputRate, targetRate);
    if (inputRate == m_inputRate && out == m_outputRate) {
        return;
    }
    m_inputRate = inputRate;
    m_outputRate = out;
    m_stages.clear();
    for (int rate = inputRate; rate > out; rate /= 2) {
        Stage stage;
        stage.history.assign(kTaps - 1, {});
        m_stages.push_back(std::move(stage));
    }
}

void TciIqDecimator::process(const float* iq, int pairs, std::vector<float>& out)
{
    out.clear();
    if (pairs <= 0) {
        return;
    }
    if (m_stages.empty()) {
        out.assign(iq, iq + 2 * static_cast<std::size_t>(pairs));
        return;
    }
    m_a.resize(static_cast<std::size_t>(pairs));
    for (int n = 0; n < pairs; ++n) {
        m_a[n] = {iq[2 * n], iq[2 * n + 1]};
    }
    for (Stage& stage : m_stages) {
        runStage(stage, m_a, m_b);
        std::swap(m_a, m_b);
    }
    out.reserve(2 * m_a.size());
    for (const auto& c : m_a) {
        out.push_back(c.real());
        out.push_back(c.imag());
    }
}

void TciIqDecimator::runStage(Stage& stage, const std::vector<std::complex<float>>& in,
                              std::vector<std::complex<float>>& out) const
{
    // History and new block side by side: output i is the filter centred so
    // that its newest input is buf[i]. Outputs fall on every second input, and
    // `phase` carries that cadence across blocks of any length, odd included.
    std::vector<std::complex<float>>& buf = stage.history;
    const std::size_t kept = buf.size();
    buf.insert(buf.end(), in.begin(), in.end());

    const auto& taps = halfBandTaps();
    out.clear();
    std::size_t i = kept + static_cast<std::size_t>(stage.phase);
    for (; i < buf.size(); i += 2) {
        std::complex<float> acc{};
        for (const Tap& t : taps) {
            acc += t.gain * buf[i - static_cast<std::size_t>(t.delay)];
        }
        out.push_back(acc);
    }
    stage.phase = static_cast<int>(i - buf.size());
    buf.erase(buf.begin(), buf.end() - static_cast<std::ptrdiff_t>(kept));
}

} // namespace AetherSDR
