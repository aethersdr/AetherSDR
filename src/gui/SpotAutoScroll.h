#pragma once

#include <Qt>

namespace AetherSDR {

// Which end of the spot table view (if either) DxClusterDialog::flushSpotBatch()
// should scroll to after inserting new spots.
enum class SpotAutoScrollTarget { None, Top, Bottom };

// Decides where the newest spot lands and whether the viewport is already
// sitting there — pulled out of flushSpotBatch() as a pure function (#4889
// review) so this four-branch decision is unit-testable without constructing
// DxClusterDialog, which pulls in DxClusterClient, WsjtxClient,
// SpotCollectorClient, PotaClient, EibiClient, N1MMSpotClient, FreeDvClient,
// RadioModel and DxccColorProvider just to build.
//
// `sortColumn`/`sortOrder` are the spot proxy model's current sort state;
// `timeColumn` is SpotTableModel::ColTime, passed in rather than depended on
// so this header stays dependency-free. `scrollValue`/`scrollMaximum` are the
// vertical scrollbar's state taken BEFORE the insert that triggered this call
// — "was the user already looking at the end the newest spot is about to
// land on" is what decides whether to follow.
//
// The source model is prepend-based (SpotTableModel::addSpot(s) always
// inserts at row 0), so:
//   - sortColumn < 0 (genuinely unsorted), or Time descending (newest first,
//     same order as the source model) → newest lands at the TOP.
//   - Time ascending → newest lands at the BOTTOM.
//   - sorted by any other column → newest lands wherever it lands; never
//     auto-scroll and disturb where the user put the viewport.
//
// The `sortColumn < 0` branch is defensive rather than reachable today:
// QTableView::setSortingEnabled(true) applies the header's default sort
// indicator immediately (Qt 6: section 0, descending — the Time column,
// descending), so the spot table is never actually in an unsorted state
// once shown. Kept because "unsorted" is a real value the proxy's API can
// report, and this function shouldn't assume a caller-side invariant that
// isn't its own to enforce.
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
