#pragma once

#include "core/ThreadCpuRing.h"

#include <QApplication>
#include <QList>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>

namespace AetherSDR {

// Draws one thread's recent CPU readings as a mini-chart (#2554). A delegate,
// not setCellWidget(), because the table refills every 1.5 s. The vertical scale
// is fixed at 0-100 % of one core so a tall line always means a busy thread;
// per-row auto-fit would exaggerate idle noise and flatten saturation.
class SparklineDelegate : public QStyledItemDelegate {
public:
    // The series lives on the item under this role, oldest reading first.
    static constexpr int kSeriesRole = Qt::UserRole + 1;

    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        // Paint the cell chrome without the display text (the value exists for
        // sorting). Drawn through the style directly: QStyledItemDelegate::paint()
        // re-runs initStyleOption() and would repopulate the text over the chart.
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
