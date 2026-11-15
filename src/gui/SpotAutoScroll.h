#pragma once

#include <Qt>

namespace AetherSDR {

// Which end of the spot table view (if either) DxClusterDialog::flushSpotBatch()
// should scroll to after inserting new spots.
enum class SpotAutoScrollTarget { None, Top, Bottom };

// Where the newest spot lands, and whether the viewport is already there. Pure
// so it's testable without DxClusterDialog. Scroll state is taken before the
// insert. The source model prepends (row 0), so:
//   - unsorted, or Time descending -> TOP;
//   - Time ascending               -> BOTTOM;
//   - any other column             -> never auto-scroll.
inline SpotAutoScrollTarget decideSpotAutoScrollTarget(
    int sortColumn, Qt::SortOrder sortOrder, int timeColumn,
    int scrollValue, int scrollMaximum)
{
    const bool newestAtTop = sortColumn < 0
        || (sortColumn == timeColumn && sortOrder == Qt::DescendingOrder);
    const bool newestAtBottom = sortColumn == timeColumn
        && sortOrder == Qt::AscendingOrder;

    if (newestAtTop && scrollValue <= 2) {
        return SpotAutoScrollTarget::Top;
    }
    if (newestAtBottom && scrollValue >= scrollMaximum - 2) {
        return SpotAutoScrollTarget::Bottom;
    }
    return SpotAutoScrollTarget::None;
}

} // namespace AetherSDR
