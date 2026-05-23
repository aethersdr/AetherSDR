#include "NetworkDiagnosticsDialog.h"
#include "core/AudioEngine.h"
#include "core/LogManager.h"
#include "core/AppSettings.h"
#include "core/TciServer.h"   // self-guards on HAVE_WEBSOCKETS
#include "models/RadioModel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStringList>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileDialog>
#include <QTextStream>
#include <QTime>
#include <QMenu>
#include <QColor>
#include <QJsonArray>
#include <QStandardPaths>
#include <functional>
#include <QSyntaxHighlighter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QTextCharFormat>
#include <QVBoxLayout>

namespace AetherSDR {

constexpr const char* kNetworkDiagnosticsStyle = R"(
QWidget {
    color: #aeb9cc;
    background: #07101c;
    font-size: 13px;
}
QLabel {
    background: transparent;
}
QFrame#DiagnosticsPanel {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #111d2c, stop:1 #0a1421);
    border: 1px solid #233246;
    border-radius: 7px;
}
QTabWidget::pane {
    background: transparent;
    border: none;
    top: 0;
}
QTabBar {
    background: transparent;
    border-bottom: none;
}
QTabBar::tab {
    color: #aeb9cc;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 9px 16px;
    margin: 3px 2px 2px 0;
    min-height: 24px;
    font-weight: 600;
}
QTabBar::tab:selected {
    color: #d4deea;
    border-color: #54c768;
    background: #0d1c20;
}
QTabBar::tab:hover {
    border-color: #3c526d;
    color: #d6dfeb;
}
QLabel#DiagnosticsPanelTitle {
    background: transparent;
    color: #8d99ad;
    font-size: 13px;
    font-weight: 700;
}
QPushButton {
    color: #aeb9cc;
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #142235, stop:1 #0b1625);
    border: 1px solid #26374e;
    border-radius: 7px;
    padding: 8px 16px;
    font-weight: 600;
}
QPushButton:hover {
    border-color: #3c526d;
    color: #d6dfeb;
}
QPushButton:checked {
    color: #d4deea;
    border-color: #54c768;
    background: #0d1c20;
}
QPushButton:disabled {
    color: #6e7a8d;
    border-color: #1d2a3c;
    background: #0b1522;
}
QComboBox {
    color: #aeb9cc;
    background: #0b1625;
    border: 1px solid #26374e;
    border-radius: 5px;
    padding: 6px 28px 6px 10px;
}
QComboBox:hover {
    border-color: #3c526d;
}
QComboBox::drop-down {
    border: none;
    width: 20px;
}
QComboBox QAbstractItemView {
    background: #0b1625;
    color: #aeb9cc;
    border: 1px solid #26374e;
    selection-background-color: #1b3650;
    selection-color: #d4deea;
}
QCheckBox {
    background: transparent;
    color: #aeb9cc;
    spacing: 9px;
}
QCheckBox::indicator {
    width: 18px;
    height: 18px;
    border-radius: 4px;
    border: 1px solid #34533c;
    background: #0d1a18;
}
QCheckBox::indicator:checked {
    background: #5ebd69;
    border-color: #65d379;
}
QPlainTextEdit,
QTableWidget {
    color: #c2ccdb;
    background: #050b13;
    border: 1px solid #233246;
    selection-background-color: #1b3650;
    font-family: "SF Mono", "Menlo", "Consolas", monospace;
    font-size: 12px;
}
QTableWidget {
    gridline-color: #233246;
}
QTableWidget::item {
    padding: 4px;
}
QTableWidget::item:alternate {
    background: #0b1625;
}
QHeaderView::section {
    background: #111d2c;
    color: #8d99ad;
    border: 1px solid #233246;
    padding: 5px;
    font-weight: 700;
}
QScrollArea,
QScrollArea > QWidget > QWidget {
    background: transparent;
    border: none;
}
QScrollBar:vertical {
    background: #07101c;
    width: 12px;
    margin: 8px 2px 8px 2px;
    border-radius: 6px;
}
QScrollBar::handle:vertical {
    background: #25364d;
    border-radius: 5px;
    min-height: 34px;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0px;
}
)";

static QFrame* makeDiagnosticsPanel(const QString& title, QWidget* parent = nullptr)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("DiagnosticsPanel"));
    frame->setAttribute(Qt::WA_StyledBackground, true);

    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(8, 5, 8, 8);
    layout->setSpacing(4);

    auto* titleLabel = new QLabel(title, frame);
    titleLabel->setObjectName(QStringLiteral("DiagnosticsPanelTitle"));
    layout->addWidget(titleLabel);

    return frame;
}

static QVBoxLayout* diagnosticsPanelLayout(QFrame* panel)
{
    return qobject_cast<QVBoxLayout*>(panel ? panel->layout() : nullptr);
}

static void addDiagnosticsPanelContent(QFrame* panel, QLayout* content)
{
    if (auto* layout = diagnosticsPanelLayout(panel)) {
        layout->addLayout(content);
        layout->addStretch(1);
    }
}

class LogSyntaxHighlighter : public QSyntaxHighlighter {
public:
    explicit LogSyntaxHighlighter(QTextDocument* parent)
        : QSyntaxHighlighter(parent)
    {
        m_timeFormat.setForeground(QColor("#8d99ad"));
        m_debugFormat.setForeground(QColor("#8d99ad"));
        m_infoFormat.setForeground(QColor("#77d8ff"));
        m_warningFormat.setForeground(QColor("#e8b977"));
        m_criticalFormat.setForeground(QColor("#ff6b6b"));
        m_categoryFormat.setForeground(QColor("#d4deea"));
        m_categoryFormat.setFontWeight(QFont::Bold);
        m_numberFormat.setForeground(QColor("#80ed91"));
        m_protocolFormat.setForeground(QColor("#d8b4ff"));
    }

protected:
    void highlightBlock(const QString& text) override
    {
        static const QRegularExpression timeRe(QStringLiteral("^\\[[^\\]]+\\]"));
        static const QRegularExpression levelRe(QStringLiteral("\\]\\s+(DBG|INF|WRN|CRT|FTL)\\s+"));
        static const QRegularExpression categoryRe(QStringLiteral("\\]\\s+(?:DBG|INF|WRN|CRT|FTL)\\s+([^:]+):"));
        static const QRegularExpression numberRe(QStringLiteral("\\b(?:0x[0-9a-fA-F]+|\\d+(?:\\.\\d+)?)\\b"));
        static const QRegularExpression protocolRe(QStringLiteral("\\b(?:C\\d+|R\\d+|S[0-9a-fA-F]+|VITA-49|UDP|TCP|RX|TX)\\b"));

        QRegularExpressionMatch match = timeRe.match(text);
        if (match.hasMatch()) {
            setFormat(match.capturedStart(), match.capturedLength(), m_timeFormat);
        }

        match = levelRe.match(text);
        if (match.hasMatch()) {
            const QString level = match.captured(1);
            QTextCharFormat levelFormat = m_debugFormat;
            if (level == "INF") {
                levelFormat = m_infoFormat;
            } else if (level == "WRN") {
                levelFormat = m_warningFormat;
            } else if (level == "CRT" || level == "FTL") {
                levelFormat = m_criticalFormat;
            }
            setFormat(match.capturedStart(1), match.capturedLength(1), levelFormat);
        }

        match = categoryRe.match(text);
        if (match.hasMatch()) {
            setFormat(match.capturedStart(1), match.capturedLength(1), m_categoryFormat);
        }

        QRegularExpressionMatchIterator it = numberRe.globalMatch(text);
        while (it.hasNext()) {
            match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), m_numberFormat);
        }

        it = protocolRe.globalMatch(text);
        while (it.hasNext()) {
            match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), m_protocolFormat);
        }
    }

private:
    QTextCharFormat m_timeFormat;
    QTextCharFormat m_debugFormat;
    QTextCharFormat m_infoFormat;
    QTextCharFormat m_warningFormat;
    QTextCharFormat m_criticalFormat;
    QTextCharFormat m_categoryFormat;
    QTextCharFormat m_numberFormat;
    QTextCharFormat m_protocolFormat;
};

class TimeSeriesGraphWidget : public QWidget {
public:
    struct Series {
        QString label;
        QColor  color;
        QVector<QPointF> points;
        QString unitSuffix;
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
        double minY = 1.0;
        if (m_logScale) {
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
                : maxY * i / yTicks;
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

            for (const QPointF& point : series.points) {
                const double xRatio = std::clamp(point.x() / std::max(1, m_rangeSeconds), 0.0, 1.0);
                double yRatio;
                if (m_logScale) {
                    const double clamped = std::clamp(point.y(), minY, maxY);
                    yRatio = std::log10(clamped / minY) / std::log10(maxY / minY);
                } else {
                    yRatio = std::clamp(point.y() / maxY, 0.0, 1.0);
                }
                const QPointF mapped(plot.left() + plot.width() * xRatio,
                                     plot.bottom() - plot.height() * yRatio);
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
            }
            flushBucket();
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
                yRatio = std::clamp(v / maxY, 0.0, 1.0);
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
};

NetworkDiagnosticsDialog::NetworkDiagnosticsDialog(RadioModel* model,
                                                   AudioEngine* audio,
                                                   NetworkDiagnosticsHistory* history,
                                                   TciServer* tci,
                                                   QWidget* parent)
    : PersistentDialog("Network Diagnostics", "NetworkDiagnosticsDialogGeometry", parent),
      m_model(model), m_audio(audio), m_history(history), m_tci(tci)
{
    setMinimumSize(920, 680);
    resize(980, 760);
    bodyWidget()->setStyleSheet(QString::fromLatin1(kNetworkDiagnosticsStyle));

    auto* body = new QVBoxLayout(bodyWidget());
    body->setSpacing(8);

    // Timeframe selector lives in the top-right corner of the QTabWidget's
    // tab bar so the tabs and the dropdown share a single row, eliminating
    // the otherwise-empty band above the tabs.
    auto* rangeLabel = new QLabel("Timeframe");
    m_rangeCombo = new QComboBox(this);
    m_rangeCombo->setFixedWidth(132);
    m_rangeCombo->addItem("1 minute", 60);
    m_rangeCombo->addItem("5 minutes", 5 * 60);
    m_rangeCombo->addItem("15 minutes", 15 * 60);
    m_rangeCombo->addItem("1 hour", 60 * 60);
    m_rangeCombo->addItem("1 day", 24 * 60 * 60);
    m_rangeCombo->addItem("1 week", 7 * 24 * 60 * 60);

    auto* tabs = new QTabWidget(this);
    auto* corner = new QWidget(tabs);
    corner->setObjectName("networkDiagnosticsTimeframeCorner");
    auto* cornerRow = new QHBoxLayout(corner);
    cornerRow->setContentsMargins(0, 0, 6, 2);
    cornerRow->setSpacing(6);
    cornerRow->addWidget(rangeLabel);
    cornerRow->addWidget(m_rangeCombo);
    tabs->setCornerWidget(corner, Qt::TopRightCorner);
    body->addWidget(tabs, 1);

    auto* overviewPage = new QWidget(this);
    auto* overviewLayout = new QGridLayout(overviewPage);
    overviewLayout->setContentsMargins(8, 8, 8, 8);
    overviewLayout->setHorizontalSpacing(12);
    overviewLayout->setVerticalSpacing(12);
    overviewLayout->setColumnStretch(0, 1);
    overviewLayout->setColumnStretch(1, 1);
    overviewLayout->setColumnStretch(2, 1);
    overviewLayout->setColumnStretch(3, 1);
    tabs->addTab(overviewPage, "Overview");

    auto* detailsScroll = new QScrollArea(this);
    detailsScroll->setWidgetResizable(true);
    detailsScroll->setFrameShape(QFrame::NoFrame);
    tabs->addTab(detailsScroll, "Details");

    auto* content = new QWidget;
    detailsScroll->setWidget(content);
    auto* contentLayout = new QGridLayout(content);
    contentLayout->setContentsMargins(6, 6, 6, 6);
    contentLayout->setColumnStretch(0, 1);
    contentLayout->setColumnStretch(1, 1);
    contentLayout->setColumnMinimumWidth(0, 430);
    contentLayout->setColumnMinimumWidth(1, 430);
    contentLayout->setHorizontalSpacing(12);
    contentLayout->setVerticalSpacing(8);
    content->setMinimumWidth(900);

    auto makeVal = [](const QString& init = "") {
        static constexpr int kValueColumnWidth = 230;
        auto* l = new QLabel(init);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        l->setWordWrap(false);
        l->setMinimumWidth(kValueColumnWidth);
        l->setMaximumWidth(kValueColumnWidth);
        l->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        l->setMinimumHeight(l->fontMetrics().height() + 1);
        l->setStyleSheet("QLabel { color: #b9c4d7; font-weight: 600; }");
        return l;
    };

    auto makeDim = [&](const QString& init = "") {
        return makeVal(init);
    };

    auto makeNote = [](const QString& text) {
        auto* l = new QLabel(text);
        l->setWordWrap(true);
        l->setStyleSheet("QLabel { color: #8d99ad; font-size: 10px; line-height: 1.1; }");
        return l;
    };

    auto makeHealthCard = [](const QString& title, const QString& subtitle) {
        auto* card = makeDiagnosticsPanel(title);
        card->setMinimumHeight(96);
        auto* layout = diagnosticsPanelLayout(card);
        auto* value = new QLabel("--");
        value->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        value->setMinimumHeight(value->fontMetrics().height() + 4);
        value->setStyleSheet("QLabel { color: #d4deea; font-weight: 700; font-size: 18px; }");
        auto* hint = new QLabel(subtitle);
        hint->setWordWrap(true);
        hint->setStyleSheet("QLabel { color: #8d99ad; font-size: 11px; }");
        layout->addWidget(value);
        layout->addWidget(hint);
        layout->addStretch();
        return std::pair<QFrame*, QLabel*>{card, value};
    };

    const auto statusCard = makeHealthCard("Status", "Overall connection quality");
    m_overviewStatusValue = statusCard.second;
    overviewLayout->addWidget(statusCard.first, 0, 0);
    const auto latencyCard = makeHealthCard("Latency", "Round-trip time");
    m_overviewLatencyValue = latencyCard.second;
    overviewLayout->addWidget(latencyCard.first, 0, 1);
    const auto lossCard = makeHealthCard("Packet Loss", "Recent sequence gaps");
    m_overviewLossValue = lossCard.second;
    overviewLayout->addWidget(lossCard.first, 0, 2);
    const auto audioCard = makeHealthCard("Audio Buffer", "Current playback cushion");
    m_overviewAudioValue = audioCard.second;
    overviewLayout->addWidget(audioCard.first, 0, 3);

    // ── Network Status group ─────────────────────────────────────────────
    auto* statusGroup = makeDiagnosticsPanel("Network Status");
    statusGroup->setMinimumWidth(430);
    auto* statusGrid = new QGridLayout;
    statusGrid->setContentsMargins(0, 0, 0, 0);
    statusGrid->setColumnStretch(1, 1);
    statusGrid->setVerticalSpacing(2);
    statusGrid->setHorizontalSpacing(12);
    addDiagnosticsPanelContent(statusGroup, statusGrid);

    int row = 0;
    statusGrid->addWidget(makeNote(
        "Connection path and TCP latency. Use this to confirm the selected route "
        "to the radio is stable."), row++, 0, 1, 2);
    statusGrid->addWidget(new QLabel("Status:"), row, 0);
    m_statusLabel = makeVal();
    statusGrid->addWidget(m_statusLabel, row++, 1);

    statusGrid->addWidget(new QLabel("Target Radio IP:"), row, 0);
    m_targetIpLabel = makeVal();
    statusGrid->addWidget(m_targetIpLabel, row++, 1);

    statusGrid->addWidget(new QLabel("Selected Source:"), row, 0);
    m_sourcePathLabel = makeVal();
    m_sourcePathLabel->setWordWrap(true);
    statusGrid->addWidget(m_sourcePathLabel, row++, 1);

    statusGrid->addWidget(new QLabel("Local TCP:"), row, 0);
    m_tcpEndpointLabel = makeVal();
    statusGrid->addWidget(m_tcpEndpointLabel, row++, 1);

    statusGrid->addWidget(new QLabel("Local UDP:"), row, 0);
    m_udpEndpointLabel = makeVal();
    statusGrid->addWidget(m_udpEndpointLabel, row++, 1);

    statusGrid->addWidget(new QLabel("First UDP Packet:"), row, 0);
    m_udpSeenLabel = makeVal();
    statusGrid->addWidget(m_udpSeenLabel, row++, 1);

    statusGrid->addWidget(new QLabel("Latency (RTT):"), row, 0);
    m_rttLabel = makeVal();
    statusGrid->addWidget(m_rttLabel, row++, 1);

    statusGrid->addWidget(new QLabel("Max Latency (RTT):"), row, 0);
    m_maxRttLabel = makeVal();
    statusGrid->addWidget(m_maxRttLabel, row++, 1);

    // ── Stream Rates group ───────────────────────────────────────────────
    auto* rateGroup = makeDiagnosticsPanel("Incoming Stream Rates");
    rateGroup->setMinimumWidth(430);
    auto* rateGrid = new QGridLayout;
    rateGrid->setContentsMargins(0, 0, 0, 0);
    rateGrid->setColumnStretch(1, 1);
    rateGrid->setVerticalSpacing(2);
    rateGrid->setHorizontalSpacing(12);
    addDiagnosticsPanelContent(rateGroup, rateGrid);

    row = 0;
    rateGrid->addWidget(makeNote(
        "Current receive/transmit bitrates by stream type. Large swings can indicate "
        "bursty delivery even when no packets are lost."), row++, 0, 1, 2);
    rateGrid->addWidget(new QLabel("Audio:"), row, 0);
    m_audioRateLabel = makeVal();
    rateGrid->addWidget(m_audioRateLabel, row++, 1);

    rateGrid->addWidget(new QLabel("FFT:"), row, 0);
    m_fftRateLabel = makeVal();
    rateGrid->addWidget(m_fftRateLabel, row++, 1);

    rateGrid->addWidget(new QLabel("Waterfall:"), row, 0);
    m_wfRateLabel = makeVal();
    rateGrid->addWidget(m_wfRateLabel, row++, 1);

    rateGrid->addWidget(new QLabel("Meters:"), row, 0);
    m_meterRateLabel = makeVal();
    rateGrid->addWidget(m_meterRateLabel, row++, 1);

    rateGrid->addWidget(new QLabel("DAX:"), row, 0);
    m_daxRateLabel = makeDim();
    rateGrid->addWidget(m_daxRateLabel, row++, 1);

    rateGrid->addWidget(new QLabel("Total RX:"), row, 0);
    m_rxRateLabel = makeVal();
    rateGrid->addWidget(m_rxRateLabel, row++, 1);

    rateGrid->addWidget(new QLabel("Total TX:"), row, 0);
    m_txRateLabel = makeDim();
    rateGrid->addWidget(m_txRateLabel, row++, 1);

    // ── Packet Loss group ────────────────────────────────────────────────
    auto* dropGroup = makeDiagnosticsPanel("Packet Loss (Sequence Gaps)");
    dropGroup->setMinimumWidth(430);
    auto* dropGrid = new QGridLayout;
    dropGrid->setContentsMargins(0, 0, 0, 0);
    dropGrid->setColumnStretch(1, 1);
    dropGrid->setVerticalSpacing(2);
    dropGrid->setHorizontalSpacing(12);
    addDiagnosticsPanelContent(dropGroup, dropGrid);

    row = 0;
    dropGrid->addWidget(makeNote(
        "Inferred packet loss from missing VITA sequence numbers. Zero loss here does "
        "not rule out jitter or late bursts."), row++, 0, 1, 2);
    dropGrid->addWidget(new QLabel("Audio:"), row, 0);
    m_audioDropLabel = makeDim();
    dropGrid->addWidget(m_audioDropLabel, row++, 1);

    dropGrid->addWidget(new QLabel("FFT:"), row, 0);
    m_fftDropLabel = makeDim();
    dropGrid->addWidget(m_fftDropLabel, row++, 1);

    dropGrid->addWidget(new QLabel("Waterfall:"), row, 0);
    m_wfDropLabel = makeDim();
    dropGrid->addWidget(m_wfDropLabel, row++, 1);

    dropGrid->addWidget(new QLabel("Meters:"), row, 0);
    m_meterDropLabel = makeDim();
    dropGrid->addWidget(m_meterDropLabel, row++, 1);

    dropGrid->addWidget(new QLabel("DAX:"), row, 0);
    m_daxDropLabel = makeDim();
    dropGrid->addWidget(m_daxDropLabel, row++, 1);

    m_droppedLabel = new QLabel;
    m_droppedLabel->setAlignment(Qt::AlignCenter);
    m_droppedLabel->setWordWrap(true);
    m_droppedLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_droppedLabel->setStyleSheet("QLabel { color: #b9c4d7; font-weight: 600; }");
    dropGrid->addWidget(m_droppedLabel, row++, 0, 1, 2);

    // ── Audio Playback group ──────────────────────────────────────────────
    auto* audioGroup = makeDiagnosticsPanel("Audio Playback");
    audioGroup->setMinimumWidth(430);
    auto* audioGrid = new QGridLayout;
    audioGrid->setContentsMargins(0, 0, 0, 0);
    audioGrid->setColumnStretch(1, 1);
    audioGrid->setVerticalSpacing(2);
    audioGrid->setHorizontalSpacing(12);
    addDiagnosticsPanelContent(audioGroup, audioGrid);

    row = 0;
    audioGrid->addWidget(makeNote(
        "Radio-to-PC speaker audio health. The stream rate should stay near 24 kHz while audio is playing. "
        "The Audio tab shows per-stream rows and timing trends."),
        row++, 0, 1, 2);

    audioGrid->addWidget(new QLabel("Health:"), row, 0);
    m_audioStreamHealthLabel = makeVal();
    m_audioStreamHealthLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_audioStreamHealthLabel->setWordWrap(true);
    m_audioStreamHealthLabel->setMinimumWidth(0);
    m_audioStreamHealthLabel->setMaximumWidth(QWIDGETSIZE_MAX);
    m_audioStreamHealthLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_audioStreamHealthLabel->setToolTip(
        "Possible values:\n"
        "- Healthy: stream is current, near 24 kHz, with no recent packet gaps or repeated late arrivals.\n"
        "- Measuring RX audio: waiting for enough packets to calculate timing.\n"
        "- No recent RX audio: the stream exists but packets stopped arriving.\n"
        "- Audio stream is arriving too slowly: feed rate is low or slow-delivery delay is building.\n"
        "- Audio packets are repeatedly late: late arrivals are recurring enough to threaten the playback buffer.\n"
        "- Audio packet gaps detected: VITA sequence numbers indicate missing audio packets.\n"
        "- Playback buffer ran low recently: local output briefly ran low, but the network stream may still be healthy.");
    audioGrid->addWidget(m_audioStreamHealthLabel, row++, 1);

    audioGrid->addWidget(new QLabel("Stream Status:"), row, 0);
    m_audioStreamLabel = makeVal();
    m_audioStreamLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_audioStreamLabel->setWordWrap(true);
    m_audioStreamLabel->setMinimumWidth(0);
    m_audioStreamLabel->setMaximumWidth(QWIDGETSIZE_MAX);
    m_audioStreamLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    audioGrid->addWidget(m_audioStreamLabel, row++, 1);

    audioGrid->addWidget(new QLabel("Avg Stream Rate:"), row, 0);
    m_audioFeedRateLabel = makeVal();
    audioGrid->addWidget(m_audioFeedRateLabel, row++, 1);

    audioGrid->addWidget(new QLabel("Worst Slow Delivery:"), row, 0);
    m_audioFeedDeficitLabel = makeVal();
    audioGrid->addWidget(m_audioFeedDeficitLabel, row++, 1);

    audioGrid->addWidget(new QLabel("Late Arrivals:"), row, 0);
    m_audioLateGapLabel = makeVal();
    audioGrid->addWidget(m_audioLateGapLabel, row++, 1);

    audioGrid->addWidget(new QLabel("Support Details:"), row, 0);
    m_audioStreamsDetailLabel = new QLabel;
    m_audioStreamsDetailLabel->setWordWrap(true);
    m_audioStreamsDetailLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_audioStreamsDetailLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_audioStreamsDetailLabel->setStyleSheet("QLabel { color: #8d99ad; font-size: 10px; }");
    m_audioStreamsDetailLabel->setToolTip(
        "The radio normally sends one mixed PC speaker stream. Use the Audio tab for per-stream timing rows.");
    audioGrid->addWidget(m_audioStreamsDetailLabel, row++, 1);

    audioGrid->addWidget(new QLabel("RX Buffer Now:"), row, 0);
    m_audioBufferLabel = makeVal();
    audioGrid->addWidget(m_audioBufferLabel, row++, 1);

    audioGrid->addWidget(new QLabel("RX Buffer Peak:"), row, 0);
    m_audioBufferPeakLabel = makeVal();
    audioGrid->addWidget(m_audioBufferPeakLabel, row++, 1);

    audioGrid->addWidget(new QLabel("Playback Underruns:"), row, 0);
    m_audioUnderrunLabel = makeVal();
    audioGrid->addWidget(m_audioUnderrunLabel, row++, 1);

    audioGrid->addWidget(new QLabel("Underruns (last sec):"), row, 0);
    m_audioUnderrunRateLabel = makeVal();
    audioGrid->addWidget(m_audioUnderrunRateLabel, row++, 1);

    audioGrid->addWidget(new QLabel("Audio Arrival Gap:"), row, 0);
    m_audioPacketGapLabel = makeVal();
    audioGrid->addWidget(m_audioPacketGapLabel, row++, 1);

    audioGrid->addWidget(new QLabel("Max Arrival Gap:"), row, 0);
    m_audioPacketGapMaxLabel = makeVal();
    audioGrid->addWidget(m_audioPacketGapMaxLabel, row++, 1);

    audioGrid->addWidget(new QLabel("Network Jitter:"), row, 0);
    m_audioJitterLabel = makeVal();
    audioGrid->addWidget(m_audioJitterLabel, row++, 1);

    contentLayout->addWidget(statusGroup, 0, 0);
    contentLayout->addWidget(rateGroup, 0, 1);
    contentLayout->addWidget(dropGroup, 1, 0);
    contentLayout->addWidget(audioGroup, 1, 1);

    m_overviewLatencyGraph = new TimeSeriesGraphWidget("Latency and Jitter", " ms");
    m_overviewLossGraph = new TimeSeriesGraphWidget("Recent Packet Loss", "%");
    m_overviewRatesGraph = new TimeSeriesGraphWidget("Total Stream Rates", " kbps");
    m_overviewRatesGraph->setLogScale(true);
    m_overviewAudioGraph = new TimeSeriesGraphWidget("RX Audio Timing", " ms");
    m_overviewAudioGraph->setPrimaryAxisSeries("Buffer");
    overviewLayout->addWidget(m_overviewLatencyGraph, 1, 0, 1, 2);
    overviewLayout->addWidget(m_overviewLossGraph, 1, 2, 1, 2);
    overviewLayout->addWidget(m_overviewRatesGraph, 2, 0, 1, 2);
    overviewLayout->addWidget(m_overviewAudioGraph, 2, 2, 1, 2);

    auto makeGraphTab = [tabs](const QString& tabName, TimeSeriesGraphWidget** graph,
                               const QString& title, const QString& suffix) {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(8, 8, 8, 8);
        *graph = new TimeSeriesGraphWidget(title, suffix, page);
        layout->addWidget(*graph);
        tabs->addTab(page, tabName);
    };
    makeGraphTab("Latency", &m_latencyGraph, "Latency, Arrival Gap, and Jitter", " ms");
    makeGraphTab("Rates", &m_ratesGraph, "Incoming Stream Rates", " kbps");
    m_ratesGraph->setLogScale(true);
    makeGraphTab("Packet Loss", &m_lossGraph, "Packet Loss by Stream", "%");

    makeGraphTab("Audio", &m_audioGraph, "RX Audio Buffer and Timing", " ms");
    m_audioGraph->setPrimaryAxisSeries("Buffer");
    if (auto* audioPage = tabs->widget(tabs->count() - 1)) {
        if (auto* audioLayout = qobject_cast<QVBoxLayout*>(audioPage->layout())) {
            audioLayout->setSpacing(8);
            auto* audioNote = new QLabel(
                "PC speaker audio is normally one radio-mixed stream. If more than one transport stream appears, "
                "each row below is shown separately; slice letters list the unmuted slices feeding the speaker mix.",
                audioPage);
            audioNote->setWordWrap(true);
            audioNote->setStyleSheet("QLabel { color: #8d99ad; font-size: 11px; }");
            audioLayout->insertWidget(0, audioNote);

            m_audioStreamsTable = new QTableWidget(0, 9, audioPage);
            m_audioStreamsTable->setHorizontalHeaderLabels({
                "Stream", "Source", "Format", "Rate", "Slow Delivery",
                "Late", "Packet Gaps", "Worst Gap", "Last Packet"
            });
            m_audioStreamsTable->verticalHeader()->setVisible(false);
            m_audioStreamsTable->horizontalHeader()->setStretchLastSection(false);
            m_audioStreamsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
            m_audioStreamsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
            m_audioStreamsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
            m_audioStreamsTable->setSelectionMode(QAbstractItemView::NoSelection);
            m_audioStreamsTable->setFocusPolicy(Qt::NoFocus);
            m_audioStreamsTable->setAlternatingRowColors(true);
            m_audioStreamsTable->setMinimumHeight(136);
            m_audioStreamsTable->setMaximumHeight(190);
            audioLayout->insertWidget(1, m_audioStreamsTable);

            m_audioFeedGraph = new TimeSeriesGraphWidget("RX Audio Stream Rate vs Target", " Hz", audioPage);
            audioLayout->addWidget(m_audioFeedGraph, 1);
        }
    }

    QWidget* logsTab = buildLogsTab();
    tabs->addTab(logsTab, "Logs");

    // TCI client list — only when a TCI server instance is available.
    QWidget* tciTab = nullptr;
#ifdef HAVE_WEBSOCKETS
    if (m_tci) {
        tciTab = buildTciTab();
        tabs->addTab(tciTab, "TCI");
    }
#endif

    // ── Close button ─────────────────────────────────────────────────────
    auto* closeBtn = new QPushButton("Close");
    closeBtn->setFixedWidth(80);
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    body->addLayout(btnRow);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    // Refresh every second
    connect(m_rangeCombo, &QComboBox::currentIndexChanged, this, [this] {
        updateCharts();
    });
    connect(tabs, &QTabWidget::currentChanged, this,
            [tabs, corner, logsTab, tciTab](int index) {
        QWidget* w = tabs->widget(index);
        corner->setVisible(w != logsTab && w != tciTab);
    });
    connect(&m_refreshTimer, &QTimer::timeout, this, &NetworkDiagnosticsDialog::refresh);
    m_refreshTimer.start(1000);
    connect(&m_logRefreshTimer, &QTimer::timeout, this, &NetworkDiagnosticsDialog::appendNewLogData);
    m_logRefreshTimer.start(500);
    initializeLogTail();
    refresh();
}

#ifdef HAVE_WEBSOCKETS

// TCI carries no client-identity handshake — a client is only ever known
// by its endpoint plus the streams it subscribed to. Infer a friendly
// hint from those subscriptions rather than claiming a real identity.
static QString tciRoleHint(const TciClientInfo& c)
{
    if (c.audio) return QStringLiteral("Audio (WSJT-X / digital mode)");
    if (c.iq)    return QStringLiteral("IQ stream (panadapter / SDR client)");
    if (c.rxSensors || c.txSensors)
                 return QStringLiteral("Telemetry / monitor");
    return QStringLiteral("Control only");
}

// User-assigned client aliases are stored as ONE AppSettings value holding a
// JSON {ip: name} map. The IP must NOT be used as a settings key: AppSettings
// is XML/dotted-path backed, and an address like "10.0.0.78" is an invalid
// key (segments start with a digit, ':' in IPv6) — it gets silently dropped.
static const QString kTciAliasKey = QStringLiteral("TciClientAliases");

static QHash<QString, QString> tciAliasMap()
{
    const QString raw = AppSettings::instance()
        .value(kTciAliasKey, QString()).toString();
    QHash<QString, QString> m;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (doc.isObject()) {
        const QJsonObject o = doc.object();
        for (auto it = o.begin(); it != o.end(); ++it)
            m.insert(it.key(), it.value().toString());
    }
    return m;
}

static void tciAliasSet(const QString& ip, const QString& name)
{
    if (ip.isEmpty())
        return;
    QHash<QString, QString> m = tciAliasMap();
    if (name.isEmpty())
        m.remove(ip);
    else
        m.insert(ip, name);
    QJsonObject o;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
        o.insert(it.key(), it.value());
    AppSettings::instance().setValue(kTciAliasKey,
        QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    AppSettings::instance().save();
}

QWidget* NetworkDiagnosticsDialog::buildTciTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto* intro = new QLabel(
        "Applications currently connected to this radio's TCI server. "
        "TCI has no client-identity handshake, so clients are listed by "
        "network endpoint and the role is inferred from the streams each "
        "client has subscribed to. You can type your own label in the "
        "Name column — it is saved locally, keyed by IP address.");
    intro->setWordWrap(true);
    intro->setStyleSheet("QLabel { color: #8aa8c0; }");
    layout->addWidget(intro);

    auto* clientsHdr = new QLabel(QStringLiteral("Connected clients"));
    clientsHdr->setStyleSheet(
        "QLabel { color: #8aa8c0; font-weight: bold; "
        "letter-spacing: 0.06em; }");
    layout->addWidget(clientsHdr);

    m_tciClientSummary = new QLabel;
    m_tciClientSummary->setStyleSheet(
        "QLabel { color: #c8d8e8; font-weight: bold; }");
    layout->addWidget(m_tciClientSummary);

    m_tciClientTable = new QTableWidget(0, 7, this);
    m_tciClientTable->setHorizontalHeaderLabels(
        {QStringLiteral("Name"), QStringLiteral("Endpoint"),
         QStringLiteral("Likely role"),
         QStringLiteral("Audio"), QStringLiteral("IQ"),
         QStringLiteral("RX sensors"), QStringLiteral("TX sensors")});
    m_tciClientTable->verticalHeader()->setVisible(false);
    // Only the Name column is editable (per-item flags set in
    // refreshTciClientTable); double-click or F2 to rename.
    m_tciClientTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                      QAbstractItemView::EditKeyPressed);
    m_tciClientTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tciClientTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    auto* hh = m_tciClientTable->horizontalHeader();
    hh->setStretchLastSection(false);
    hh->setSectionResizeMode(0, QHeaderView::Stretch);            // Name
    hh->setSectionResizeMode(1, QHeaderView::ResizeToContents);   // Endpoint
    hh->setSectionResizeMode(2, QHeaderView::Stretch);            // Likely role
    for (int c = 3; c < 7; ++c)
        hh->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_tciClientTable->setStyleSheet(
        "QTableWidget { background: #0a0a14; color: #c8d8e8; "
        "gridline-color: #203040; border: 1px solid #203040; }"
        "QTableWidget::item:selected { background: #173049; }"
        "QHeaderView::section { background: #111120; color: #8aa8c0; "
        "border: 1px solid #203040; padding: 3px 6px; font-weight: bold; }");
    // Keep the roster compact (~6 rows then scroll) so the monitor below
    // gets the bulk of the page.
    m_tciClientTable->setSizePolicy(QSizePolicy::Expanding,
                                    QSizePolicy::Fixed);
    m_tciClientTable->setMaximumHeight(196);
    layout->addWidget(m_tciClientTable, 0);

    // Persist a user-assigned alias when the Name cell is edited (stored in
    // the JSON map under one valid key, keyed by client IP held in UserRole).
    // itemChanged also fires during programmatic repopulation —
    // refreshTciClientTable blocks this signal while it runs, so anything
    // arriving here is a genuine user edit.
    connect(m_tciClientTable, &QTableWidget::itemChanged, this,
            [](QTableWidgetItem* item) {
        if (!item || item->column() != 0)
            return;
        tciAliasSet(item->data(Qt::UserRole).toString(),
                    item->text().trimmed());
    });

    refreshTciClientTable();

    // Live updates while the dialog is open. Both ends are QObjects, so Qt
    // tears the connection down automatically if either is destroyed.
    if (m_tci)
        connect(m_tci, &TciServer::clientsChanged,
                this, &NetworkDiagnosticsDialog::refreshTciClientTable);

    // ── Live traffic monitor ──────────────────────────────────────────────
    auto* monHdr = new QLabel(QStringLiteral("TCI traffic monitor"));
    monHdr->setStyleSheet(
        "QLabel { color: #8aa8c0; font-weight: bold; "
        "letter-spacing: 0.06em; }");
    layout->addWidget(monHdr);

    auto* mHelp = new QLabel(QStringLiteral(
        "Live TCI traffic (both directions). Pause, then right-click a row "
        "to suppress all messages of that command — same as the standalone "
        "TCI Monitor."));
    mHelp->setWordWrap(true);
    mHelp->setStyleSheet("QLabel { color: #8aa8c0; }");
    layout->addWidget(mHelp);

    const QString btnStyle = QStringLiteral(
        "QPushButton { background: #111120; border: 1px solid #203040; "
        "border-radius: 3px; color: #c8d8e8; padding: 3px 12px; }"
        "QPushButton:hover { border-color: #00b4d8; }"
        "QPushButton:checked { background: #003040; border-color: #ffaa00; "
        "color: #ffaa00; }");

    auto* tools = new QHBoxLayout;
    tools->setSpacing(6);

    m_tciPauseBtn = new QPushButton(QStringLiteral("Pause"));
    m_tciPauseBtn->setCheckable(true);
    m_tciPauseBtn->setStyleSheet(btnStyle);
    connect(m_tciPauseBtn, &QPushButton::toggled, this, [this](bool on) {
        m_tciMonitorPaused = on;
        m_tciPauseBtn->setText(on ? QStringLiteral("Paused")
                                  : QStringLiteral("Pause"));
    });
    tools->addWidget(m_tciPauseBtn);

    auto* clearBtn = new QPushButton(QStringLiteral("Clear"));
    clearBtn->setStyleSheet(btnStyle);
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        if (m_tciLogTable) m_tciLogTable->setRowCount(0);
    });
    tools->addWidget(clearBtn);

    auto* saveBtn = new QPushButton(QStringLiteral("Save log…"));
    saveBtn->setStyleSheet(btnStyle);
    connect(saveBtn, &QPushButton::clicked,
            this, &NetworkDiagnosticsDialog::onTciSaveLog);
    tools->addWidget(saveBtn);

    tools->addStretch(1);
    m_tciSuppressLabel = new QLabel;
    m_tciSuppressLabel->setStyleSheet("QLabel { color: #ffaa00; }");
    tools->addWidget(m_tciSuppressLabel);
    auto* clrSupBtn = new QPushButton(QStringLiteral("Clear suppressions"));
    clrSupBtn->setStyleSheet(btnStyle);
    connect(clrSupBtn, &QPushButton::clicked, this, [this] {
        m_tciSuppressed.clear();
        AppSettings::instance().setValue(
            QStringLiteral("TciMonitorSuppressed"), QString());
        AppSettings::instance().save();
        refreshTciSuppressLabel();
    });
    tools->addWidget(clrSupBtn);
    layout->addLayout(tools);

    // Restore persisted suppressions (JSON array under one valid key).
    {
        const QJsonDocument d = QJsonDocument::fromJson(
            AppSettings::instance()
                .value(QStringLiteral("TciMonitorSuppressed"), QString())
                .toString().toUtf8());
        if (d.isArray())
            for (const QJsonValue& v : d.array())
                m_tciSuppressed.insert(v.toString());
    }

    m_tciLogTable = new QTableWidget(0, 3, this);
    m_tciLogTable->setHorizontalHeaderLabels(
        {QStringLiteral("Time"), QStringLiteral("Dir/Cmd"),
         QStringLiteral("Message")});
    m_tciLogTable->verticalHeader()->setVisible(false);
    m_tciLogTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tciLogTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tciLogTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tciLogTable->setShowGrid(false);
    m_tciLogTable->horizontalHeader()->setStretchLastSection(true);
    m_tciLogTable->setColumnWidth(0, 96);
    m_tciLogTable->setColumnWidth(1, 150);
    m_tciLogTable->setStyleSheet(
        "QTableWidget { background: #07070e; color: #dde6f0; "
        "border: 1px solid #203040; font-family: Consolas, monospace; "
        "font-size: 11px; gridline-color: transparent; }"
        "QTableWidget::item:selected { background: #173049; }"
        "QHeaderView::section { background: #111120; color: #8aa8c0; "
        "border: 1px solid #203040; padding: 2px 6px; }");
    m_tciLogTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tciLogTable, &QTableWidget::customContextMenuRequested,
            this, &NetworkDiagnosticsDialog::onTciLogContextMenu);
    layout->addWidget(m_tciLogTable, 1);

    refreshTciSuppressLabel();

    if (m_tci)
        connect(m_tci, &TciServer::tciMessage,
                this, &NetworkDiagnosticsDialog::appendTciMessage);

    return page;
}

void NetworkDiagnosticsDialog::refreshTciClientTable()
{
    if (!m_tci || !m_tciClientTable)
        return;

    const QVector<TciClientInfo> list = m_tci->connectedClients();

    // Repopulating fires itemChanged for every setItem — suppress it so the
    // alias-persist handler only ever sees real user edits.
    const QHash<QString, QString> aliases = tciAliasMap();

    QSignalBlocker block(m_tciClientTable);
    m_tciClientTable->setRowCount(list.size());

    auto readOnly = [](const QString& text, bool centre = false) {
        auto* it = new QTableWidgetItem(text);
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        if (centre)
            it->setTextAlignment(Qt::AlignCenter);
        return it;
    };

    for (int r = 0; r < list.size(); ++r) {
        const TciClientInfo& c = list[r];

        auto* nameItem = new QTableWidgetItem(aliases.value(c.peerAddress));
        nameItem->setData(Qt::UserRole, c.peerAddress);
        nameItem->setFlags(nameItem->flags() | Qt::ItemIsEditable);
        nameItem->setToolTip(QStringLiteral(
            "Your own label for this client (saved locally, keyed by IP)"));
        m_tciClientTable->setItem(r, 0, nameItem);

        m_tciClientTable->setItem(r, 1, readOnly(
            c.peerAddress + QStringLiteral(":") + QString::number(c.peerPort)));
        m_tciClientTable->setItem(r, 2, readOnly(tciRoleHint(c)));
        const QString audio = c.audio
            ? (c.audioReceiver < 0
                   ? QStringLiteral("Yes (all RX)")
                   : QStringLiteral("Yes (RX %1)").arg(c.audioReceiver))
            : QStringLiteral("—");
        m_tciClientTable->setItem(r, 3, readOnly(audio, true));
        m_tciClientTable->setItem(r, 4, readOnly(c.iq        ? QStringLiteral("Yes") : QStringLiteral("—"), true));
        m_tciClientTable->setItem(r, 5, readOnly(c.rxSensors ? QStringLiteral("Yes") : QStringLiteral("—"), true));
        m_tciClientTable->setItem(r, 6, readOnly(c.txSensors ? QStringLiteral("Yes") : QStringLiteral("—"), true));
    }

    const int n = list.size();
    m_tciClientSummary->setText(
        n == 0 ? QStringLiteral("No TCI clients connected")
               : QStringLiteral("%1 client%2 connected")
                     .arg(n).arg(n == 1 ? QString() : QStringLiteral("s")));
}

// Colour a raw TCI line by its command family — same palette as the
// standalone TCI Monitor so the embedded view reads identically.
static QString tciLineColor(const QString& line)
{
    const int colon = line.indexOf(QLatin1Char(':'));
    const QString cmd =
        (colon < 0 ? line : line.left(colon)).trimmed().toLower();
    if (cmd == "vfo")                              return QStringLiteral("#00d8ef");
    if (cmd == "mode" || cmd == "modulation")      return QStringLiteral("#00d8ef");
    if (cmd == "spot")                             return QStringLiteral("#4cff7c");
    if (cmd == "spot_delete" || cmd == "spot_clear") return QStringLiteral("#ffaa00");
    if (cmd == "start" || cmd == "ready" || cmd == "protocol")
                                                   return QStringLiteral("#6b8099");
    if (cmd.contains("error"))                     return QStringLiteral("#ff5050");
    return QStringLiteral("#dde6f0");
}

static QString tciCmdOf(const QString& line)
{
    const int colon = line.indexOf(QLatin1Char(':'));
    return (colon < 0 ? line : line.left(colon)).trimmed().toLower();
}

void NetworkDiagnosticsDialog::appendTciMessage(const QString& direction,
                                                const QString& text)
{
    if (!m_tciLogTable || m_tciMonitorPaused)
        return;

    const QString stamp =
        QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const bool rx = (direction == QLatin1String("rx"));
    const QString dirTag = rx ? QStringLiteral("▶ ")   // ▶
                              : QStringLiteral("◀ ");   // ◀

    // One row per semicolon-delimited command so colour + suppression work
    // per command, exactly like the standalone monitor.
    const QStringList cmds = text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& raw : cmds) {
        const QString cmd = raw.trimmed();
        if (cmd.isEmpty())
            continue;
        const QString prefix = tciCmdOf(cmd);
        if (m_tciSuppressed.contains(prefix))
            continue;

        constexpr int kMaxRows = 20000;   // rolling window, bound memory
        if (m_tciLogTable->rowCount() >= kMaxRows)
            m_tciLogTable->removeRow(0);

        const int row = m_tciLogTable->rowCount();
        m_tciLogTable->insertRow(row);
        const QColor colour(tciLineColor(cmd));

        auto* tItem = new QTableWidgetItem(stamp);
        tItem->setForeground(QColor(QStringLiteral("#6b8099")));
        auto* cItem = new QTableWidgetItem(dirTag + prefix);
        cItem->setForeground(rx ? QColor(QStringLiteral("#9cff00"))
                                : QColor(QStringLiteral("#7c9cff")));
        auto* mItem = new QTableWidgetItem(cmd);
        mItem->setForeground(colour);
        m_tciLogTable->setItem(row, 0, tItem);
        m_tciLogTable->setItem(row, 1, cItem);
        m_tciLogTable->setItem(row, 2, mItem);
    }
    m_tciLogTable->scrollToBottom();
}

void NetworkDiagnosticsDialog::onTciLogContextMenu(const QPoint& pos)
{
    if (!m_tciLogTable)
        return;
    const QModelIndex idx = m_tciLogTable->indexAt(pos);
    const auto sel = m_tciLogTable->selectionModel()->selectedRows();

    QString cmdHere;
    if (idx.isValid()) {
        if (auto* it = m_tciLogTable->item(idx.row(), 2))
            cmdHere = tciCmdOf(it->text());
    }

    QMenu menu(this);
    QAction* removeAct = nullptr;
    if (!sel.isEmpty())
        removeAct = menu.addAction(sel.size() == 1
            ? QStringLiteral("Remove this row")
            : QStringLiteral("Remove %1 selected rows").arg(sel.size()));
    QAction* suppressAct = nullptr;
    if (!cmdHere.isEmpty())
        suppressAct = menu.addAction(
            QStringLiteral("Suppress all '%1' messages").arg(cmdHere));
    if (!menu.actions().isEmpty())
        menu.addSeparator();
    QAction* clearAct = menu.addAction(QStringLiteral("Clear log"));

    QAction* chosen = menu.exec(m_tciLogTable->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == clearAct) {
        m_tciLogTable->setRowCount(0);
    } else if (removeAct && chosen == removeAct) {
        QList<int> rows;
        for (const QModelIndex& i : sel) rows << i.row();
        std::sort(rows.begin(), rows.end(), std::greater<int>());
        for (int r : rows) m_tciLogTable->removeRow(r);
    } else if (suppressAct && chosen == suppressAct) {
        tciSuppress(cmdHere);
    }
}

void NetworkDiagnosticsDialog::tciSuppress(const QString& cmd)
{
    if (cmd.isEmpty() || !m_tciLogTable)
        return;
    m_tciSuppressed.insert(cmd);
    // Drop existing rows of this command for instant relief.
    for (int r = m_tciLogTable->rowCount() - 1; r >= 0; --r) {
        auto* it = m_tciLogTable->item(r, 2);
        if (it && tciCmdOf(it->text()) == cmd)
            m_tciLogTable->removeRow(r);
    }
    // Serialise sorted so the on-disk JSON is stable across saves
    // (QSet iteration order is not deterministic).
    QStringList sorted(m_tciSuppressed.begin(), m_tciSuppressed.end());
    sorted.sort();
    QJsonArray arr;
    for (const QString& s : sorted) arr.append(s);
    AppSettings::instance().setValue(QStringLiteral("TciMonitorSuppressed"),
        QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    AppSettings::instance().save();
    refreshTciSuppressLabel();
}

void NetworkDiagnosticsDialog::refreshTciSuppressLabel()
{
    if (!m_tciSuppressLabel)
        return;
    if (m_tciSuppressed.isEmpty()) {
        m_tciSuppressLabel->clear();
        return;
    }
    QStringList s(m_tciSuppressed.begin(), m_tciSuppressed.end());
    s.sort();
    m_tciSuppressLabel->setText(
        QStringLiteral("Suppressed: %1").arg(s.join(QStringLiteral(", "))));
}

void NetworkDiagnosticsDialog::onTciSaveLog()
{
    if (!m_tciLogTable)
        return;
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString suggested =
        QStringLiteral("%1/tci-monitor-%2.log").arg(dir, stamp);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save TCI log"), suggested,
        QStringLiteral("Log files (*.log);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "TCI monitor: cannot write" << path;
        return;
    }
    QTextStream ts(&f);
    for (int r = 0; r < m_tciLogTable->rowCount(); ++r) {
        const auto* t = m_tciLogTable->item(r, 0);
        const auto* c = m_tciLogTable->item(r, 1);
        const auto* m = m_tciLogTable->item(r, 2);
        ts << (t ? t->text() : QString()) << '\t'
           << (c ? c->text() : QString()) << '\t'
           << (m ? m->text() : QString()) << '\n';
    }
}

#else  // !HAVE_WEBSOCKETS — keep symbols defined for the linker

QWidget* NetworkDiagnosticsDialog::buildTciTab() { return new QWidget(this); }
void     NetworkDiagnosticsDialog::refreshTciClientTable() {}
void     NetworkDiagnosticsDialog::appendTciMessage(const QString&,
                                                    const QString&) {}
void     NetworkDiagnosticsDialog::onTciSaveLog() {}
void     NetworkDiagnosticsDialog::onTciLogContextMenu(const QPoint&) {}
void     NetworkDiagnosticsDialog::tciSuppress(const QString&) {}
void     NetworkDiagnosticsDialog::refreshTciSuppressLabel() {}

#endif

QWidget* NetworkDiagnosticsDialog::buildLogsTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto* filterGroup = makeDiagnosticsPanel("Filter Categories", page);
    auto* filterGrid = new QGridLayout;
    filterGrid->setContentsMargins(0, 0, 0, 0);
    filterGrid->setHorizontalSpacing(12);
    filterGrid->setVerticalSpacing(4);
    addDiagnosticsPanelContent(filterGroup, filterGrid);

    auto* allCategories = new QCheckBox("General", filterGroup);
    allCategories->setProperty("logCategory", QStringLiteral("default"));
    allCategories->setChecked(true);
    allCategories->setToolTip("Uncategorized Qt log output");
    filterGrid->addWidget(allCategories, 0, 0);
    m_logCategoryCheckboxes.push_back(allCategories);
    m_visibleLogCategories.insert(QStringLiteral("default"));
    connect(allCategories, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            m_visibleLogCategories.insert(QStringLiteral("default"));
        } else {
            m_visibleLogCategories.remove(QStringLiteral("default"));
        }
        rebuildLogView();
    });

    const QList<LogManager::Category> categories = LogManager::instance().categories();
    constexpr int kColumns = 4;
    int index = 1;
    for (const LogManager::Category& category : categories) {
        auto* checkbox = new QCheckBox(category.label, filterGroup);
        checkbox->setProperty("logCategory", category.id);
        checkbox->setChecked(true);
        checkbox->setToolTip(QString("%1\nCategory: %2").arg(category.description, category.id));
        m_logCategoryCheckboxes.push_back(checkbox);
        m_visibleLogCategories.insert(category.id);
        filterGrid->addWidget(checkbox, index / kColumns, index % kColumns);
        connect(checkbox, &QCheckBox::toggled, this, [this, id = category.id](bool on) {
            if (on) {
                m_visibleLogCategories.insert(id);
            } else {
                m_visibleLogCategories.remove(id);
            }
            rebuildLogView();
        });
        ++index;
    }
    layout->addWidget(filterGroup);

    auto* filterButtonRow = new QHBoxLayout;
    auto* selectAllBtn = new QPushButton("Select All", page);
    auto* deselectAllBtn = new QPushButton("Deselect All", page);
    selectAllBtn->setMinimumHeight(32);
    deselectAllBtn->setMinimumHeight(32);
    connect(selectAllBtn, &QPushButton::clicked, this, [this]() {
        setAllLogCategoriesVisible(true);
    });
    connect(deselectAllBtn, &QPushButton::clicked, this, [this]() {
        setAllLogCategoriesVisible(false);
    });
    filterButtonRow->addWidget(selectAllBtn);
    filterButtonRow->addWidget(deselectAllBtn);
    filterButtonRow->addStretch();
    layout->addLayout(filterButtonRow);

    auto* infoRow = new QHBoxLayout;
    m_logPathLabel = new QLabel(page);
    m_logPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_logPathLabel->setStyleSheet("QLabel { color: #8d99ad; font-size: 11px; }");
    infoRow->addWidget(m_logPathLabel, 1);

    m_logLiveToggle = new QPushButton("Live", page);
    m_logLiveToggle->setCheckable(true);
    m_logLiveToggle->setChecked(true);
    m_logLiveToggle->setFixedWidth(92);
    m_logLiveToggle->setToolTip("Live follows the newest log output. Turn it off to inspect older lines.");
    connect(m_logLiveToggle, &QPushButton::toggled, this, [this](bool live) {
        setLogFollowLive(live);
    });
    infoRow->addWidget(m_logLiveToggle);
    layout->addLayout(infoRow);

    m_logViewer = new QPlainTextEdit(page);
    m_logViewer->setReadOnly(true);
    m_logViewer->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logViewer->setMaximumBlockCount(2500);
    new LogSyntaxHighlighter(m_logViewer->document());
    layout->addWidget(m_logViewer, 1);

    connect(m_logViewer->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (m_handlingLogScroll || !m_logViewer) {
            return;
        }
        if (value < m_logViewer->verticalScrollBar()->maximum()) {
            setLogFollowLive(false);
        }
    });

    return page;
}

void NetworkDiagnosticsDialog::initializeLogTail()
{
    const QString logPath = LogManager::instance().logFilePath();
    if (m_logPathLabel) {
        m_logPathLabel->setText(QString("Log: %1").arg(logPath));
    }

    m_logFile.close();
    m_logFile.setFileName(logPath);
    m_logOffset = 0;
    m_logPartialLine.clear();
    m_logLines.clear();

    if (!reopenLogFile(false)) {
        rebuildLogView();
        return;
    }

    static constexpr qint64 kInitialTailBytes = 128 * 1024;
    const qint64 size = m_logFile.size();
    if (size > kInitialTailBytes) {
        m_logFile.seek(size - kInitialTailBytes);
        m_logFile.readLine();
    }
    m_logOffset = m_logFile.pos();
    appendNewLogData();
}

bool NetworkDiagnosticsDialog::reopenLogFile(bool keepExistingLines)
{
    const QString logPath = LogManager::instance().logFilePath();
    if (m_logPathLabel) {
        m_logPathLabel->setText(QString("Log: %1").arg(logPath));
    }

    m_logFile.close();
    m_logFile.setFileName(logPath);
    m_logOffset = 0;
    m_logPartialLine.clear();
    if (!keepExistingLines) {
        m_logLines.clear();
        if (m_logViewer) {
            m_logViewer->clear();
        }
    }

    if (!m_logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (m_lastReopenFailurePath != logPath) {
            addLogLine(QString("[--:--:--.---] WRN default: Unable to open log file: %1").arg(logPath));
            m_lastReopenFailurePath = logPath;
        }
        return false;
    }

    m_lastReopenFailurePath.clear();
    return true;
}

void NetworkDiagnosticsDialog::appendNewLogData()
{
    if (!m_logViewer) {
        return;
    }

    const QString currentLogPath = LogManager::instance().logFilePath();
    if (!m_logFile.isOpen() || m_logFile.fileName() != currentLogPath) {
        if (!reopenLogFile(true)) {
            rebuildLogView();
            return;
        }
    }

    const QFileInfo pathInfo(currentLogPath);
    if (!pathInfo.exists()) {
        if (!reopenLogFile(true)) {
            rebuildLogView();
            return;
        }
    }

    const qint64 size = pathInfo.exists() ? pathInfo.size() : m_logFile.size();
    if (size < m_logOffset) {
        if (!reopenLogFile(false)) {
            rebuildLogView();
            return;
        }
        m_logFile.seek(0);
        m_logOffset = 0;
        m_logPartialLine.clear();
        addLogLine(QString("[--:--:--.---] INF default: Log file was reset; following from the beginning"));
    } else if (m_logFile.pos() != m_logOffset) {
        m_logFile.seek(m_logOffset);
    }

    const QByteArray bytes = m_logFile.readAll();
    if (bytes.isEmpty()) {
        return;
    }
    m_logOffset = m_logFile.pos();
    appendLogText(QString::fromUtf8(bytes));
}

void NetworkDiagnosticsDialog::appendLogText(const QString& text)
{
    QString pending = QString::fromUtf8(m_logPartialLine) + text;
    m_logPartialLine.clear();

    int start = 0;
    while (start < pending.size()) {
        const int newline = pending.indexOf('\n', start);
        if (newline < 0) {
            m_logPartialLine = pending.mid(start).toUtf8();
            break;
        }
        addLogLine(pending.mid(start, newline - start).trimmed());
        start = newline + 1;
    }
}

void NetworkDiagnosticsDialog::addLogLine(const QString& line)
{
    if (line.isEmpty()) {
        return;
    }

    const LogLine logLine{line, logCategoryFromLine(line)};
    m_logLines.push_back(logLine);
    static constexpr qsizetype kMaxStoredLines = 5000;
    while (m_logLines.size() > kMaxStoredLines) {
        m_logLines.removeFirst();
    }

    if (!m_logFollowLive || !logLineVisible(logLine) || !m_logViewer) {
        return;
    }

    m_logViewer->appendPlainText(logLine.text);
    m_handlingLogScroll = true;
    m_logViewer->verticalScrollBar()->setValue(m_logViewer->verticalScrollBar()->maximum());
    m_handlingLogScroll = false;
}

void NetworkDiagnosticsDialog::rebuildLogView()
{
    if (!m_logViewer) {
        return;
    }

    const bool wasFollowing = m_logFollowLive;
    m_handlingLogScroll = true;
    m_logViewer->clear();
    QStringList visibleLines;
    visibleLines.reserve(m_logLines.size());
    for (const LogLine& line : m_logLines) {
        if (logLineVisible(line)) {
            visibleLines.push_back(line.text);
        }
    }
    m_logViewer->setPlainText(visibleLines.join('\n'));
    if (wasFollowing) {
        m_logViewer->verticalScrollBar()->setValue(m_logViewer->verticalScrollBar()->maximum());
    }
    m_handlingLogScroll = false;
}

bool NetworkDiagnosticsDialog::logLineVisible(const LogLine& line) const
{
    return m_visibleLogCategories.contains(line.category);
}

QString NetworkDiagnosticsDialog::logCategoryFromLine(const QString& line) const
{
    static const QRegularExpression categoryRe(QStringLiteral("^\\[[^\\]]+\\]\\s+\\S+\\s+([^:]+):"));
    const QRegularExpressionMatch match = categoryRe.match(line);
    if (!match.hasMatch()) {
        return QStringLiteral("default");
    }

    QString category = match.captured(1).trimmed();
    if (category.isEmpty()) {
        return QStringLiteral("default");
    }
    return category;
}

void NetworkDiagnosticsDialog::setLogFollowLive(bool on)
{
    m_logFollowLive = on;
    if (m_logLiveToggle) {
        const QSignalBlocker blocker(m_logLiveToggle);
        m_logLiveToggle->setChecked(on);
        m_logLiveToggle->setText(on ? "Live" : "Paused");
        m_logLiveToggle->setToolTip(on
            ? "Live follows the newest log output. Turn it off to inspect older lines."
            : "Paused. Turn Live back on to jump to the newest log output.");
    }
    if (on && m_logViewer) {
        rebuildLogView();
        m_handlingLogScroll = true;
        m_logViewer->verticalScrollBar()->setValue(m_logViewer->verticalScrollBar()->maximum());
        m_handlingLogScroll = false;
    }
}

void NetworkDiagnosticsDialog::setAllLogCategoriesVisible(bool visible)
{
    m_visibleLogCategories.clear();
    for (QCheckBox* checkbox : m_logCategoryCheckboxes) {
        if (!checkbox) {
            continue;
        }
        const QString category = checkbox->property("logCategory").toString();
        if (visible && !category.isEmpty()) {
            m_visibleLogCategories.insert(category);
        }
        const QSignalBlocker blocker(checkbox);
        checkbox->setChecked(visible);
    }
    rebuildLogView();
}

static QString formatDrop(const PanadapterStream::CategoryStats& cs)
{
    if (cs.packets == 0) {
        return "0 / 0";
    }
    const double pct = (cs.errors * 100.0) / cs.packets;
    return QString("%1 / %2 (%3%)").arg(cs.errors).arg(cs.packets).arg(pct, 0, 'f', 2);
}

static double lossPercent(const PanadapterStream::CategoryStats& cs)
{
    if (cs.packets <= 0) {
        return 0.0;
    }
    return (cs.errors * 100.0) / cs.packets;
}

static double kbpsFromBytes(qint64 bytesDelta, double elapsedSeconds)
{
    if (bytesDelta <= 0 || elapsedSeconds <= 0.0) {
        return 0.0;
    }
    return (bytesDelta * 8.0) / (1000.0 * elapsedSeconds);
}

static double audioBufferMs(qsizetype bytes, int sampleRate)
{
    if (sampleRate <= 0) {
        return 0.0;
    }

    static constexpr int kStereoChannels = 2;
    static constexpr int kFloatBytesPerSample = 4;
    return (bytes * 1000.0) / (sampleRate * kStereoChannels * kFloatBytesPerSample);
}

static QString formatAudioBuffer(qsizetype bytes, int sampleRate)
{
    if (sampleRate <= 0) {
        return QString("%1 bytes").arg(bytes);
    }

    const double ms = audioBufferMs(bytes, sampleRate);
    return QString("%1 bytes (%2 ms)").arg(bytes).arg(ms, 0, 'f', 1);
}

static QString formatMsValue(int value)
{
    return value < 1 ? "< 1 ms" : QString("%1 ms").arg(value);
}

static QString formatPacketClassCode(quint16 pcc)
{
    return QStringLiteral("0x%1")
        .arg(QString::number(pcc, 16).toUpper().rightJustified(4, QLatin1Char('0')));
}

static QString formatAudioCodec(quint16 pcc)
{
    switch (pcc) {
    case 0x03E3u:
        return "Stereo PCM";
    case 0x0123u:
        return "Reduced PCM";
    case 0x8005u:
        return "Opus";
    default:
        return "Unknown audio";
    }
}

static QString formatStreamId(quint32 streamId)
{
    return QStringLiteral("0x%1")
        .arg(QString::number(streamId, 16).toUpper().rightJustified(8, QLatin1Char('0')));
}

static QString formatFeedRate(double hz, int streamCount = 1)
{
    if (hz <= 0.0) {
        return "Waiting for audio";
    }
    QString suffix;
    if (streamCount > 1) {
        suffix = " avg / stream";
    }
    if (hz >= 1000.0) {
        return QString("%1 kHz%2").arg(hz / 1000.0, 0, 'f', 1).arg(suffix);
    }
    return QString("%1 Hz%2").arg(hz, 0, 'f', 1).arg(suffix);
}

static QString formatFeedDeficit(double deficitMs)
{
    if (std::abs(deficitMs) < 1.0) {
        return "On time";
    }
    if (deficitMs < 0.0) {
        return QString("%1 ms ahead").arg(std::abs(deficitMs), 0, 'f', 1);
    }
    return QString("%1 ms behind").arg(deficitMs, 0, 'f', 1);
}

static QString formatLateArrivals(qint64 latePackets, double latePacketsPerSecond)
{
    if (latePackets <= 0 && latePacketsPerSecond <= 0.0) {
        return "None";
    }
    return QString("%1 late (%2/s)")
        .arg(latePackets)
        .arg(latePacketsPerSecond, 0, 'f', 1);
}

static QStringList audibleSliceLabels(const RadioModel* model)
{
    QStringList labels;
    if (!model) {
        return labels;
    }

    const QList<SliceModel*> slices = model->slices();
    for (const SliceModel* slice : slices) {
        if (!slice || slice->audioMute() || slice->audioGain() <= 0.5f) {
            continue;
        }
        labels.push_back(slice->letter());
    }
    labels.removeDuplicates();
    std::sort(labels.begin(), labels.end(), [](const QString& a, const QString& b) {
        return QString::localeAwareCompare(a, b) < 0;
    });
    return labels;
}

static QString formatSliceMix(const QStringList& sliceLabels)
{
    if (sliceLabels.isEmpty()) {
        return "no unmuted slices";
    }
    return QString("slice%1 %2")
        .arg(sliceLabels.size() == 1 ? "" : "s")
        .arg(sliceLabels.join(", "));
}

static QString formatAudioStream(const NetworkDiagnosticsSample& sample,
                                 const QStringList& sliceLabels)
{
    if (sample.audioStreamCount <= 0) {
        return "Waiting for RX audio";
    }

    const QString streamText = QString("%1 speaker stream%2")
        .arg(sample.audioStreamCount)
        .arg(sample.audioStreamCount == 1 ? "" : "s");
    return QString("%1 carrying %2, %3")
        .arg(streamText,
             formatSliceMix(sliceLabels),
             formatAudioCodec(sample.audioPacketClassCode));
}

static QString formatAudioSupportDetails(int streamCount, const QStringList& sliceLabels)
{
    if (streamCount <= 0) {
        return "No RX audio packets seen yet.";
    }
    return QString("The radio sends PC speaker audio as a mixed remote_audio_rx stream. "
                   "Slice audio is mixed before the packet stream, so per-slice packet timing is not available here. "
                   "The Audio tab shows the transport stream rows; current mix: %1.")
        .arg(formatSliceMix(sliceLabels));
}

static QString formatAudioStreamSource(const QStringList& sliceLabels)
{
    if (sliceLabels.isEmpty()) {
        return "Speaker mix (no unmuted slices)";
    }
    return QString("Speaker mix: %1").arg(formatSliceMix(sliceLabels));
}

static QString formatAudioStreamFormat(quint16 pcc)
{
    return QString("%1 (%2)")
        .arg(formatAudioCodec(pcc),
             formatPacketClassCode(pcc));
}

enum class AudioHealthState {
    Waiting,
    NoRecentAudio,
    Measuring,
    SlowDelivery,
    RepeatedLatePackets,
    PacketGaps,
    BufferLow,
    Healthy,
};

struct AudioHealthStatus {
    AudioHealthState state{AudioHealthState::Waiting};
    QString text;
};

static AudioHealthStatus formatAudioHealth(const NetworkDiagnosticsSample& sample)
{
    if (sample.audioStreamCount <= 0) {
        return {AudioHealthState::Waiting, "Waiting for RX audio"};
    }
    if (sample.audioLastPacketAgeMs > 1000) {
        return {AudioHealthState::NoRecentAudio, "No recent RX audio"};
    }
    if (sample.audioFeedRateHz <= 0.0) {
        return {AudioHealthState::Measuring, "Measuring RX audio"};
    }

    const bool underfed = sample.audioFeedDeficitMs >= 20.0
        || sample.audioFeedRateHz < AudioEngine::DEFAULT_SAMPLE_RATE * 0.95;
    if (underfed) {
        return {AudioHealthState::SlowDelivery, "Audio stream is arriving too slowly"};
    }
    const bool recurringLatePackets = sample.audioLatePacketsPerSecond >= 2.0
        || (sample.audioLatePacketsPerSecond >= 1.0 && sample.audioFeedDeficitMs >= 10.0);
    if (recurringLatePackets) {
        return {AudioHealthState::RepeatedLatePackets, "Audio packets are repeatedly late"};
    }
    if (sample.audioPacketGaps > 0) {
        return {AudioHealthState::PacketGaps, "Audio packet gaps detected"};
    }
    if (sample.underrunsPerSecond > 0.0) {
        return {AudioHealthState::BufferLow, "Playback buffer ran low recently"};
    }
    return {AudioHealthState::Healthy, "Healthy"};
}

static QString audioHealthStyle(AudioHealthState state)
{
    switch (state) {
    case AudioHealthState::Healthy:
        return "QLabel { color: #64d36e; font-weight: 700; }";
    case AudioHealthState::SlowDelivery:
    case AudioHealthState::PacketGaps:
        return "QLabel { color: #ff6b6b; font-weight: 700; }";
    case AudioHealthState::Waiting:
    case AudioHealthState::NoRecentAudio:
    case AudioHealthState::Measuring:
        return "QLabel { color: #8d99ad; font-weight: 700; }";
    case AudioHealthState::RepeatedLatePackets:
    case AudioHealthState::BufferLow:
        return "QLabel { color: #e8b977; font-weight: 700; }";
    }
    return "QLabel { color: #8d99ad; font-weight: 700; }";
}

NetworkDiagnosticsHistory::NetworkDiagnosticsHistory(RadioModel* model, AudioEngine* audio, QObject* parent)
    : QObject(parent)
    , m_model(model)
    , m_audio(audio)
{
    m_lastRxBytes = m_model->rxBytes();
    m_lastTxBytes = m_model->txBytes();
    m_lastSampleMs = QDateTime::currentMSecsSinceEpoch();
    m_lastAudioUnderrunCount = m_audio ? m_audio->rxBufferUnderrunCount() : 0;
    for (int i = 0; i < PanadapterStream::CatCount; ++i) {
        m_lastCatBytes[i] = m_model->categoryStats(static_cast<PanadapterStream::StreamCategory>(i)).bytes;
    }

    connect(&m_sampleTimer, &QTimer::timeout, this, [this] {
        sampleNow();
    });
    m_sampleTimer.start(1000);
    sampleNow();
}

NetworkDiagnosticsSample NetworkDiagnosticsHistory::latestSample() const
{
    return m_samples.isEmpty() ? NetworkDiagnosticsSample{} : m_samples.last();
}

void NetworkDiagnosticsHistory::sampleNow()
{
    NetworkDiagnosticsSample sample;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const double elapsedSeconds = std::max(0.001, (nowMs - m_lastSampleMs) / 1000.0);
    sample.timestampMs = nowMs;
    sample.rttMs = m_model->lastPingRtt();

    static constexpr PanadapterStream::StreamCategory cats[] = {
        PanadapterStream::CatAudio, PanadapterStream::CatFFT,
        PanadapterStream::CatWaterfall, PanadapterStream::CatMeter
    };

    for (PanadapterStream::StreamCategory cat : cats) {
        PanadapterStream::CategoryStats cs = m_model->categoryStats(cat);
        const qint64 delta = std::max<qint64>(0, cs.bytes - m_lastCatBytes[cat]);
        m_lastCatBytes[cat] = cs.bytes;
        const double rateKbps = kbpsFromBytes(delta, elapsedSeconds);
        if (cat == PanadapterStream::CatAudio) {
            sample.audioKbps = rateKbps;
            sample.audioLossPct = lossPercent(cs);
        } else if (cat == PanadapterStream::CatFFT) {
            sample.fftKbps = rateKbps;
            sample.fftLossPct = lossPercent(cs);
        } else if (cat == PanadapterStream::CatWaterfall) {
            sample.waterfallKbps = rateKbps;
            sample.waterfallLossPct = lossPercent(cs);
        } else if (cat == PanadapterStream::CatMeter) {
            sample.meterKbps = rateKbps;
            sample.meterLossPct = lossPercent(cs);
        }
    }

    {
        PanadapterStream::CategoryStats cs = m_model->categoryStats(PanadapterStream::CatDAX);
        const qint64 delta = std::max<qint64>(0, cs.bytes - m_lastCatBytes[PanadapterStream::CatDAX]);
        m_lastCatBytes[PanadapterStream::CatDAX] = cs.bytes;
        sample.daxKbps = kbpsFromBytes(delta, elapsedSeconds);
        sample.daxLossPct = lossPercent(cs);
    }

    const qint64 curRx = m_model->rxBytes();
    sample.rxKbps = kbpsFromBytes(std::max<qint64>(0, curRx - m_lastRxBytes), elapsedSeconds);
    m_lastRxBytes = curRx;
    const qint64 curTx = m_model->txBytes();
    sample.txKbps = kbpsFromBytes(std::max<qint64>(0, curTx - m_lastTxBytes), elapsedSeconds);
    m_lastTxBytes = curTx;

    sample.packetLossPct = m_model->packetLossPercent();

    if (m_audio) {
        const int sampleRate = m_audio->rxBufferSampleRate();
        const quint64 underruns = m_audio->rxBufferUnderrunCount();
        quint64 underrunDelta = 0;
        if (underruns >= m_lastAudioUnderrunCount) {
            underrunDelta = underruns - m_lastAudioUnderrunCount;
        }
        m_lastAudioUnderrunCount = underruns;
        sample.audioGapMs = m_model->audioPacketGapMs();
        sample.audioJitterMs = m_model->audioPacketJitterMs();
        sample.audioBufferMs = audioBufferMs(m_audio->rxBufferBytes(), sampleRate);
        sample.underrunsPerSecond = static_cast<double>(underrunDelta) / elapsedSeconds;
    }

    const QVector<PanadapterStream::AudioStreamDiagnostics> audioStreams =
        m_model->audioStreamDiagnostics();
    sample.audioStreamCount = audioStreams.size();
    qint64 totalLatePackets = 0;
    for (const PanadapterStream::AudioStreamDiagnostics& stream : audioStreams) {
        totalLatePackets += stream.latePackets;
    }
    if (totalLatePackets >= m_lastAudioLatePackets) {
        sample.audioLatePacketsPerSecond =
            (totalLatePackets - m_lastAudioLatePackets) / elapsedSeconds;
    }
    m_lastAudioLatePackets = totalLatePackets;

    if (!audioStreams.isEmpty()) {
        double feedRateSum = 0.0;
        int feedRateCount = 0;
        qint64 primaryPackets = -1;
        sample.audioLastPacketAgeMs = std::numeric_limits<qint64>::max();
        for (const PanadapterStream::AudioStreamDiagnostics& stream : audioStreams) {
            if (stream.feedRateHz > 0.0) {
                feedRateSum += stream.feedRateHz;
                ++feedRateCount;
            }
            sample.audioFeedDeficitMs = std::max(sample.audioFeedDeficitMs,
                                                 std::max(0.0, stream.deficitMs));
            sample.audioLatePackets += stream.latePackets;
            sample.audioPacketGaps += stream.sequenceErrors;
            sample.audioLastPacketAgeMs = std::min(sample.audioLastPacketAgeMs,
                                                   stream.lastPacketAgeMs);
            if (stream.packets > primaryPackets) {
                primaryPackets = stream.packets;
                sample.audioPacketClassCode = stream.packetClassCode;
            }
        }
        if (feedRateCount > 0) {
            sample.audioFeedRateHz = feedRateSum / feedRateCount;
        }
        if (sample.audioLastPacketAgeMs == std::numeric_limits<qint64>::max()) {
            sample.audioLastPacketAgeMs = 0;
        }
    }
    m_lastSampleMs = nowMs;

    m_samples.push_back(sample);
    pruneSamples(nowMs);
}

void NetworkDiagnosticsHistory::pruneSamples(qint64 nowMs)
{
    static constexpr qint64 kMaxHistoryMs = 7LL * 24 * 60 * 60 * 1000;
    static constexpr qint64 kRawHistoryMs = 60LL * 60 * 1000;
    const qint64 cutoff = nowMs - kMaxHistoryMs;
    const qint64 rawCutoff = nowMs - kRawHistoryMs;
    QVector<NetworkDiagnosticsSample> compacted;
    compacted.reserve(m_samples.size());

    qint64 lastMinuteBucket = -1;
    int bucketSampleCount = 0;
    auto mergeAverage = [](double current, double incoming, int currentCount) {
        return ((current * currentCount) + incoming) / (currentCount + 1);
    };
    for (const NetworkDiagnosticsSample& sample : m_samples) {
        if (sample.timestampMs < cutoff) {
            continue;
        }
        if (sample.timestampMs >= rawCutoff) {
            compacted.push_back(sample);
            lastMinuteBucket = -1;
            bucketSampleCount = 0;
            continue;
        }

        const qint64 minuteBucket = sample.timestampMs / 60000;
        if (minuteBucket != lastMinuteBucket) {
            compacted.push_back(sample);
            lastMinuteBucket = minuteBucket;
            bucketSampleCount = 1;
        } else if (!compacted.isEmpty()) {
            NetworkDiagnosticsSample& bucket = compacted.last();
            bucket.rxKbps = mergeAverage(bucket.rxKbps, sample.rxKbps, bucketSampleCount);
            bucket.txKbps = mergeAverage(bucket.txKbps, sample.txKbps, bucketSampleCount);
            bucket.audioKbps = mergeAverage(bucket.audioKbps, sample.audioKbps, bucketSampleCount);
            bucket.fftKbps = mergeAverage(bucket.fftKbps, sample.fftKbps, bucketSampleCount);
            bucket.waterfallKbps = mergeAverage(bucket.waterfallKbps, sample.waterfallKbps, bucketSampleCount);
            bucket.meterKbps = mergeAverage(bucket.meterKbps, sample.meterKbps, bucketSampleCount);
            bucket.daxKbps = mergeAverage(bucket.daxKbps, sample.daxKbps, bucketSampleCount);
            bucket.rttMs = std::max(bucket.rttMs, sample.rttMs);
            bucket.audioGapMs = std::max(bucket.audioGapMs, sample.audioGapMs);
            bucket.audioJitterMs = std::max(bucket.audioJitterMs, sample.audioJitterMs);
            bucket.packetLossPct = std::max(bucket.packetLossPct, sample.packetLossPct);
            bucket.audioLossPct = std::max(bucket.audioLossPct, sample.audioLossPct);
            bucket.fftLossPct = std::max(bucket.fftLossPct, sample.fftLossPct);
            bucket.waterfallLossPct = std::max(bucket.waterfallLossPct, sample.waterfallLossPct);
            bucket.meterLossPct = std::max(bucket.meterLossPct, sample.meterLossPct);
            bucket.daxLossPct = std::max(bucket.daxLossPct, sample.daxLossPct);
            bucket.audioBufferMs = sample.audioBufferMs;
            bucket.underrunsPerSecond = std::max(bucket.underrunsPerSecond, sample.underrunsPerSecond);
            bucket.audioFeedRateHz =
                mergeAverage(bucket.audioFeedRateHz, sample.audioFeedRateHz, bucketSampleCount);
            bucket.audioFeedDeficitMs = std::max(bucket.audioFeedDeficitMs, sample.audioFeedDeficitMs);
            bucket.audioLatePacketsPerSecond =
                std::max(bucket.audioLatePacketsPerSecond, sample.audioLatePacketsPerSecond);
            bucket.audioLatePackets = sample.audioLatePackets;
            bucket.audioPacketGaps = sample.audioPacketGaps;
            bucket.audioLastPacketAgeMs = sample.audioLastPacketAgeMs;
            bucket.audioPacketClassCode = sample.audioPacketClassCode;
            bucket.audioStreamCount = sample.audioStreamCount;
            ++bucketSampleCount;
        }
    }
    m_samples = std::move(compacted);
}

static void updateAudioStreamTable(QTableWidget* table,
                                   const RadioModel* model,
                                   const QVector<PanadapterStream::AudioStreamDiagnostics>& audioStreams)
{
    if (!table) {
        return;
    }

    auto makeItem = [](const QString& text, const QColor& color = QColor("#c2ccdb")) {
        auto* item = new QTableWidgetItem(text);
        item->setForeground(color);
        return item;
    };
    auto makeNumberItem = [&](const QString& text, bool warning = false) {
        auto* item = makeItem(text, warning ? QColor("#e8b977") : QColor("#c2ccdb"));
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return item;
    };

    const QStringList sliceLabels = audibleSliceLabels(model);
    table->setRowCount(audioStreams.isEmpty()
                       ? 1
                       : static_cast<int>(audioStreams.size()));
    table->clearContents();

    if (audioStreams.isEmpty()) {
        table->setItem(0, 0, makeItem("Waiting"));
        table->setItem(0, 1, makeItem(formatAudioStreamSource(sliceLabels), QColor("#8d99ad")));
        for (int col = 2; col < table->columnCount(); ++col) {
            table->setItem(0, col, makeItem("--", QColor("#6e7a8d")));
        }
        table->resizeRowsToContents();
        return;
    }

    for (int row = 0; row < static_cast<int>(audioStreams.size()); ++row) {
        const PanadapterStream::AudioStreamDiagnostics& stream = audioStreams.at(row);
        const bool late = stream.latePackets > 0;
        const bool gaps = stream.sequenceErrors > 0;
        table->setItem(row, 0, makeItem(formatStreamId(stream.streamId)));
        table->setItem(row, 1, makeItem(formatAudioStreamSource(sliceLabels)));
        table->setItem(row, 2, makeItem(formatAudioStreamFormat(stream.packetClassCode)));
        table->setItem(row, 3, makeNumberItem(formatFeedRate(stream.feedRateHz)));
        table->setItem(row, 4, makeNumberItem(formatFeedDeficit(std::max(0.0, stream.deficitMs)),
                                              stream.deficitMs >= 20.0));
        table->setItem(row, 5, makeNumberItem(QString::number(stream.latePackets), late));
        table->setItem(row, 6, makeNumberItem(QString::number(stream.sequenceErrors), gaps));
        table->setItem(row, 7, makeNumberItem(QString("%1 ms").arg(stream.maxGapMs),
                                              stream.maxGapMs > stream.expectedPacketMs * 2.0));
        table->setItem(row, 8, makeNumberItem(QString("%1 ms ago").arg(stream.lastPacketAgeMs),
                                              stream.lastPacketAgeMs > 1000));
    }

    table->resizeRowsToContents();
}

void NetworkDiagnosticsDialog::refresh()
{
    const NetworkDiagnosticsSample sample = m_history ? m_history->latestSample() : NetworkDiagnosticsSample{};

    // Status and RTT
    m_statusLabel->setText(m_model->networkQuality());
    m_targetIpLabel->setText(m_model->targetRadioIp().isEmpty()
                                 ? "Not connected"
                                 : m_model->targetRadioIp());
    m_sourcePathLabel->setText(m_model->selectedSourcePath());
    m_tcpEndpointLabel->setText(m_model->localTcpEndpoint());
    m_udpEndpointLabel->setText(m_model->localUdpEndpoint());
    m_udpSeenLabel->setText(m_model->firstUdpPacketSeen() ? "Yes" : "No");

    const int rtt = m_model->lastPingRtt();
    m_rttLabel->setText(rtt < 1 ? "< 1 ms" : QString("%1 ms").arg(rtt));
    m_overviewStatusValue->setText(m_model->networkQuality());
    m_overviewLatencyValue->setText(rtt < 1 ? "< 1 ms" : QString("%1 ms").arg(rtt));

    const int maxRtt = m_model->maxPingRtt();
    m_maxRttLabel->setText(maxRtt < 1 ? "< 1 ms" : QString("%1 ms").arg(maxRtt));

    // Per-category rates
    static constexpr PanadapterStream::StreamCategory cats[] = {
        PanadapterStream::CatAudio, PanadapterStream::CatFFT,
        PanadapterStream::CatWaterfall, PanadapterStream::CatMeter
    };
    QLabel* rateLabels[] = { m_audioRateLabel, m_fftRateLabel, m_wfRateLabel, m_meterRateLabel };
    QLabel* dropLabels[] = { m_audioDropLabel, m_fftDropLabel, m_wfDropLabel, m_meterDropLabel };

    for (int i = 0; i < 4; ++i) {
        PanadapterStream::CategoryStats cs = m_model->categoryStats(cats[i]);
        double rateKbps = 0.0;
        if (cats[i] == PanadapterStream::CatAudio) {
            rateKbps = sample.audioKbps;
        } else if (cats[i] == PanadapterStream::CatFFT) {
            rateKbps = sample.fftKbps;
        } else if (cats[i] == PanadapterStream::CatWaterfall) {
            rateKbps = sample.waterfallKbps;
        } else if (cats[i] == PanadapterStream::CatMeter) {
            rateKbps = sample.meterKbps;
        }
        rateLabels[i]->setText(QString("%1 kbps").arg(static_cast<int>(rateKbps)));
        dropLabels[i]->setText(formatDrop(cs));
    }

    // DAX traffic
    {
        PanadapterStream::CategoryStats cs = m_model->categoryStats(PanadapterStream::CatDAX);
        m_daxRateLabel->setText(QString("%1 kbps").arg(static_cast<int>(sample.daxKbps)));
        m_daxDropLabel->setText(formatDrop(cs));
    }

    // Total RX: all UDP bytes received on our VITA-49 socket
    m_rxRateLabel->setText(QString("%1 kbps").arg(static_cast<int>(sample.rxKbps)));
    m_txRateLabel->setText(QString("%1 kbps").arg(static_cast<int>(sample.txKbps)));

    // Total dropped (across all owned streams)
    const int dropped = m_model->packetDropCount();
    const int total = m_model->packetTotalCount();
    if (total > 0) {
        const double pct = (dropped * 100.0) / total;
        const int windowPackets = m_model->packetLossWindowPackets();
        const int windowDrops = m_model->packetLossWindowDrops();
        const double windowPct = m_model->packetLossPercent();
        m_overviewLossValue->setText(QString("%1%").arg(windowPct, 0, 'f', 2));
        m_droppedLabel->setText(
            QString("Last %1s: %2 / %3 dropped (%4%)   Total: %5 / %6 dropped (%7%)")
                .arg(m_model->packetLossWindowSeconds())
                .arg(windowDrops)
                .arg(windowPackets)
                .arg(windowPct, 0, 'f', 2)
                .arg(dropped).arg(total).arg(pct, 0, 'f', 2));
    } else {
        m_droppedLabel->setText("No packets received yet");
        m_overviewLossValue->setText("0.00%");
    }

    if (m_audio) {
        const int sampleRate = m_audio->rxBufferSampleRate();
        const quint64 underruns = m_audio->rxBufferUnderrunCount();
        const QVector<PanadapterStream::AudioStreamDiagnostics> audioStreams =
            m_model->audioStreamDiagnostics();
        const QStringList sliceLabels = audibleSliceLabels(m_model);
        m_audioBufferLabel->setText(formatAudioBuffer(m_audio->rxBufferBytes(), sampleRate));
        m_overviewAudioValue->setText(QString("%1 ms").arg(audioBufferMs(m_audio->rxBufferBytes(), sampleRate), 0, 'f', 1));
        m_audioBufferPeakLabel->setText(formatAudioBuffer(m_audio->rxBufferPeakBytes(), sampleRate));
        m_audioUnderrunLabel->setText(QString::number(underruns));
        m_audioUnderrunRateLabel->setText(QString::number(sample.underrunsPerSecond, 'f', 0));

        m_audioPacketGapLabel->setText(formatMsValue(m_model->audioPacketGapMs()));
        m_audioPacketGapMaxLabel->setText(formatMsValue(m_model->audioPacketGapMaxMs()));
        m_audioJitterLabel->setText(formatMsValue(m_model->audioPacketJitterMs()));
        m_audioStreamLabel->setText(formatAudioStream(sample, sliceLabels));
        m_audioFeedRateLabel->setText(formatFeedRate(sample.audioFeedRateHz, sample.audioStreamCount));
        m_audioFeedDeficitLabel->setText(formatFeedDeficit(sample.audioFeedDeficitMs));
        m_audioLateGapLabel->setText(formatLateArrivals(sample.audioLatePackets,
                                                        sample.audioLatePacketsPerSecond));
        const AudioHealthStatus audioHealth = formatAudioHealth(sample);
        m_audioStreamHealthLabel->setText(audioHealth.text);
        m_audioStreamHealthLabel->setStyleSheet(audioHealthStyle(audioHealth.state));
        m_audioStreamsDetailLabel->setText(formatAudioSupportDetails(sample.audioStreamCount,
                                                                     sliceLabels));
        updateAudioStreamTable(m_audioStreamsTable, m_model, audioStreams);
    } else {
        m_audioBufferLabel->setText("Unavailable");
        m_audioBufferPeakLabel->setText("Unavailable");
        m_audioUnderrunLabel->setText("Unavailable");
        m_audioUnderrunRateLabel->setText("Unavailable");
        m_audioPacketGapLabel->setText("Unavailable");
        m_audioPacketGapMaxLabel->setText("Unavailable");
        m_audioJitterLabel->setText("Unavailable");
        m_audioStreamLabel->setText("Unavailable");
        m_audioFeedRateLabel->setText("Unavailable");
        m_audioFeedDeficitLabel->setText("Unavailable");
        m_audioLateGapLabel->setText("Unavailable");
        m_audioStreamHealthLabel->setText("Unavailable");
        m_audioStreamHealthLabel->setStyleSheet("QLabel { color: #8d99ad; font-weight: 700; }");
        m_audioStreamsDetailLabel->setText("Unavailable");
        updateAudioStreamTable(m_audioStreamsTable, m_model, {});
        m_overviewAudioValue->setText("Unavailable");
    }

    // Color the status label
    const QString q = m_model->networkQuality();
    if (q == "Excellent" || q == "Very Good") {
        m_statusLabel->setStyleSheet("QLabel { color: #64d36e; font-weight: 700; }");
        m_overviewStatusValue->setStyleSheet("QLabel { color: #64d36e; font-weight: 700; font-size: 18px; }");
    } else if (q == "Good") {
        m_statusLabel->setStyleSheet("QLabel { color: #80ed91; font-weight: 700; }");
        m_overviewStatusValue->setStyleSheet("QLabel { color: #80ed91; font-weight: 700; font-size: 18px; }");
    } else if (q == "Fair") {
        m_statusLabel->setStyleSheet("QLabel { color: #e8b977; font-weight: 700; }");
        m_overviewStatusValue->setStyleSheet("QLabel { color: #e8b977; font-weight: 700; font-size: 18px; }");
    } else if (q == "Poor") {
        m_statusLabel->setStyleSheet("QLabel { color: #ff6b6b; font-weight: 700; }");
        m_overviewStatusValue->setStyleSheet("QLabel { color: #ff6b6b; font-weight: 700; font-size: 18px; }");
    } else {
        m_statusLabel->setStyleSheet("QLabel { color: #d4deea; font-weight: 700; }");
        m_overviewStatusValue->setStyleSheet("QLabel { color: #d4deea; font-weight: 700; font-size: 18px; }");
    }

    updateCharts();
}

int NetworkDiagnosticsDialog::selectedRangeSeconds() const
{
    if (!m_rangeCombo) {
        return 5 * 60;
    }
    return m_rangeCombo->currentData().toInt();
}

void NetworkDiagnosticsDialog::updateCharts()
{
    const int rangeSeconds = selectedRangeSeconds();
    const qint64 bucketMs = rangeSeconds <= 5 * 60
        ? 1000
        : std::max<qint64>(5000, (static_cast<qint64>(rangeSeconds) * 1000) / 300);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 endMs = rangeSeconds <= 5 * 60 ? nowMs : (nowMs / bucketMs) * bucketMs;
    const qint64 cutoffMs = endMs - static_cast<qint64>(rangeSeconds) * 1000;
    static const QVector<NetworkDiagnosticsSample> kEmptySamples;
    const QVector<NetworkDiagnosticsSample>& samples = m_history ? m_history->samples() : kEmptySamples;

    auto buildSeries = [&](const QString& label, const QColor& color, auto valueFor) {
        TimeSeriesGraphWidget::Series series{label, color, {}, {}};
        series.points.reserve(samples.size());
        if (bucketMs <= 1000) {
            for (const NetworkDiagnosticsSample& sample : samples) {
                if (sample.timestampMs < cutoffMs || sample.timestampMs > endMs) {
                    continue;
                }
                const double secondsFromStart = (sample.timestampMs - cutoffMs) / 1000.0;
                series.points.push_back(QPointF(secondsFromStart,
                                               std::max(0.0, static_cast<double>(valueFor(sample)))));
            }
        } else {
            qint64 currentBucket = -1;
            double bucketSum = 0.0;
            int bucketCount = 0;
            auto flushBucket = [&] {
                if (currentBucket < 0 || bucketCount <= 0) {
                    return;
                }
                const qint64 bucketCenterMs = currentBucket * bucketMs + bucketMs / 2;
                const double secondsFromStart = (bucketCenterMs - cutoffMs) / 1000.0;
                series.points.push_back(QPointF(secondsFromStart, bucketSum / bucketCount));
                bucketSum = 0.0;
                bucketCount = 0;
            };
            for (const NetworkDiagnosticsSample& sample : samples) {
                if (sample.timestampMs < cutoffMs || sample.timestampMs > endMs) {
                    continue;
                }
                const qint64 sampleBucket = sample.timestampMs / bucketMs;
                if (currentBucket != sampleBucket) {
                    flushBucket();
                    currentBucket = sampleBucket;
                }
                bucketSum += std::max(0.0, static_cast<double>(valueFor(sample)));
                ++bucketCount;
            }
            flushBucket();
        }
        return series;
    };
    auto buildSeriesWithUnit = [&](const QString& label,
                                   const QColor& color,
                                   const QString& unitSuffix,
                                   auto valueFor) {
        TimeSeriesGraphWidget::Series series = buildSeries(label, color, valueFor);
        series.unitSuffix = unitSuffix;
        return series;
    };

    QVector<TimeSeriesGraphWidget::Series> latencySeries{
        buildSeriesWithUnit("RTT", QColor("#00b4d8"), " ms", [](const NetworkDiagnosticsSample& s) { return static_cast<double>(s.rttMs); }),
        buildSeriesWithUnit("Arrival gap", QColor("#f2c94c"), " ms", [](const NetworkDiagnosticsSample& s) { return static_cast<double>(s.audioGapMs); }),
        buildSeriesWithUnit("Jitter", QColor("#eb5757"), " ms", [](const NetworkDiagnosticsSample& s) { return static_cast<double>(s.audioJitterMs); })
    };
    QVector<TimeSeriesGraphWidget::Series> rateSeries{
        buildSeriesWithUnit("RX total", QColor("#00b4d8"), " kbps", [](const NetworkDiagnosticsSample& s) { return s.rxKbps; }),
        buildSeriesWithUnit("Audio", QColor("#6fcf97"), " kbps", [](const NetworkDiagnosticsSample& s) { return s.audioKbps; }),
        buildSeriesWithUnit("FFT", QColor("#bb6bd9"), " kbps", [](const NetworkDiagnosticsSample& s) { return s.fftKbps; }),
        buildSeriesWithUnit("Waterfall", QColor("#f2994a"), " kbps", [](const NetworkDiagnosticsSample& s) { return s.waterfallKbps; }),
        buildSeriesWithUnit("Meters", QColor("#56ccf2"), " kbps", [](const NetworkDiagnosticsSample& s) { return s.meterKbps; }),
        buildSeriesWithUnit("DAX", QColor("#bdbdbd"), " kbps", [](const NetworkDiagnosticsSample& s) { return s.daxKbps; })
    };
    QVector<TimeSeriesGraphWidget::Series> lossSeries{
        buildSeriesWithUnit("Recent total", QColor("#eb5757"), "%", [](const NetworkDiagnosticsSample& s) { return s.packetLossPct; }),
        buildSeriesWithUnit("Audio", QColor("#6fcf97"), "%", [](const NetworkDiagnosticsSample& s) { return s.audioLossPct; }),
        buildSeriesWithUnit("FFT", QColor("#bb6bd9"), "%", [](const NetworkDiagnosticsSample& s) { return s.fftLossPct; }),
        buildSeriesWithUnit("Waterfall", QColor("#f2994a"), "%", [](const NetworkDiagnosticsSample& s) { return s.waterfallLossPct; }),
        buildSeriesWithUnit("Meters", QColor("#56ccf2"), "%", [](const NetworkDiagnosticsSample& s) { return s.meterLossPct; }),
        buildSeriesWithUnit("DAX", QColor("#bdbdbd"), "%", [](const NetworkDiagnosticsSample& s) { return s.daxLossPct; })
    };
    QVector<TimeSeriesGraphWidget::Series> audioBufferSeries{
        buildSeriesWithUnit("Buffer", QColor("#00b4d8"), " ms", [](const NetworkDiagnosticsSample& s) { return s.audioBufferMs; }),
        buildSeriesWithUnit("Slow delivery", QColor("#f2c94c"), " ms", [](const NetworkDiagnosticsSample& s) { return s.audioFeedDeficitMs; }),
        buildSeriesWithUnit("Playback underruns", QColor("#eb5757"), "/s", [](const NetworkDiagnosticsSample& s) { return s.underrunsPerSecond; }),
        buildSeriesWithUnit("Late arrivals", QColor("#bb6bd9"), "/s", [](const NetworkDiagnosticsSample& s) { return s.audioLatePacketsPerSecond; })
    };
    QVector<TimeSeriesGraphWidget::Series> audioFeedSeries{
        buildSeriesWithUnit("Avg stream rate", QColor("#6fcf97"), " Hz", [](const NetworkDiagnosticsSample& s) { return s.audioFeedRateHz; }),
        buildSeriesWithUnit("Target", QColor("#8aa8c0"), " Hz", [](const NetworkDiagnosticsSample&) {
            return static_cast<double>(AudioEngine::DEFAULT_SAMPLE_RATE);
        })
    };

    m_overviewLatencyGraph->setSeries(latencySeries, rangeSeconds);
    m_overviewLossGraph->setSeries({lossSeries.first()}, rangeSeconds);
    m_overviewRatesGraph->setSeries({
        buildSeriesWithUnit("RX total", QColor("#00b4d8"), " kbps", [](const NetworkDiagnosticsSample& s) { return s.rxKbps; }),
        buildSeriesWithUnit("TX total", QColor("#f2c94c"), " kbps", [](const NetworkDiagnosticsSample& s) { return s.txKbps; })
    }, rangeSeconds);
    m_overviewAudioGraph->setSeries(audioBufferSeries, rangeSeconds);
    m_latencyGraph->setSeries(latencySeries, rangeSeconds);
    m_ratesGraph->setSeries(rateSeries, rangeSeconds);
    m_lossGraph->setSeries(lossSeries, rangeSeconds);
    m_audioGraph->setSeries(audioBufferSeries, rangeSeconds);
    m_audioFeedGraph->setSeries(audioFeedSeries, rangeSeconds);
}

} // namespace AetherSDR
