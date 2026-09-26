#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace AetherSDR {

// Acquisition-owned estimator on a fixed RF grid. Retunes slide storage by
// whole bins; they never repeatedly interpolate the retained state. Current
// observations and the displayed estimate are sampled against that grid in
// the selected averaging domain. Construction alone allocates. No extrapolated
// or wrapped history is admitted, including outside the usable RF interval.
class SpectrumTemporalAverage final {
public:
    static constexpr int kMsPerStep = 10;
    explicit SpectrumTemporalAverage(std::size_t bins)
        : m_state(bins), m_observation(bins), m_valid(bins) {}

    void reset() noexcept { m_haveAverage = false; }

    // Retire converter-centered evidence when its RF location changes. The
    // next observation still displays the actual measured bins, never a notch.
    void invalidate(double lowHz, double highHz) noexcept
    {
        if (!m_haveAverage) { return; }
        for (std::size_t i = 0; i < m_valid.size(); ++i) {
            const double hz = m_originHz + double(i) * m_stepHz;
            if (hz >= lowHz && hz <= highHz) { m_valid[i] = false; }
        }
    }

    // Input is ascending-frequency power, or dB for logarithmic/disabled mode.
    // Output is dB on the ORIGINAL native FFT grid. Returns the number of
    // usable output bins with valid retained RF history. Newly exposed bins
    // seed their measured value; one observation cannot supply a prior average.
    std::size_t processFrame(std::span<float> values, int average, bool logarithmic,
                             double elapsedSeconds, double originHz, double stepHz,
                             std::size_t firstUsable, std::size_t endUsable) noexcept
    {
        average = std::clamp(average, 0, 100);
        if (average == 0 || m_average == 0 || logarithmic != m_logarithmic) { reset(); }
        m_average = average;
        m_logarithmic = logarithmic;
        if (average == 0) { return 0; } // exact disabled passthrough
        if (values.size() != m_state.size() || !std::isfinite(originHz)
            || !std::isfinite(stepHz) || stepHz <= 0
            || !std::isfinite(elapsedSeconds) || elapsedSeconds < 0) {
            reset();
            if (!logarithmic) {
                for (float& value : values) { value = decibels(value); }
            }
            return 0;
        }
        firstUsable = std::min(firstUsable, values.size());
        endUsable = std::clamp(endUsable, firstUsable, values.size());
        if (m_stepHz != stepHz) { reset(); }
        if (m_haveAverage) {
            const double shift = std::round((originHz - m_originHz) / stepHz);
            if (!std::isfinite(shift) || std::abs(shift) >= double(values.size())) {
                reset();
            } else {
                slide(static_cast<std::ptrdiff_t>(shift));
                m_originHz += shift * stepHz;
            }
        }
        if (!m_haveAverage) {
            std::fill(m_valid.begin(), m_valid.end(), false);
            m_originHz = originHz;
            m_stepHz = stepHz;
        }
        std::copy(values.begin(), values.end(), m_observation.begin());
        const double exponent = -elapsedSeconds / (average * kMsPerStep / 1000.0);
        const double retained = std::exp(exponent);
        const double alpha = -std::expm1(exponent);
        const double offset = (originHz - m_originHz) / stepHz;
        std::size_t reused = 0;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const auto previous = sample(m_state, double(i) + offset, 0, values.size(), true);
            const float estimate = previous ? float(retained * *previous + alpha * m_observation[i])
                                            : m_observation[i];
            values[i] = logarithmic ? estimate : decibels(estimate);
            if (previous && i >= firstUsable && i < endUsable) { ++reused; }
        }
        // Update the fixed grid only from this frame's observations. Sampling
        // an already-remapped average back into itself would diffuse carriers
        // on every fractional-bin retune, even with no elapsed sample time.
        for (std::size_t i = 0; i < m_state.size(); ++i) {
            const auto observed = sample(m_observation, double(i) - offset,
                                         firstUsable, endUsable, false);
            if (observed) {
                m_state[i] = m_valid[i] ? float(retained * m_state[i] + alpha * *observed) : *observed;
            }
            m_valid[i] = observed.has_value();
        }
        m_haveAverage = true;
        return reused;
    }

private:
    static float decibels(float power) noexcept
    { return 10.0f * std::log10(std::max(power, 1e-12f)); }

    std::optional<float> sample(const std::vector<float>& values, double position,
                                std::size_t first, std::size_t end, bool history) const noexcept
    {
        // Round only floating arithmetic noise, not a fractional RF offset.
        const double nearest = std::round(position);
        if (std::abs(position - nearest) < 1e-7) { position = nearest; }
        if (!std::isfinite(position) || position < double(first) || position >= double(end)) { return {}; }
        const std::size_t left = static_cast<std::size_t>(position);
        const double fraction = position - double(left);
        if (history && !m_valid[left]) { return {}; }
        if (fraction == 0) { return values[left]; }
        if (left + 1 >= end || (history && !m_valid[left + 1])) { return {}; }
        return float((1 - fraction) * values[left] + fraction * values[left + 1]);
    }

    void slide(std::ptrdiff_t shift) noexcept
    {
        const std::ptrdiff_t size = static_cast<std::ptrdiff_t>(m_state.size());
        if (shift > 0) {
            for (std::ptrdiff_t i = 0; i < size; ++i) {
                if (i + shift < size) {
                    m_state[i] = m_state[i + shift]; m_valid[i] = m_valid[i + shift];
                } else { m_valid[i] = false; }
            }
        } else if (shift < 0) {
            for (std::ptrdiff_t i = size; i-- > 0;) {
                if (i + shift >= 0) {
                    m_state[i] = m_state[i + shift]; m_valid[i] = m_valid[i + shift];
                } else { m_valid[i] = false; }
            }
        }
    }

    std::vector<float> m_state;
    std::vector<float> m_observation;
    std::vector<std::uint8_t> m_valid;
    int m_average = 0;
    bool m_logarithmic = false;
    bool m_haveAverage = false;
    double m_originHz = 0;
    double m_stepHz = 0;
};
} // namespace AetherSDR
