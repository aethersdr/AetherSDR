#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace AetherSDR {

// Contour shuttle ring (-7..+7, reported only when it moves) as a rate input:
// tick() runs on a timer while deflected and integrates Hz/s into whole steps of
// the caller's step size, carrying the remainder. A new deflection steps once
// after kFirstStepDelaySec, longer than the ~35 ms spring overshoot on release,
// and the rate never falls below (|position| + 1) steps/s (#5928).
class ShuttleRateIntegrator {
public:
    static constexpr int kMaxPosition = 7;
    static constexpr double kFirstStepDelaySec = 0.060;

    // Normal-speed rate at each |position|, in Hz/s. Exponential so the first
    // detents creep (20 Hz/s suits CW) and full deflection sweeps a band
    // (100 kHz/s crosses 20 m in about 3.5 s).
    static double rateHzPerSec(int position)
    {
        static constexpr double kRate[kMaxPosition + 1] = {
            0.0, 20.0, 100.0, 500.0, 2'000.0, 8'000.0, 30'000.0, 100'000.0};
        return kRate[std::clamp(std::abs(position), 0, kMaxPosition)];
    }

    // Centre or a reversal drops the remainder, so it never leaks into the other
    // direction, and arms the first step.
    void setPosition(int position)
    {
        position = std::clamp(position, -kMaxPosition, kMaxPosition);
        const bool reversed = m_position != 0 && position != 0
                           && (position > 0) != (m_position > 0);
        if (position == 0 || reversed)
            m_remainderHz = 0.0;
        if (position != 0 && (m_position == 0 || reversed)) {
            m_firstStepPending = true;
            m_heldSec = 0.0;
        }
        m_position = position;
    }

    int position() const { return m_position; }

    void resetRemainder() { m_remainderHz = 0.0; }

    void reset()
    {
        m_position = 0;
        m_remainderHz = 0.0;
        m_firstStepPending = false;
        m_heldSec = 0.0;
    }

    // Advance by dtSec and return the signed number of whole stepHz steps to
    // apply. The rate is max(table, (|position| + 1) steps/s) times
    // speedMultiplier, capped at maxRateHz (lower for RIT/XIT than the VFO).
    int tick(double dtSec, int stepHz, double speedMultiplier, double maxRateHz)
    {
        if (m_position == 0 || stepHz <= 0 || !(dtSec > 0.0))
            return 0;
        const double floorHz = (std::abs(m_position) + 1) * static_cast<double>(stepHz);
        const double rate = std::min(std::max(rateHzPerSec(m_position), floorHz) * speedMultiplier,
                                     maxRateHz);
        m_remainderHz += rate * dtSec;
        m_heldSec += dtSec;
        if (m_firstStepPending && m_heldSec >= kFirstStepDelaySec) {
            m_firstStepPending = false;
            m_remainderHz += stepHz;
        }
        // The epsilon keeps binary rounding (0.04 s is not exact) from
        // turning an exact whole step into 0.999... and dropping it.
        const int steps = static_cast<int>(std::floor(m_remainderHz / stepHz + 1e-9));
        m_remainderHz -= static_cast<double>(steps) * stepHz;
        return m_position > 0 ? steps : -steps;
    }

private:
    int m_position{0};
    double m_remainderHz{0.0};
    bool m_firstStepPending{false};
    double m_heldSec{0.0};   // time since this deflection began
};

} // namespace AetherSDR
