#include "gui/workspace/ClassicLayout.h"

#include <QVector>

namespace AetherSDR {

namespace {

// Pan layout table: rows of equal-width cells, rows of equal height. Must match
// PanLayoutDialog::kAllLayouts and PanadapterStack's kLayoutPanCounts by hand;
// the headless test pins only this copy. This stores per-row cell COUNTS while
// kAllLayouts stores per-cell weights (all 1 today): a non-uniform layout like
// {{2,1}} needs this changed to carry weights, or it renders as equal halves.
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

    // Up to kClassicMaxAppletsPerColumn applets per column (slots stay above the
    // 90 px floor on a 1080 px canvas). Beyond that the column wraps, up to three
    // columns, since the canvas cannot scroll like the real panel; further applets
    // compress within the last column.
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
