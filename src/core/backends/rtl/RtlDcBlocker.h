#pragma once

#include <cmath>
#include <complex>
#include <numbers>
#include <span>

namespace AetherSDR::rtl {

// Acquisition-thread-only, continuous complex high pass. Unlike subtracting a
// callback's mean, its response does not depend on USB block boundaries. The
// normalized DC blocker H(z) = g(1-z^-1)/(1-rz^-1) has unity Nyquist gain.
// Double state avoids coefficient cancellation at RTL sample rates.
class RtlDcBlocker final {
public:
    static constexpr double kCornerHz = 5.0;

    void configure(bool enabled, double sampleRateHz) noexcept
    {
        m_enabled = enabled && std::isfinite(sampleRateHz) && sampleRateHz > 2 * kCornerHz;
        m_pole = m_enabled ? std::exp(-2 * std::numbers::pi * kCornerHz / sampleRateHz) : 0;
        m_gain = (1 + m_pole) / 2;
        reset();
    }
    void reset() noexcept { m_previous = {}; m_output = {}; }
    void process(std::span<std::complex<float>> samples) noexcept
    {
        if (!m_enabled) { return; }
        for (std::complex<float>& sample : samples) {
            const std::complex<double> input(sample);
            m_output = m_gain * (input - m_previous) + m_pole * m_output;
            m_previous = input;
            sample = std::complex<float>(m_output);
        }
    }

private:
    bool m_enabled = false;
    double m_pole = 0;
    double m_gain = 1;
    std::complex<double> m_previous;
    std::complex<double> m_output;
};

} // namespace AetherSDR::rtl
