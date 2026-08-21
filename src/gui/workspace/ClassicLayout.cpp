#include "gui/workspace/ClassicLayout.h"

#include <QVector>

namespace AetherSDR {

namespace {

// The pan layout table, mirroring kAllLayouts in PanLayoutDialog.cpp: each
// entry is rows of equal-width cells, rows of equal height.  Kept as data
// rather than as the dialog's widget code so the geometry is testable without
// a GUI — and so a thirteenth layout id is one line here, not a new branch.
//
// DRIFT WARNING: this is a third copy of the same knowledge.  The other two —
// PanLayoutDialog::kAllLayouts and PanadapterStack's kLayoutPanCounts — live
// in the GUI target, which the headless migration test does not link, so
// nothing compares them automatically.  What the test does do is duplicate the
// expected id/cell-count pairs, so a change HERE fails immediately; a change
// to either of the other two still has to be mirrored by hand.  Phase 3 puts
// this in reach of a GUI-linked test and should close the gap properly.
//
// AND NOTE THE SHAPE IS NOT IDENTICAL.  kAllLayouts stores per-cell WEIGHTS
// (`{{1,1},{1}}`); this stores per-row CELL COUNTS.  Today every weight is 1,
// so the two agree exactly — but a future non-uniform layout, say a 2:1 split
// written `{{2,1}}`, is not representable here at all and would silently come
// out as equal halves rather than failing.  Adding one means changing this
// vector to carry weights, not just adding a row (PR #4900 review, L1).
struct LayoutRows {
    const char* id;
    QVector<int> rows;   // cells per row, top to bottom
};

const QVector<LayoutRows>& layoutTable()
{
    static const QVector<LayoutRows> kTable = {
        {"2v",  {1, 1}},
        {"2h",  {2}},
        {"2h1", {2, 1}},
        {"12h", {1, 2}},
        {"3v",  {1, 1, 1}},
        {"2x2", {2, 2}},
        {"4v",  {1, 1, 1, 1}},
        {"3h2", {3, 2}},
        {"2x3", {2, 2, 2}},
        {"4h3", {4, 3}},
        {"2x4", {2, 2, 2, 2}},
        {"1",   {1}},
    };
    return kTable;
}

const LayoutRows* findLayout(const QString& layoutId)
{
    for (const LayoutRows& entry : layoutTable()) {
        if (layoutId == QLatin1String(entry.id)) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

QList<NormRect> panCellsForLayout(const QString& layoutId)
{
    const LayoutRows* entry = findLayout(layoutId);
    if (!entry) {
        return {};
    }

    QList<NormRect> cells;
    const int rowCount = entry->rows.size();
    const double rowHeight = 1.0 / rowCount;

    for (int row = 0; row < rowCount; ++row) {
        const int cellCount = entry->rows.at(row);
        const double cellWidth = 1.0 / cellCount;
        for (int col = 0; col < cellCount; ++col) {
            NormRect r;
            r.x = col * cellWidth;
            r.y = row * rowHeight;
            r.w = cellWidth;
            r.h = rowHeight;
            cells.append(r);
        }
    }
    return cells;
}

QStringList knownPanLayoutIds()
{
    QStringList ids;
    for (const LayoutRows& entry : layoutTable()) {
        ids.append(QString::fromLatin1(entry.id));
    }
    return ids;
}

int panCountForLayout(const QString& layoutId)
{
    return static_cast<int>(panCellsForLayout(layoutId).size());
}

QList<CanvasItem> composeClassic(const QStringList& panIds,
                                 const QStringList& appletIds,
                                 const QString& layoutId,
                                 bool appletsLeft)
{
    QList<CanvasItem> items;

    // The column takes its width only if there is something to put in it —
    // an empty applet column would leave a band of dead canvas that the
    // operator never asked for.
    const bool haveApplets = !appletIds.isEmpty();
    const double columnWidth = haveApplets ? kClassicAppletColumnWidth : 0.0;
    const int wrapCols = haveApplets
                             ? qMin(3, (static_cast<int>(appletIds.size())
                                        + kClassicMaxAppletsPerColumn - 1)
                                           / kClassicMaxAppletsPerColumn)
                             : 0;
    const double panWidth = 1.0 - wrapCols * columnWidth;
    const double panOriginX = appletsLeft ? wrapCols * columnWidth : 0.0;

    // ── Pans, in their layout's cells, scaled into the pan region ────────
    QList<NormRect> cells = panCellsForLayout(layoutId);
    if (cells.isEmpty()) {
        cells = panCellsForLayout(QStringLiteral("1"));
    }

    const int placeable = qMin(static_cast<int>(panIds.size()),
                               static_cast<int>(cells.size()));
    for (int i = 0; i < placeable; ++i) {
        const NormRect& cell = cells.at(i);

        CanvasItem item;
        item.id          = QStringLiteral("pan:") + panIds.at(i);
        item.contentType = QStringLiteral("panadapter");
        item.z           = static_cast<int>(items.size());
        item.rect.x = panOriginX + cell.x * panWidth;
        item.rect.y = cell.y;
        item.rect.w = cell.w * panWidth;
        item.rect.h = cell.h;
        items.append(item);
    }

    // ── Applets, stacked down the column(s) ──────────────────────────────
    //
    // Up to ~11 applets share one column (slots stay above the 90 px display
    // floor on a 1080 px canvas).  Beyond that the column WRAPS instead of
    // letting slots collapse into an overlap — the L3 answer from PR #4900:
    // the real panel scrolls, the canvas cannot, so Classic trades spectrum
    // width for a second (at most third) column.  Applets beyond three full
    // columns compress within the last one; at that point the operator has
    // more applets open than any arrangement can show, and the canvas is the
    // tool for choosing which ones matter.
    if (haveApplets) {
        const int n = static_cast<int>(appletIds.size());
        const int colCount = qMin(3, (n + kClassicMaxAppletsPerColumn - 1)
                                         / kClassicMaxAppletsPerColumn);

        // Even distribution: 12 applets become 6+6, not 11+1.
        const int base  = n / colCount;
        const int extra = n % colCount;

        int index = 0;
        for (int col = 0; col < colCount; ++col) {
            const int inThisColumn = base + (col < extra ? 1 : 0);
            const double slotHeight = 1.0 / qMax(1, inThisColumn);

            // Columns fill from the panel side inward.  Right-docked panel:
            // column 0 hugs the right edge, column 1 sits left of it.
            const double colX = appletsLeft
                                    ? col * columnWidth
                                    : 1.0 - (col + 1) * columnWidth;

            for (int j = 0; j < inThisColumn && index < n; ++j, ++index) {
                CanvasItem item;
                item.id          = QStringLiteral("applet:") + appletIds.at(index);
                item.contentType = QStringLiteral("applet");
                item.z           = static_cast<int>(items.size());
                item.rect.x = colX;
                item.rect.y = j * slotHeight;
                item.rect.w = columnWidth;
                item.rect.h = slotHeight;
                items.append(item);
            }
        }
    }

    return items;
}

}  // namespace AetherSDR
