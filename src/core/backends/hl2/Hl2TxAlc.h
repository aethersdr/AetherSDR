#pragma once

// The HL2 transmit level chain as a pure, sample-counted stage: mic gain, the
// reduction-only ALC, and the hard clamp. Hl2TxDsp::processAudioBlock runs this
// same object, so hl2_tx_alc_test measures what the modulator receives.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace AetherSDR::hl2 {

class Hl2TxAlc {
public:
    struct Settings {
        bool enabled = true;
        double targetPeak = 0.85;
        double releaseSec = 0.500;
        double sampleRateHz = 24000.0;
    };
    struct Levels {
        float micPeak = 0.0f;    // after mic gain, before the ALC
        float postPeak = 0.0f;   // after the ALC and the clamp
    };

    // The gain at the END of the last block, linear. 1.0 is unity.
    [[nodiscard]] double gain() const noexcept { return m_gain; }
    void reset() noexcept { m_gain = 1.0; }

    // One decision per call (reduce to targetPeak/peak, else one-pole release,
    // never above unity), reached inside the call with no look-ahead. Release:
    // a line from the previous gain. Attack: the lower convex hull of the
    // previous gain and each sample's limit targetPeak/|x|, so no sample
    // exceeds targetPeak; an attack constant would leave a step at the clamp.
    Levels process(std::span<const float> in, double micGain,
                   const Settings& settings, std::span<float> out)
    {
        const std::size_t n = std::min(in.size(), out.size());
        Levels levels;
        for (std::size_t s = 0; s < n; ++s) {
            out[s] = static_cast<float>(in[s] * micGain);
            levels.micPeak = std::max(levels.micPeak, std::fabs(out[s]));
        }

        const double from = settings.enabled ? m_gain : 1.0;
        m_knots.clear();
        m_knots.push_back({-1.0, from});
        if (!settings.enabled) {
            // Off: unity, and the hard clamp is the only over-level backstop.
            m_gain = 1.0;
        } else if (levels.micPeak > 1e-6f) {
            const double target =
                std::min(settings.targetPeak / levels.micPeak, 1.0);
            if (target < m_gain) {
                m_gain = target;
                attackKnots(out.first(n), from, settings.targetPeak,
                            levels.micPeak);
            } else {
                const double blockSec = static_cast<double>(n)
                                      / settings.sampleRateHz;
                const double a = 1.0 - std::exp(
                    -blockSec / std::max(1e-6, settings.releaseSec));
                m_gain += a * (target - m_gain);
                m_knots.push_back({static_cast<double>(n) - 1.0, m_gain});
            }
        }

        std::size_t k = 0;
        for (std::size_t s = 0; s < n; ++s) {
            const double x = static_cast<double>(s);
            while (k + 1 < m_knots.size() && m_knots[k + 1].x < x)
                ++k;
            double g = m_gain;   // past the last knot: the decision, held
            if (k + 1 < m_knots.size()) {
                const Knot& a = m_knots[k];
                const Knot& b = m_knots[k + 1];
                g = a.g + (b.g - a.g) * (x - a.x) / (b.x - a.x);
            }
            out[s] = std::clamp(static_cast<float>(out[s] * g), -1.0f, 1.0f);
            levels.postPeak = std::max(levels.postPeak, std::fabs(out[s]));
        }
        return levels;
    }

private:
    struct Knot {
        double x;   // sample index; -1 is the previous block's last sample
        double g;
    };

    // Only a sample the previous gain would put over the target constrains the
    // path, and the path ends at the first sample carrying the block peak.
    void attackKnots(std::span<const float> pre, double from, double targetPeak,
                     float peak)
    {
        for (std::size_t s = 0; s < pre.size(); ++s) {
            const double mag = std::fabs(pre[s]);
            if (mag * from <= targetPeak)
                continue;
            const Knot c{static_cast<double>(s), targetPeak / mag};
            while (m_knots.size() >= 2) {
                const Knot& a = m_knots[m_knots.size() - 2];
                const Knot& b = m_knots.back();
                // Drop b when it lies on or above the chord a -> c.
                if ((b.x - a.x) * (c.g - b.g) - (b.g - a.g) * (c.x - b.x) > 0.0)
                    break;
                m_knots.pop_back();
            }
            m_knots.push_back(c);
            if (std::fabs(pre[s]) >= peak)
                return;
        }
    }

    double m_gain = 1.0;
    std::vector<Knot> m_knots;   // scratch, kept to avoid a per-block allocation
};

}  // namespace AetherSDR::hl2
