#pragma once

#include <algorithm>

namespace AetherSDR {

enum class PassbandDragEdge {
    Both,
    Low,
    High,
};

struct PassbandEdgePair {
    int lowHz = 0;
    int highHz = 0;

    bool operator==(const PassbandEdgePair&) const = default;
};

inline double passbandDragScaleHzPerPixel(int spanHz, int pixels) noexcept
{
    return static_cast<double>(std::max(0, spanHz)) / std::max(1, pixels);
}

inline PassbandEdgePair constrainPassbandWidth(int lowHz, int highHz,
                                               int minimumWidthHz,
                                               int maximumWidthHz,
                                               PassbandDragEdge edge) noexcept
{
    const int minimum = std::max(1, minimumWidthHz);
    const int maximum = std::max(minimum, maximumWidthHz);
    const auto resize = [edge](int low, int high, int width) {
        if (edge == PassbandDragEdge::Low) {
            low = high - width;
        } else if (edge == PassbandDragEdge::High) {
            high = low + width;
        } else {
            const int centre = (low + high) / 2;
            low = centre - width / 2;
            high = low + width;
        }
        return PassbandEdgePair{low, high};
    };

    const int width = highHz - lowHz;
    if (width < minimum) {
        return resize(lowHz, highHz, minimum);
    }
    if (width > maximum) {
        return resize(lowHz, highHz, maximum);
    }
    return {lowHz, highHz};
}

} // namespace AetherSDR
