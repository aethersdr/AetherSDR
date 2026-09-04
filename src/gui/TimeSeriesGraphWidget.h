#pragma once

// Extracted verbatim from NetworkDiagnosticsDialog.cpp (#2554): the System Info
// dialog needs the same time-series chart, and a file-private class cannot be
// shared. Nothing about the widget changed in the move.

#include <optional>

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPair>
#include <QPen>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSet>
#include <QSizePolicy>
#include <QString>
#include <QVector>
#include <QWidget>

namespace AetherSDR {

// Shared amber color for all adaptive-throttle UI elements (badge, graph band, fps-cap
// trace, state label).  Matches qualityColor("Fair") in MainWindow.cpp.
static constexpr auto kThrottleAmber = "#cc9900";

class TimeSeriesGraphWidget : public QWidget {
public:
    struct Series {
        QString label;
        QColor  color;
        QVector<QPointF> points;
        QString unitSuffix;
        bool    stepFunction{false};  // draw as horizontal-then-vertical steps
        double  maxConnectGapSeconds{0.0};
    };

    struct LegendHit {
        QRect   rect;
        QString label;
    };

    explicit TimeSeriesGraphWidget(QString title, QString suffix, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_title(std::move(title))
        , m_suffix(std::move(suffix))
    {
        setMinimumHeight(220);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::PointingHandCursor);
    }

    void setSeries(QVector<Series> series, int rangeSeconds)
    {
        m_series = std::move(series);
        m_rangeSeconds = rangeSeconds;
        if (!m_selectedLabels.isEmpty()) {
            QSet<QString> available;
            for (const Series& series : m_series) {
                available.insert(series.label);
            }
            for (auto it = m_selectedLabels.begin(); it != m_selectedLabels.end();) {
                if (!available.contains(*it)) {
                    it = m_selectedLabels.erase(it);
                } else {
                    ++it;
                }
            }
        }
        update();
    }

    void setPrimaryAxisSeries(QString label)
    {
        m_primaryAxisSeries = std::move(label);
    }

    // Switch the y-axis to logarithmic scale.  Suitable for series whose
    // dynamic range spans multiple orders of magnitude (rate graphs:
    // RX total ~4 Mbps next to per-stream lines ~50 kbps).  Latency,
    // loss, and audio-buffer graphs stay linear — their ranges are
    // small enough that log scaling would just compress useful detail.
    void setLogScale(bool on)
    {
        if (m_logScale == on) {
            return;
        }
        m_logScale = on;
        update();
    }

    void setFixedYRange(double minimum, double maximum)
    {
        if (!std::isfinite(minimum) || !std::isfinite(maximum)
            || minimum >= maximum) {
            m_fixedMinY.reset();
            m_fixedMaxY.reset();
        } else {
            m_fixedMinY = minimum;
            m_fixedMaxY = maximum;
        }
        update();
    }

    // Highlight time spans where adaptive throttle was active.
    // Each pair is (startRatio, endRatio) in [0,1] over the visible range.
    void setThrottleSpans(QVector<QPair<double,double>> spans)
    {
        m_throttleSpans = std::move(spans);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor("#050b13"));

        const QRectF plot = rect().adjusted(84, 30, -14, -42);
        painter.setPen(QPen(QColor("#233246"), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 7, 7);

        painter.setPen(QColor("#d4deea"));
        const QFont normalFont = painter.font();
        QFont titleFont = painter.font();
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.drawText(QRectF(10, 6, width() - 190, 18), Qt::AlignLeft | Qt::AlignVCenter, m_title);
        painter.setFont(normalFont);
        painter.setPen(QColor("#8d99ad"));
        painter.drawText(QRectF(width() - 180, 6, 166, 18),
                         Qt::AlignRight | Qt::AlignVCenter, rangeLabel());

        const QVector<Series> visibleSeries = activeSeries();
        const bool hasPoints = std::any_of(visibleSeries.cbegin(), visibleSeries.cend(), [](const Series& series) {
            return !series.points.isEmpty();
        });
        if (!hasPoints || plot.width() < 20 || plot.height() < 20) {
            painter.setPen(QColor("#8d99ad"));
            painter.drawText(plot, Qt::AlignCenter, "Collecting graph data");
            return;
        }

        const QVector<Series> scaledSeries = axisScaledSeries(visibleSeries);
        double maxY = 1.0;
        for (const Series& series : scaledSeries) {
            for (const QPointF& point : series.points) {
                maxY = std::max(maxY, point.y());
            }
        }
        const QString axisSuffix = activeAxisSuffix(visibleSeries);

        // Log-scale path: snap maxY up to the next power of 10 and
        // anchor the floor at 1 unit (1 kbps for rate graphs) so quiet
        // streams have headroom and the low decades stay visible
        // regardless of the smallest observed sample.  Log can't plot
        // zero, but the bottom decade label is overridden to "0" below
        // so the axis reads with a familiar zero baseline.
        double minY = 0.0;
        if (m_fixedMinY.has_value() && m_fixedMaxY.has_value()) {
            minY = *m_fixedMinY;
            maxY = *m_fixedMaxY;
        } else if (m_logScale) {
            const double exactMax = std::max(maxY, 10.0);
            maxY = std::pow(10.0, std::ceil(std::log10(exactMax)));
            minY = 1.0;
            if (minY >= maxY) {
                minY = maxY / 10.0;
            }
        } else {
            maxY = niceCeiling(maxY);
        }

        // Y-axis grid + tick labels.  Linear: 4 evenly-spaced.
        // Log: one tick per decade between minY and maxY so labels
        // sit at clean 1k / 10k / 100k / 1M / 10M boundaries.
        const int yTicks = m_logScale
            ? std::max(1, static_cast<int>(std::round(std::log10(maxY / minY))))
            : 4;
        painter.setPen(QPen(QColor("#233246"), 1));
        for (int i = 0; i <= yTicks; ++i) {
            const double y = plot.bottom() - (plot.height() * i / yTicks);
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            const double tickValue = m_logScale
                ? minY * std::pow(10.0, static_cast<double>(i) * std::log10(maxY / minY) / yTicks)
                : minY + (maxY - minY) * i / yTicks;
            // For the log path, relabel the bottom-most tick as "0" so
            // the axis reads with a familiar zero baseline — values at
            // or below the floor (~1 unit) are functionally silent and
            // already clamp to minY in the y-mapping below.
            QString label;
            if (m_logScale && i == 0) {
                label = QString("0%1").arg(axisSuffix);
            } else {
                label = formatAxisValue(tickValue, axisSuffix);
            }
            painter.setPen(QColor("#8d99ad"));
            painter.drawText(QRectF(4, y - 8, 74, 16), Qt::AlignRight | Qt::AlignVCenter,
                             label);
            painter.setPen(QPen(QColor("#233246"), 1));
        }
        for (int i = 0; i <= 4; ++i) {
            const double x = plot.left() + (plot.width() * i / 4.0);
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }

        // Amber shading for adaptive-throttle active spans
        if (!m_throttleSpans.isEmpty()) {
            QColor bandColor(kThrottleAmber);
            bandColor.setAlpha(28);
            painter.setBrush(bandColor);
            painter.setPen(Qt::NoPen);
            for (const auto& span : m_throttleSpans) {
                const double x0 = plot.left() + plot.width() * std::clamp(span.first,  0.0, 1.0);
                const double x1 = plot.left() + plot.width() * std::clamp(span.second, 0.0, 1.0);
                if (x1 > x0)
                    painter.fillRect(QRectF(x0, plot.top(), x1 - x0, plot.height()), bandColor);
            }
            painter.setBrush(Qt::NoBrush);
        }

        for (const Series& series : visibleSeries) {
            if (series.points.isEmpty()) {
                continue;
            }

            QPainterPath path;
            bool first = true;
            bool hasBucket = false;
            int bucketPixel = 0;
            int bucketCount = 0;
            QPointF bucketSum;
            auto flushBucket = [&] {
                if (!hasBucket || bucketCount <= 0) {
                    return;
                }
                const QPointF mapped = bucketSum / bucketCount;
                if (first) {
                    path.moveTo(mapped);
                    first = false;
                } else {
                    path.lineTo(mapped);
                }
                bucketSum = QPointF();
                bucketCount = 0;
            };

            auto mapPoint = [&](const QPointF& point) -> QPointF {
                const double xRatio = std::clamp(point.x() / std::max(1, m_rangeSeconds), 0.0, 1.0);
                double yRatio;
                if (m_logScale) {
                    const double clamped = std::clamp(point.y(), minY, maxY);
                    yRatio = std::log10(clamped / minY) / std::log10(maxY / minY);
                } else {
                    yRatio = std::clamp(
                        (point.y() - minY) / std::max(0.001, maxY - minY),
                        0.0,
                        1.0);
                }
                return {plot.left() + plot.width() * xRatio,
                        plot.bottom() - plot.height() * yRatio};
            };

            if (series.stepFunction) {
                // Step-function: horizontal run at current y, then vertical jump at each x
                for (int pi = 0; pi < series.points.size(); ++pi) {
                    const QPointF mapped = mapPoint(series.points[pi]);
                    if (first) {
                        path.moveTo(mapped);
                        first = false;
                    } else {
                        // Horizontal to new x, then vertical to new y
                        path.lineTo(QPointF(mapped.x(), path.currentPosition().y()));
                        path.lineTo(mapped);
                    }
                }
                // Extend last step to the right edge of the plot
                if (!first)
                    path.lineTo(QPointF(plot.right(), path.currentPosition().y()));
            } else {
                double previousSeconds = -1.0;
                for (const QPointF& point : series.points) {
                    if (series.maxConnectGapSeconds > 0.0
                        && previousSeconds >= 0.0
                        && point.x() - previousSeconds > series.maxConnectGapSeconds) {
                        flushBucket();
                        hasBucket = false;
                        first = true;
                    }
                    const QPointF mapped = mapPoint(point);
                    const int pixel = static_cast<int>(std::round(mapped.x()));
                    if (!hasBucket) {
                        hasBucket = true;
                        bucketPixel = pixel;
                    } else if (pixel != bucketPixel) {
                        flushBucket();
                        bucketPixel = pixel;
                    }
                    bucketSum += mapped;
                    ++bucketCount;
                    previousSeconds = point.x();
                }
                flushBucket();
            }
            painter.setPen(QPen(series.color, 2));
            painter.drawPath(path);
        }

        // Per-series "last sample" hints in the left gutter.  Each
        // visible series gets a colored label at the y-pixel matching
        // its most recent value; labels are spread vertically to avoid
        // overlap when several streams sit close together (e.g. RX and
        // Audio both around ~1 Mbps).
        struct ValueHint {
            double  idealY;
            double  y;
            QColor  color;
            QString text;
        };
        QVector<ValueHint> hints;
        hints.reserve(visibleSeries.size());
        for (const Series& series : visibleSeries) {
            if (series.points.isEmpty()) {
                continue;
            }
            const double v = series.points.last().y();
            double yRatio;
            if (m_logScale) {
                const double clamped = std::clamp(v, minY, maxY);
                yRatio = std::log10(clamped / minY) / std::log10(maxY / minY);
            } else {
                yRatio = std::clamp(
                    (v - minY) / std::max(0.001, maxY - minY),
                    0.0,
                    1.0);
            }
            const double y = plot.bottom() - plot.height() * yRatio;
            const QString unitSuffix = series.unitSuffix.isEmpty() ? m_suffix : series.unitSuffix;
            hints.push_back({y, y, series.color, formatAxisValue(v, unitSuffix)});
        }
        std::sort(hints.begin(), hints.end(),
                  [](const ValueHint& a, const ValueHint& b) { return a.idealY < b.idealY; });
        constexpr double kHintMinGap = 14.0;
        double prev = plot.top() - kHintMinGap;
        for (ValueHint& h : hints) {
            if (h.y < prev + kHintMinGap) h.y = prev + kHintMinGap;
            if (h.y > plot.bottom())      h.y = plot.bottom();
            prev = h.y;
        }
        double next = plot.bottom() + kHintMinGap;
        for (int i = hints.size() - 1; i >= 0; --i) {
            if (hints[i].y > next - kHintMinGap) hints[i].y = next - kHintMinGap;
            if (hints[i].y < plot.top())          hints[i].y = plot.top();
            next = hints[i].y;
        }
        for (const ValueHint& h : hints) {
            const QRectF rect(4, h.y - 10, 74, 20);
            // Vertical alpha gradient (0 → chart bg → 0) so the soft
            // top/bottom edges blend into adjacent hints rather than
            // butting them with a hard rectangle seam.  The opaque
            // middle band still hides whatever decade tick may sit at
            // the same y-coordinate.
            QLinearGradient bgGrad(rect.center().x(), rect.top(),
                                   rect.center().x(), rect.bottom());
            const QColor bgSolid("#050b13");
            QColor bgEdge = bgSolid;
            bgEdge.setAlpha(0);
            // 20 px total: 6 px fully-opaque centre band, 7 px fade
            // on each side (stops at 0.35 and 0.65 = 7/20 and 13/20).
            bgGrad.setColorAt(0.0,  bgEdge);
            bgGrad.setColorAt(0.35, bgSolid);
            bgGrad.setColorAt(0.65, bgSolid);
            bgGrad.setColorAt(1.0,  bgEdge);
            painter.fillRect(rect, bgGrad);
            painter.setPen(h.color);
            painter.drawText(rect, Qt::AlignRight | Qt::AlignVCenter, h.text);
        }

        drawLegend(&painter, plot);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        for (const LegendHit& hit : m_legendHits) {
            if (!hit.rect.contains(event->pos())) {
                continue;
            }
            if (event->modifiers().testFlag(Qt::ControlModifier)) {
                if (m_selectedLabels.contains(hit.label)) {
                    m_selectedLabels.remove(hit.label);
                } else {
                    m_selectedLabels.insert(hit.label);
                }
            } else {
                const bool onlySelected = m_selectedLabels.size() == 1 && m_selectedLabels.contains(hit.label);
                m_selectedLabels.clear();
                if (!onlySelected) {
                    m_selectedLabels.insert(hit.label);
                }
            }
            update();
            return;
        }
        QWidget::mousePressEvent(event);
    }

private:
    static double niceCeiling(double value)
    {
        if (value <= 1.0) {
            return 1.0;
        }
        const double magnitude = std::pow(10.0, std::floor(std::log10(value)));
        const double normalized = value / magnitude;
        if (normalized <= 2.0) {
            return 2.0 * magnitude;
        }
        if (normalized <= 5.0) {
            return 5.0 * magnitude;
        }
        return 10.0 * magnitude;
    }

    QString formatAxisValue(double value, const QString& suffix) const
    {
        if (suffix.contains("ksps", Qt::CaseInsensitive)) {
            return QString("%1 ksps").arg(value, 0, 'f', 2);
        }
        // kbps already has a metric prefix baked in — scale up to
        // Mbps / Gbps when the value crosses each power of 1000 so the
        // axis reads "3.8 Mbps" rather than the confusing "3.8k kbps".
        if (suffix.contains("kbps", Qt::CaseInsensitive)) {
            if (value >= 1'000'000.0) {
                return QString("%1 Gbps").arg(value / 1'000'000.0, 0, 'f', 1);
            }
            if (value >= 1000.0) {
                return QString("%1 Mbps").arg(value / 1000.0, 0, 'f', 1);
            }
            const int precision = value >= 10.0 ? 0 : 1;
            return QString("%1 kbps").arg(value, 0, 'f', precision);
        }
        if (value >= 1000000.0) {
            return QString("%1M%2").arg(value / 1000000.0, 0, 'f', 1).arg(suffix);
        }
        if (value >= 1000.0) {
            return QString("%1k%2").arg(value / 1000.0, 0, 'f', 1).arg(suffix);
        }
        const int precision = value >= 10.0 ? 0 : 1;
        return QString("%1%2").arg(value, 0, 'f', precision).arg(suffix);
    }

    QString rangeLabel() const
    {
        if (m_rangeSeconds < 3600) {
            const int minutes = m_rangeSeconds / 60;
            return QString("Last %1 %2").arg(minutes).arg(minutes == 1 ? "minute" : "minutes");
        }
        if (m_rangeSeconds < 86400) {
            const int hours = m_rangeSeconds / 3600;
            return QString("Last %1 %2").arg(hours).arg(hours == 1 ? "hour" : "hours");
        }
        const int days = m_rangeSeconds / 86400;
        return QString("Last %1 %2").arg(days).arg(days == 1 ? "day" : "days");
    }

    QVector<Series> activeSeries() const
    {
        if (m_selectedLabels.isEmpty()) {
            return m_series;
        }

        QVector<Series> selected;
        selected.reserve(m_selectedLabels.size());
        for (const Series& series : m_series) {
            if (m_selectedLabels.contains(series.label)) {
                selected.push_back(series);
            }
        }
        return selected.isEmpty() ? m_series : selected;
    }

    QVector<Series> axisScaledSeries(const QVector<Series>& visibleSeries) const
    {
        if (!m_primaryAxisSeries.isEmpty()
            && (m_selectedLabels.isEmpty() || m_selectedLabels.contains(m_primaryAxisSeries))) {
            QVector<Series> primary;
            primary.reserve(1);
            for (const Series& series : visibleSeries) {
                if (series.label == m_primaryAxisSeries) {
                    primary.push_back(series);
                    break;
                }
            }
            if (!primary.isEmpty()) {
                return primary;
            }
        }
        return visibleSeries;
    }

    QString activeAxisSuffix(const QVector<Series>& visibleSeries) const
    {
        if (!m_primaryAxisSeries.isEmpty()
            && (m_selectedLabels.isEmpty() || m_selectedLabels.contains(m_primaryAxisSeries))) {
            for (const Series& series : m_series) {
                if (series.label == m_primaryAxisSeries) {
                    return series.unitSuffix.isEmpty() ? m_suffix : series.unitSuffix;
                }
            }
            return m_suffix;
        }

        if (m_selectedLabels.size() == 1) {
            const QString selectedLabel = *m_selectedLabels.constBegin();
            for (const Series& series : m_series) {
                if (series.label == selectedLabel) {
                    return series.unitSuffix.isEmpty() ? m_suffix : series.unitSuffix;
                }
            }
        }

        QString suffix;
        for (const Series& series : visibleSeries) {
            if (suffix.isEmpty()) {
                suffix = series.unitSuffix;
            } else if (suffix != series.unitSuffix) {
                return m_suffix;
            }
        }
        return suffix.isEmpty() ? m_suffix : suffix;
    }

    void drawLegend(QPainter* painter, const QRectF& plot)
    {
        m_legendHits.clear();
        int x = static_cast<int>(plot.left());
        int y = static_cast<int>(plot.bottom()) + 12;
        const QFontMetrics fm(painter->font());
        for (const Series& series : m_series) {
            if (series.points.isEmpty()) {
                continue;
            }
            const bool selected = m_selectedLabels.isEmpty() || m_selectedLabels.contains(series.label);
            const QColor textColor = selected ? QColor("#d4deea") : QColor("#6e7a8d");
            const QColor lineColor = selected ? series.color : QColor("#25364d");
            const int labelWidth = fm.horizontalAdvance(series.label);
            const QRect hitRect(x, y, labelWidth + 24, 18);

            painter->setPen(QPen(lineColor, selected ? 2 : 1));
            painter->drawLine(x, y + 7, x + 14, y + 7);
            painter->setPen(textColor);
            painter->drawText(x + 18, y, labelWidth + 8, 16,
                              Qt::AlignLeft | Qt::AlignVCenter, series.label);
            m_legendHits.push_back({hitRect, series.label});
            x += 30 + labelWidth;
            if (x > width() - 110) {
                break;
            }
        }
    }

    QString m_title;
    QString m_suffix;
    QString m_primaryAxisSeries;
    int m_rangeSeconds{300};
    QVector<Series> m_series;
    QSet<QString> m_selectedLabels;
    QVector<LegendHit> m_legendHits;
    bool m_logScale{false};
    std::optional<double> m_fixedMinY;
    std::optional<double> m_fixedMaxY;
    QVector<QPair<double,double>> m_throttleSpans;
};

} // namespace AetherSDR
