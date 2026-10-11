#pragma once

#include <algorithm>

namespace AetherSDR {

// Where the dBm strip's arrow row starts in the FFT area: below a host's
// header (SpectrumWidget::setTopInset()), but never so low that the row
// leaves the FFT area at a short spectrum/waterfall split. Drawing and both
// hit-tests (press and hover) use it, so what is drawn is what is clicked.
constexpr int dbmArrowRowTop(int topInset, int fftHeight, int arrowHeight)
{
    return std::clamp(topInset, 0, std::max(0, fftHeight - arrowHeight));
}

} // namespace AetherSDR
