#pragma once

#include <QVector>

#include <algorithm>
#include <cstdlib>
#include <limits>

// The filter-preset index arithmetic, lifted out of RxApplet so it can be
// tested without constructing a GUI.
//
// It is here because it got this wrong twice in one diff, both times by mixing
// two parallel arrays. The applet holds the OPERATOR'S configurable preset list
// and, on a radio that declares a fixed set (an IC-705 has exactly three IF
// filters), a second list belonging to the radio. Any site that searches one
// and indexes the other produces a width from the wrong container — and where
// the two differ in LENGTH, an out-of-range access.
//
// Keeping the arithmetic in one pure function is what makes "search, clamp and
// apply all walk the same list" checkable rather than a property you have to
// re-audit at every call site.

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
