#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <limits>

namespace AetherSDR::rtl {
// Acquisition-owned envelope using the fixed 2048-point Blackman-Harris FFT.
// Thresholds are dBFS per detector bin, not dBm or integrated channel power.
// The display has a different FFT size and must never drive this estimator.
// No planner, locks or allocations run here.
class RtlSquelchGate final {
public:
    static constexpr double kReferenceDb = -120.0;
    static constexpr double kStepDb = 1.2;
    static constexpr std::uint64_t kHoldFrames = 7200; // 150 ms at 48 kHz
    static constexpr std::uint64_t kStaleFrames = 4800; // missing detector: close
    static constexpr float kRampStep = 1.0f / 240.0f; // 5 ms

    void configure(bool enabled, int level, bool reset = false,
                   bool automatic = false, int marginDb = 10) noexcept
    {
        if (reset || enabled != m_enabled) {
            m_open = false; m_observed = false; m_lastAbove = 0;
            if (reset) { m_gain = enabled ? 0.0f : 1.0f; }
        }
        if (reset || automatic != m_automatic || enabled != m_enabled) { m_floor = std::numeric_limits<double>::quiet_NaN(); }
        m_automatic = automatic;
        m_marginDb = std::clamp(marginDb, 5, 20);
        m_enabled = enabled;
        if (!automatic) { m_threshold = kReferenceDb + kStepDb * std::clamp(level, 0, 100); }
    }
    void observeSpectrum(std::span<const float> bins, int low, int high,
                         std::uint64_t frame) noexcept
    {
        if (low < 0 || high < low || high >= static_cast<int>(bins.size())) {
            observe(std::numeric_limits<double>::quiet_NaN(), frame); return;
        }
        double peak = kReferenceDb;
        for (int i = low; i <= high; ++i) {
            if (!std::isfinite(bins[i])) { observe(std::numeric_limits<double>::quiet_NaN(), frame); return; }
            peak = std::max(peak, double(bins[i]));
        }
        if (m_automatic) {
            // Exclude the desired passband and window leakage. A median of
            // neighboring bin powers rejects isolated carriers. For complex
            // Gaussian noise, power is exponential: median / ln(2) estimates
            // mean power. A trimmed mean of log power is systematically low
            // and makes a peak detector hold noise open at the normal margin.
            const int first = std::max(0, low - 64);
            const int end = std::min(static_cast<int>(bins.size()), high + 65);
            std::array<double, 128> neighbors{};
            std::size_t count = 0;
            for (int i = first; i < end; ++i) {
                if (i >= low - 4 && i <= high + 4) { continue; }
                if (std::isfinite(bins[i])) { neighbors[count++] = bins[i]; }
            }
            if (!count) { observe(std::numeric_limits<double>::quiet_NaN(), frame); return; }
            std::sort(neighbors.begin(), neighbors.begin() + count);
            const double medianDb = (neighbors[(count - 1) / 2] + neighbors[count / 2]) * 0.5;
            const double floor = medianDb + 10.0 * std::log10(1.0 / std::log(2.0));
            m_floor = std::isfinite(m_floor) ? 0.9 * m_floor + 0.1 * floor : floor;
            m_threshold = m_floor + m_marginDb;
        }
        observe(peak, frame);
    }
    void observe(double peakDb, std::uint64_t frame) noexcept
    {
        if (!std::isfinite(peakDb)) { m_observed = false; m_open = false; return; }
        m_observed = true; m_lastObservation = frame;
        // Three dB of hysteresis plus a bounded hang avoids noise chatter.
        if (peakDb >= m_threshold - (m_open ? 3.0 : 0.0)) {
            m_open = true; m_lastAbove = frame;
        }
    }
    float gain(std::uint64_t frame) noexcept
    {
        if (m_enabled && (!m_observed
            || (frame >= m_lastObservation && frame - m_lastObservation > kStaleFrames)
            || (frame >= m_lastAbove && frame - m_lastAbove > kHoldFrames))) {
            m_open = false;
        }
        const float target = (!m_enabled || m_open) ? 1.0f : 0.0f;
        m_gain += std::clamp(target - m_gain, -kRampStep, kRampStep);
        return m_gain;
    }
private:
    bool m_automatic = false;
    int m_marginDb = 10;
    double m_floor = std::numeric_limits<double>::quiet_NaN();
    bool m_enabled = false;
    bool m_open = false;
    bool m_observed = false;
    double m_threshold = kReferenceDb;
    float m_gain = 1.0f;
    std::uint64_t m_lastAbove = 0;
    std::uint64_t m_lastObservation = 0;
};
} // namespace AetherSDR::rtl
