#pragma once

// The HL2 transmit level chain as a pure, sample-counted stage: mic gain, the
// reduction-only ALC, and the hard clamp. Hl2TxDsp::processAudioBlock runs this
// same object, so a test of it measures what the modulator receives.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

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

    // The gain the last block was levelled with, linear. 1.0 is unity.
    [[nodiscard]] double gain() const noexcept { return m_gain; }
    void reset() noexcept { m_gain = 1.0; }

    // Levels `in` into `out` (same length). One gain decision per call, from
    // the call's peak: straight to targetPeak/peak when that reduces, a
    // one-pole release toward it (never above unity) when it does not. Every
    // output sample is at or below targetPeak, so the clamp is a backstop for
    // the ALC-off path.
    Levels process(std::span<const float> in, double micGain,
                   const Settings& settings, std::span<float> out)
    {
        const std::size_t n = std::min(in.size(), out.size());
        Levels levels;
        for (std::size_t s = 0; s < n; ++s) {
            out[s] = static_cast<float>(in[s] * micGain);
            levels.micPeak = std::max(levels.micPeak, std::fabs(out[s]));
        }

        if (!settings.enabled) {
            // Off: unity, and the hard clamp is the only over-level backstop.
            m_gain = 1.0;
        } else if (levels.micPeak > 1e-6f) {
            const double target =
                std::min(settings.targetPeak / levels.micPeak, 1.0);
            if (target < m_gain) {
                m_gain = target;
            } else {
                const double blockSec = static_cast<double>(n)
                                      / settings.sampleRateHz;
                const double a = 1.0 - std::exp(
                    -blockSec / std::max(1e-6, settings.releaseSec));
                m_gain += a * (target - m_gain);
            }
        }

        for (std::size_t s = 0; s < n; ++s) {
            out[s] = std::clamp(static_cast<float>(out[s] * m_gain),
                                -1.0f, 1.0f);
            levels.postPeak = std::max(levels.postPeak, std::fabs(out[s]));
        }
        return levels;
    }

private:
    double m_gain = 1.0;
};

}  // namespace AetherSDR::hl2
