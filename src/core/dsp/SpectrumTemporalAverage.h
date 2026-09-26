#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace AetherSDR {

// Acquisition-owned, one state value per real FFT bin. Construction allocates;
// frame processing and reset do not. Inputs retain their RF-bin identity until
// reset. Averaging is over emitted observations, not unsampled time between them.
class SpectrumTemporalAverage final {
public:
    static constexpr int kMsPerStep = 10;
    explicit SpectrumTemporalAverage(std::size_t bins) : m_state(bins) {}

    void reset() noexcept { m_haveAverage = false; }

    void beginFrame(int average, bool logarithmic, double elapsedSeconds) noexcept
    {
        average = std::clamp(average, 0, 100);
        if (average != m_average || logarithmic != m_logarithmic) { reset(); }
        m_average = average;
        m_logarithmic = logarithmic;
        m_seedFrame = !m_haveAverage;
        m_haveAverage = average > 0;
        // Derive weight from the actual sample interval. Changing FPS changes
        // the observations available, but not the declared decay time.
        const double exponent = average > 0
            ? -elapsedSeconds / (average * kMsPerStep / 1000.0) : 0;
        m_alpha = average > 0 ? -std::expm1(exponent) : 1;
        m_retained = average > 0 ? std::exp(exponent) : 0;
    }

    float bin(std::size_t index, float power, float db) noexcept
    {
        if (m_average == 0) { return db; } // exact disabled passthrough
        const float value = m_logarithmic ? db : power;
        float& state = m_state[index];
        // Avoid subtracting almost equal large floats on a strong-to-weak
        // transition. Keep the small retained weight even when alpha rounds
        // to one; that tail can still exceed a much weaker new observation.
        state = m_seedFrame ? value : float(m_retained * state + m_alpha * value);
        return m_logarithmic ? state : 10.0f * std::log10(std::max(state, 1e-12f));
    }

private:
    std::vector<float> m_state;
    int m_average = 0;
    bool m_logarithmic = false;
    bool m_haveAverage = false;
    bool m_seedFrame = true;
    double m_alpha = 1;
    double m_retained = 0;
};
} // namespace AetherSDR
