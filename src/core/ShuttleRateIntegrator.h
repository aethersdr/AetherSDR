#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace AetherSDR {

// Turns a spring-return shuttle ring position into tuning steps (#5928).
//
// The Contour ring reports an absolute position (-7..+7) only when it moves,
// never while held, so it is a rate input rather than a step input: the caller
// runs a timer while the ring is deflected and calls tick() on it. tick()
// integrates a speed in Hz/s into whole steps of the caller's step size and
// carries the remainder, so the top speed does not depend on the step size.
//
// Two things keep the first detents from feeling dead (measured on hardware:
// at 20 Hz/s and a 10 Hz step the first step otherwise took 0.5 s):
//   - leaving centre (or reversing) applies one step once the deflection has
//     been held for kFirstStepDelaySec, the way a jog detent would, before
//     the rate takes over. The delay is longer than the spring's snap-back
//     overshoot on release (30-35 ms through the opposite side, measured),
//     so letting go never produces a step backwards;
//   - the rate never drops below (|position| + 1) steps/s, so with a large
//     step (1 kHz) every position still moves and still differs.
//
// Pure arithmetic, no Qt: unit-tested in tests/hid_device_parser_test.cpp.
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

    // The remainder is dropped when the ring returns to centre or reverses,
    // so a fraction of a step never leaks into the opposite direction. A new
    // deflection (from centre or a reversal) arms the immediate first step.
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
