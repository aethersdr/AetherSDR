#pragma once

#include "core/ThreadCpuRing.h"

#include <QApplication>
#include <QList>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>

namespace AetherSDR {

// Draws one thread's recent CPU readings as a mini-chart inside a table cell
// (#2554, the Threads tab's sparkline column).
//
// A delegate rather than a widget per row. The table is refilled every 1.5 s
// with sorting live, so setCellWidget() would mean creating and destroying a
// widget per thread per interval — around thirty of them, forever, for a
// decoration. A delegate paints from data the item already carries and creates
// nothing.
//
// The vertical scale is FIXED at 0-100 % of one core rather than fitted to each
// row's own range. Auto-fitting would draw an idle thread's noise as a dramatic
// mountain range and a saturated thread's steady 99 % as a flat line, which
// inverts the one thing this dialog exists to show. Fixed scale means a tall
// line is a busy thread, on every row, at a glance.
class SparklineDelegate : public QStyledItemDelegate {
public:
    // The series lives on the item under this role, oldest reading first.
    static constexpr int kSeriesRole = Qt::UserRole + 1;

    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        // Selection highlight and the rest of the cell chrome, WITHOUT the
        // display text: the value under this column exists to sort by, not to
        // be read — it is the same number the Peak column already shows.
        //
        // Drawn through the style directly rather than by calling
        // QStyledItemDelegate::paint() with a cleared option. That method runs
        // initStyleOption() again on its own copy, which repopulates the text
        // from DisplayRole and undoes the clear — the raw double then lands on
        // top of the chart, unrounded and left-aligned, which is exactly what
        // it did on first run.
        QStyleOptionViewItem chrome(option);
        initStyleOption(&chrome, index);
        chrome.text.clear();
        const QWidget* host = chrome.widget;
        QStyle* style = host != nullptr ? host->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &chrome, painter, host);

        const QList<double> series = index.data(kSeriesRole).value<QList<double>>();
        if (series.isEmpty()) {
            return;   // a thread seen for the first time: draw nothing rather
                      // than a flat line at zero, which would read as idle
        }

        const QRectF area = QRectF(option.rect).adjusted(3.0, 3.0, -3.0, -3.0);
        if (area.width() <= 1.0 || area.height() <= 1.0) {
            return;
        }

        const auto yFor = [&area](double percent) {
            const double clamped = percent < 0.0 ? 0.0 : (percent > 100.0 ? 100.0 : percent);
            return area.bottom() - (clamped / 100.0) * area.height();
        };

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QPen pen(option.palette.color(QPalette::Highlight));
        pen.setWidthF(1.4);
        painter->setPen(pen);

        // One step per SAMPLE SLOT, not per sample held: the newest reading sits
        // at the right edge and older ones march left at a fixed spacing, so
        // every row shares one time axis. Dividing the width by what the ring
        // happens to hold instead would stretch a thread that started ten
        // seconds ago across the full minute, making three readings look like a
        // complete history — and no two rows would be comparable until the ring
        // filled.
        const double step = area.width() / static_cast<double>(ThreadCpuRing::kSamples - 1);

        if (series.size() == 1) {
            // One reading is a point, not a line. A polyline of a single point
            // draws nothing at all, which would be indistinguishable from the
            // no-data case above.
            painter->drawPoint(QPointF(area.right(), yFor(series.first())));
        } else {
            QPainterPath path;
            for (int i = 0; i < series.size(); ++i) {
                const int fromNewest = static_cast<int>(series.size()) - 1 - i;
                const QPointF point(area.right() - step * fromNewest, yFor(series.at(i)));
                if (i == 0) {
                    path.moveTo(point);
                } else {
                    path.lineTo(point);
                }
            }
            painter->drawPath(path);
        }
        painter->restore();
    }
};

}  // namespace AetherSDR
