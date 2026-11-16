#pragma once

#include <QList>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace AetherSDR::TxFilterEdges {

// Fits a wanted TX filter to what a radio can set. A radio whose
// RadioCapabilities list fixed edges (txFilterLowEdgesHz / txFilterHighEdgesHz,
// ascending; empty means continuous) gets the nearest allowed edge on each
// side. The result is at least `minWidthHz` wide whenever the edges allow it:
// a high edge that lands too close moves up to the first one that does not.
// Pure, so the table is testable without a radio.
[[nodiscard]] inline int nearest(int wantHz, const QList<int>& edges)
{
    if (edges.isEmpty()) {
        return wantHz;
    }
    return *std::min_element(edges.cbegin(), edges.cend(), [wantHz](int a, int b) {
        return std::abs(a - wantHz) < std::abs(b - wantHz);
    });
}

[[nodiscard]] inline std::pair<int, int> snap(int lowHz, int highHz, const QList<int>& lowEdges,
                                              const QList<int>& highEdges, int minWidthHz)
{
    const int lo = nearest(lowHz, lowEdges);
    int hi = nearest(highHz, highEdges);
    if (hi - lo < minWidthHz) {
        if (highEdges.isEmpty()) {
            hi = lo + minWidthHz;
        } else {
            const auto it = std::find_if(highEdges.cbegin(), highEdges.cend(),
                                         [&](int e) { return e - lo >= minWidthHz; });
            if (it != highEdges.cend()) {
                hi = *it;
            }
        }
    }
    return {lo, hi};
}

} // namespace AetherSDR::TxFilterEdges
