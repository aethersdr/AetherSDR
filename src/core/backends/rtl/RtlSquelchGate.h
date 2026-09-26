#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace AetherSDR::rtl {
// Acquisition-owned envelope. Detector and display both consume the existing
// 2048-point Blackman-Harris FFT, normalized by FFT size: dBFS/bin, not dBm
// or integrated channel power. No planner, locks or allocations run here.
class RtlSquelchGate final {
public:
    static constexpr double kReferenceDb = -120.0;
    static constexpr double kStepDb = 1.2;
    static constexpr std::uint64_t kHoldFrames = 7200; // 150 ms at 48 kHz
    static constexpr std::uint64_t kStaleFrames = 4800; // missing detector: close
    static constexpr float kRampStep = 1.0f / 240.0f; // 5 ms

    void configure(bool enabled, int level, bool reset = false) noexcept
    {
        if (reset || enabled != m_enabled) {
            m_open = false; m_observed = false; m_lastAbove = 0;
            if (reset) { m_gain = enabled ? 0.0f : 1.0f; }
        }
        m_enabled = enabled;
        m_threshold = kReferenceDb + kStepDb * std::clamp(level, 0, 100);
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
    bool m_enabled = false;
    bool m_open = false;
    bool m_observed = false;
    double m_threshold = kReferenceDb;
    float m_gain = 1.0f;
    std::uint64_t m_lastAbove = 0;
    std::uint64_t m_lastObservation = 0;
};
} // namespace AetherSDR::rtl
