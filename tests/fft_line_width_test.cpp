#include "gui/FftLineWidth.h"

#include <array>
#include <cstdio>

int main()
{
    // The shader draws on both sides of the centerline. Pin the entire UI
    // range, including Off and subpixel widths, to device-pixel half-widths.
    constexpr std::array<float, 11> expectedHalfWidths{
        0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.25f,
        1.5f, 1.75f, 2.0f, 2.25f, 2.5f};
    int failures = 0;
    for (int step = 0; step <= 10; ++step) {
        const float fullWidth = static_cast<float>(step) * 0.5f;
        const float actual = AetherSDR::fftLineHalfWidth(fullWidth);
        if (actual != expectedHalfWidths[step]) {
            std::fprintf(stderr, "FAIL: %.1f px trace: expected half-width %.2f, got %.2f\n",
                         fullWidth, expectedHalfWidths[step], actual);
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
