#pragma once

#include <QVector>

#include <algorithm>
#include <cstdlib>
#include <limits>

// Filter-preset index arithmetic, pure for testing. The applet holds the
// operator's preset list and, on a radio with a fixed set (IC-705: three IF
// filters), the radio's list; search, clamp and apply must walk the same one,
// or widths come from the wrong container and differing lengths overrun.

namespace AetherSDR {

// The index whose width is closest to `currentWidth`. -1 when the list is
// empty, which callers must treat as "no step is possible" rather than as 0.
[[nodiscard]] inline int nearestFilterWidthIndex(const QVector<int>& widths,
                                                 int currentWidth)
{
    if (widths.isEmpty())
        return -1;
    int best = 0;
    int bestDist = std::numeric_limits<int>::max();
    for (int i = 0; i < widths.size(); ++i) {
        const int dist = std::abs(currentWidth - widths[i]);
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

// Step `steps` presets up (steps > 0) or down (steps < 0), clamped to the list.
//
// Returns -1 when there is nothing to do: an empty list, zero steps, or
// already at the end while sitting exactly on a preset. A caller that treats
// -1 as an index is the bug this function exists to prevent, so it is the only
// sentinel and it is never a valid position.
[[nodiscard]] inline int steppedFilterWidthIndex(const QVector<int>& widths,
                                                 int currentWidth, int steps)
{
    if (widths.isEmpty() || steps == 0)
        return -1;
    const int idx = nearestFilterWidthIndex(widths, currentWidth);
    if (idx < 0)
        return -1;
    // Clamped against the SAME list the search walked. Bounding by a different
    // array is how a widen request came back narrower, and — where that other
    // array was empty — how std::clamp was called with lo > hi.
    const int next = std::clamp(idx + steps, 0,
                                static_cast<int>(widths.size()) - 1);
    const bool exactlyOnAPreset = (widths[idx] == currentWidth);
    if (next == idx && exactlyOnAPreset)
        return -1;
    return next;
}

}  // namespace AetherSDR
