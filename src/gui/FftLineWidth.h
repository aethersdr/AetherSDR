#pragma once

namespace AetherSDR {

// FFT Line is a full width in device pixels. The panscope shader consumes
// a half-width; keeping the conversion here makes RFC #5561 regression-testable.
constexpr float fftLineHalfWidth(float fullWidth)
{
    return fullWidth * 0.5f;
}

} // namespace AetherSDR
