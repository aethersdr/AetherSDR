#include "StereoBiquad.h"

namespace AetherSDR {

void StereoBiquad::setCoefficients(Biquad::Type type, double sampleRateHz,
                                   double centerHz, double Q,
                                   double gainDb) noexcept
{
    // Two trig evaluations rather than one.  The redundancy is
    // intentional: keeping `Biquad` self-contained (no friend hatch
    // to copy coefficients in) keeps the primitive's API tight, and
    // setCoefficients() runs once per parameter version bump — not
    // per sample — so the duplicate cookbook math is below noise.
    m_left.setCoefficients(type, sampleRateHz, centerHz, Q, gainDb);
    m_right.setCoefficients(type, sampleRateHz, centerHz, Q, gainDb);
}

void StereoBiquad::processInterleaved(float* lr, int frames) noexcept
{
    for (int i = 0; i < frames; ++i) {
        lr[i * 2]     = m_left .process(lr[i * 2]);
        lr[i * 2 + 1] = m_right.process(lr[i * 2 + 1]);
    }
}

void StereoBiquad::reset() noexcept
{
    m_left.reset();
    m_right.reset();
}

} // namespace AetherSDR
