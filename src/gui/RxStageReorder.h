#pragma once

#include <QVector>

#include <algorithm>

namespace AetherSDR::RxStageReorder {

// The rule behind dragging a stage row up or down the AetherRX tab column.
//
// Kept out of the dialog because the dialog's version of it cannot be reached
// from a test: it runs inside a drop handler, off a nested drag loop that the
// offscreen platform plugin does not run at all. Everything here is arithmetic
// on an order and a set of row positions, so it can be.
//
// `order`     — the chain, as stage ids, in the order the signal meets them.
// `moved`     — the id being dragged; not in `order` means nothing to do.
// `midpoints` — the vertical middle of each row, in the same order as `order`.
// `dropY`     — where the drag was let go, in the same coordinates.
//
// The row lands one place past every row whose middle it was dropped below.
// Midpoints are passed in rather than derived from a row height because the
// column also holds rows that are not chain stages — AetherNR at the top and
// Out at the foot — and they are not being counted.
inline QVector<int> dropped(const QVector<int>& order, int moved,
                            const QVector<int>& midpoints, int dropY)
{
    const int from = order.indexOf(moved);
    if (from < 0) return order;

    int to = 0;
    for (int mid : midpoints) {
        if (dropY < mid) break;
        ++to;
    }

    QVector<int> out = order;
    out.removeAt(from);
    // Removing first shifts everything after it up one place.
    if (to > from) --to;
    to = std::clamp(to, 0, static_cast<int>(out.size()));
    if (to == from) return order;
    out.insert(to, moved);
    return out;
}

} // namespace AetherSDR::RxStageReorder
