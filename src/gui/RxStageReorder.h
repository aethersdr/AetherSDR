#pragma once

#include <QVector>

#include <algorithm>

namespace AetherSDR::RxStageReorder {

// Drop rule for reordering AetherRX stage rows, pure because the dialog's drop
// handler runs in a drag loop the offscreen platform does not run. `order`:
// stage ids in signal order; `moved`: the dragged id (absent → no-op);
// `midpoints`: each row's vertical middle; `dropY`: release point. The row
// lands one past every row whose middle is above dropY. Midpoints are passed in
// because the column also holds non-stage rows (AetherNR, Out).
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
