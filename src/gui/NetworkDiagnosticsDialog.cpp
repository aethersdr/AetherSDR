#include "NetworkDiagnosticsDialog.h"
#include "FramelessMoveHelper.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/LogManager.h"
#include "models/RadioModel.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QComboBox>
#include <QDateTime>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QMouseEvent>
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
#include <QSyntaxHighlighter>
#include <QTabWidget>
#include <QTextCharFormat>
#include <QVBoxLayout>
#include <QWindow>

namespace {
constexpr int kResizeMargin = 6;
}

namespace AetherSDR {

class LogSyntaxHighlighter : public QSyntaxHighlighter {
public:
    explicit LogSyntaxHighlighter(QTextDocument* parent)
        : QSyntaxHighlighter(parent)
    {
        m_timeFormat.setForeground(QColor("#7f98b0"));
        m_debugFormat.setForeground(QColor("#7f98b0"));
        m_infoFormat.setForeground(QColor("#77d8ff"));
        m_warningFormat.setForeground(QColor("#ffd166"));
        m_criticalFormat.setForeground(QColor("#ff6b6b"));
        m_categoryFormat.setForeground(QColor("#c8d8e8"));
        m_categoryFormat.setFontWeight(QFont::Bold);
        m_numberFormat.setForeground(QColor("#8fe388"));
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
        painter.fillRect(rect(), QColor("#0a0a14"));

        const QRectF plot = rect().adjusted(84, 30, -14, -42);
        painter.setPen(QPen(QColor("#203040"), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 4, 4);

        painter.setPen(QColor("#c8d8e8"));
        const QFont normalFont = painter.font();
        QFont titleFont = painter.font();
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.drawText(QRectF(10, 6, width() - 190, 18), Qt::AlignLeft | Qt::AlignVCenter, m_title);
        painter.setFont(normalFont);
        painter.setPen(QColor("#8aa8c0"));
        painter.drawText(QRectF(width() - 180, 6, 166, 18),
                         Qt::AlignRight | Qt::AlignVCenter, rangeLabel());

        const QVector<Series> visibleSeries = activeSeries();
        const bool hasPoints = std::any_of(visibleSeries.cbegin(), visibleSeries.cend(), [](const Series& series) {
            return !series.points.isEmpty();
        });
        if (!hasPoints || plot.width() < 20 || plot.height() < 20) {
            painter.setPen(QColor("#8aa8c0"));
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
        painter.setPen(QPen(QColor("#203040"), 1));
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
            painter.setPen(QColor("#8aa8c0"));
            painter.drawText(QRectF(4, y - 8, 74, 16), Qt::AlignRight | Qt::AlignVCenter,
                             label);
            painter.setPen(QPen(QColor("#203040"), 1));
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
            const QColor bgSolid("#0a0a14");
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
            const QColor textColor = selected ? QColor("#c8d8e8") : QColor("#5f7488");
            const QColor lineColor = selected ? series.color : QColor("#405468");
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
                                                   QWidget* parent)
    : QDialog(parent), m_model(model), m_audio(audio), m_history(history)
{
    setWindowTitle("Network Diagnostics");
    setMinimumSize(920, 680);
    resize(980, 760);
    // Track mouse without buttons pressed so the resize cursor updates
    // while hovering the bare margin around the dialog body.
    setMouseTracking(true);
    setStyleSheet(
        "QDialog { background: #050710; }"
        "QTabWidget::pane { border: 1px solid #203040; border-radius: 4px; top: -1px; }"
        "QTabBar::tab { background: #0a0a14; border: 1px solid #203040; "
        "border-bottom: none; color: #8aa8c0; padding: 7px 12px; }"
        "QTabBar::tab:selected { color: #c8d8e8; background: #111120; }"
        "QTabBar::tab:hover { color: #c8d8e8; }"
        "QGroupBox { border: 1px solid #203040; border-radius: 4px; "
        "color: #c8d8e8; font-weight: bold; margin-top: 12px; padding-top: 8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
        "QLabel { color: #8aa8c0; }"
        "QScrollArea { background: transparent; border: none; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }");

    // Outer layout: zero-margin so the title bar runs edge-to-edge.  The
    // 8 px resize hit zone lives on the bare gap around the inner content
    // widget (which carries its own padding).
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Custom title bar ─────────────────────────────────────────────
    // Same chrome family as AetherialAudioStrip / ContainerTitleBar:
    // 18 px tall, blue-gradient background, 10 px bold title, trio of
    // window-control buttons at the right.  Built inline so the
    // gradient + grip glyphs match exactly.
    {
        m_titleBar = new QWidget(this);
        m_titleBar->setFixedHeight(18);
        m_titleBar->setAttribute(Qt::WA_StyledBackground, true);
        m_titleBar->setStyleSheet(
            "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
            "stop:0 #5a7494, stop:0.5 #384e68, stop:1 #1e2e3e); "
            "border-bottom: 1px solid #0a1a28; }");
        m_titleBar->installEventFilter(this);

        auto* tbRow = new QHBoxLayout(m_titleBar);
        tbRow->setContentsMargins(6, 0, 2, 0);
        tbRow->setSpacing(4);

        auto* grip = new QLabel(QString::fromUtf8("\xe2\x8b\xae\xe2\x8b\xae"),
                                m_titleBar);
        grip->setStyleSheet(
            "QLabel { background: transparent; color: #a0b4c8;"
            " font-size: 10px; }");
        tbRow->addWidget(grip);

        auto* tbTitle = new QLabel("Network Diagnostics", m_titleBar);
        tbTitle->setStyleSheet(
            "QLabel { background: transparent; color: #e0ecf4;"
            " font-size: 10px; font-weight: bold; }");
        tbRow->addWidget(tbTitle);
        tbRow->addStretch();

        const QString btnStyle =
            "QPushButton { background: transparent; border: none;"
            " color: #c8d8e8; font-size: 11px; font-weight: bold;"
            " padding: 0px 4px; }"
            "QPushButton:hover { color: #ffffff; }";
        const QString closeBtnStyle =
            "QPushButton { background: transparent; border: none;"
            " color: #c8d8e8; font-size: 11px; font-weight: bold;"
            " padding: 0px 4px; }"
            "QPushButton:hover { color: #ffffff; background: #cc2030; }";

        auto* minBtn = new QPushButton(QString::fromUtf8("\xe2\x80\x94"), m_titleBar);
        minBtn->setFixedSize(16, 16);
        minBtn->setCursor(Qt::ArrowCursor);
        minBtn->setStyleSheet(btnStyle);
        minBtn->setToolTip("Minimize");
        connect(minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
        tbRow->addWidget(minBtn);

        auto* maxBtn = new QPushButton(QString::fromUtf8("\xe2\x96\xa1"), m_titleBar);
        maxBtn->setFixedSize(16, 16);
        maxBtn->setCursor(Qt::ArrowCursor);
        maxBtn->setStyleSheet(btnStyle);
        maxBtn->setToolTip("Maximize");
        connect(maxBtn, &QPushButton::clicked, this, [this]() {
            if (isMaximized()) showNormal(); else showMaximized();
        });
        tbRow->addWidget(maxBtn);

        auto* closeBtn = new QPushButton(QString::fromUtf8("\xc3\x97"), m_titleBar);
        closeBtn->setFixedSize(16, 16);
        closeBtn->setCursor(Qt::ArrowCursor);
        closeBtn->setStyleSheet(closeBtnStyle);
        closeBtn->setToolTip("Close");
        connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
        tbRow->addWidget(closeBtn);

        root->addWidget(m_titleBar);
    }

    // Content area — gets its own layout with 10 px padding so the
    // resize hit zone (bare ~8 px margin around content) is reachable
    // on every edge.
    auto* outerContent = new QWidget(this);
    auto* body = new QVBoxLayout(outerContent);
    body->setContentsMargins(10, 8, 10, 10);
    body->setSpacing(8);
    m_bodyLayout = body;
    root->addWidget(outerContent, 1);

    // Timeframe selector lives in the top-right corner of the QTabWidget's
    // tab bar so the tabs and the dropdown share a single row, eliminating
    // the otherwise-empty band above the tabs.
    auto* rangeLabel = new QLabel("Timeframe");
    rangeLabel->setStyleSheet("QLabel { color: #8aa8c0; }");
    m_rangeCombo = new QComboBox(this);
    m_rangeCombo->setFixedWidth(132);
    m_rangeCombo->setStyleSheet(
        "QComboBox { background: #0a0a14; border: 1px solid #203040; "
        "border-radius: 4px; color: #c8d8e8; padding: 3px 8px; }"
        "QComboBox:hover { border-color: #00b4d8; }"
        "QComboBox::drop-down { border: none; width: 18px; }"
        "QComboBox QAbstractItemView { background: #111120; color: #c8d8e8; "
        "selection-background-color: #00b4d8; selection-color: #000; }");
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
    contentLayout->setContentsMargins(8, 8, 8, 8);
    contentLayout->setColumnStretch(0, 1);
    contentLayout->setColumnStretch(1, 1);
    contentLayout->setColumnMinimumWidth(0, 430);
    contentLayout->setColumnMinimumWidth(1, 430);
    contentLayout->setHorizontalSpacing(16);
    contentLayout->setVerticalSpacing(14);
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
        l->setStyleSheet("QLabel { color: #c8d8e8; font-weight: bold; }");
        return l;
    };

    auto makeDim = [&](const QString& init = "") {
        return makeVal(init);
    };

    auto makeNote = [](const QString& text) {
        auto* l = new QLabel(text);
        l->setWordWrap(true);
        l->setStyleSheet("QLabel { color: #8aa8c0; font-size: 11px; line-height: 1.2; }");
        return l;
    };

    auto makeHealthCard = [](const QString& title, const QString& subtitle) {
        auto* card = new QGroupBox(title);
        card->setMinimumHeight(96);
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(10, 12, 10, 10);
        auto* value = new QLabel("--");
        value->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        value->setMinimumHeight(value->fontMetrics().height() + 4);
        value->setStyleSheet("QLabel { color: #c8d8e8; font-weight: bold; font-size: 18px; }");
        auto* hint = new QLabel(subtitle);
        hint->setWordWrap(true);
        hint->setStyleSheet("QLabel { color: #8aa8c0; font-size: 11px; }");
        layout->addWidget(value);
        layout->addWidget(hint);
        layout->addStretch();
        return std::pair<QGroupBox*, QLabel*>{card, value};
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
    auto* statusGroup = new QGroupBox("Network Status");
    statusGroup->setMinimumWidth(430);
    auto* statusGrid = new QGridLayout(statusGroup);
    statusGrid->setColumnStretch(1, 1);
    statusGrid->setVerticalSpacing(2);
    statusGrid->setHorizontalSpacing(12);

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
    auto* rateGroup = new QGroupBox("Incoming Stream Rates");
    rateGroup->setMinimumWidth(430);
    auto* rateGrid = new QGridLayout(rateGroup);
    rateGrid->setColumnStretch(1, 1);
    rateGrid->setVerticalSpacing(2);
    rateGrid->setHorizontalSpacing(12);

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
    auto* dropGroup = new QGroupBox("Packet Loss (Sequence Gaps)");
    dropGroup->setMinimumWidth(430);
    auto* dropGrid = new QGridLayout(dropGroup);
    dropGrid->setColumnStretch(1, 1);
    dropGrid->setVerticalSpacing(2);
    dropGrid->setHorizontalSpacing(12);

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
    m_droppedLabel->setStyleSheet("QLabel { color: #c8d8e8; font-weight: bold; }");
    dropGrid->addWidget(m_droppedLabel, row++, 0, 1, 2);

    // ── Audio Playback group ──────────────────────────────────────────────
    auto* audioGroup = new QGroupBox("Audio Playback");
    audioGroup->setMinimumWidth(430);
    auto* audioGrid = new QGridLayout(audioGroup);
    audioGrid->setColumnStretch(1, 1);
    audioGrid->setVerticalSpacing(2);
    audioGrid->setHorizontalSpacing(12);

    row = 0;
    audioGrid->addWidget(makeNote(
        "Speaker-side buffer health. If underruns rise while the buffer stays near zero, "
        "playback is starving. Arrival gap and jitter measure timing, not packet loss."),
        row++, 0, 1, 2);
    audioGrid->addWidget(new QLabel("RX Buffer Now:"), row, 0);
    m_audioBufferLabel = makeVal();
    audioGrid->addWidget(m_audioBufferLabel, row++, 1);

    audioGrid->addWidget(new QLabel("RX Buffer Peak:"), row, 0);
    m_audioBufferPeakLabel = makeVal();
    audioGrid->addWidget(m_audioBufferPeakLabel, row++, 1);

    audioGrid->addWidget(new QLabel("Underruns (total):"), row, 0);
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
    m_overviewAudioGraph = new TimeSeriesGraphWidget("Audio Buffer", " ms");
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
    makeGraphTab("Audio", &m_audioGraph, "Playback Buffer", " ms");
    m_audioGraph->setPrimaryAxisSeries("Buffer");
    QWidget* logsTab = buildLogsTab();
    tabs->addTab(logsTab, "Logs");

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
    connect(tabs, &QTabWidget::currentChanged, this, [tabs, corner, logsTab](int index) {
        corner->setVisible(tabs->widget(index) != logsTab);
    });
    connect(&m_refreshTimer, &QTimer::timeout, this, &NetworkDiagnosticsDialog::refresh);
    m_refreshTimer.start(1000);
    connect(&m_logRefreshTimer, &QTimer::timeout, this, &NetworkDiagnosticsDialog::appendNewLogData);
    m_logRefreshTimer.start(500);
    initializeLogTail();
    refresh();

    setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
}

void NetworkDiagnosticsDialog::setFramelessMode(bool on)
{
    const QRect geom = geometry();
    const bool wasVisible = isVisible();

    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Dialog;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    setGeometry(geom);
    if (m_titleBar) {
        m_titleBar->setVisible(on);
    }
    if (m_bodyLayout) {
        m_bodyLayout->setContentsMargins(10, on ? 8 : 10, 10, 10);
    }
    if (wasVisible) {
        show();
    }
}

QWidget* NetworkDiagnosticsDialog::buildLogsTab()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto* filterGroup = new QGroupBox("Filter Categories", page);
    auto* filterGrid = new QGridLayout(filterGroup);
    filterGrid->setContentsMargins(10, 14, 10, 8);
    filterGrid->setHorizontalSpacing(12);
    filterGrid->setVerticalSpacing(4);

    auto* allCategories = new QCheckBox("General", filterGroup);
    allCategories->setProperty("logCategory", QStringLiteral("default"));
    allCategories->setChecked(true);
    allCategories->setToolTip("Uncategorized Qt log output");
    allCategories->setStyleSheet("QCheckBox { color: #c8d8e8; }");
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
        checkbox->setStyleSheet("QCheckBox { color: #c8d8e8; }");
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
    selectAllBtn->setFixedHeight(24);
    deselectAllBtn->setFixedHeight(24);
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
    m_logPathLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 11px; }");
    infoRow->addWidget(m_logPathLabel, 1);

    m_logLiveToggle = new QPushButton("Live", page);
    m_logLiveToggle->setCheckable(true);
    m_logLiveToggle->setChecked(true);
    m_logLiveToggle->setFixedWidth(92);
    m_logLiveToggle->setToolTip("Live follows the newest log output. Turn it off to inspect older lines.");
    m_logLiveToggle->setStyleSheet(
        "QPushButton { background: #1a2030; color: #c8d8e8; border: 1px solid #203040; "
        "border-radius: 4px; padding: 3px 8px; }"
        "QPushButton:checked { background: #00607a; color: #e0f0ff; border-color: #00b4d8; }");
    connect(m_logLiveToggle, &QPushButton::toggled, this, [this](bool live) {
        setLogFollowLive(live);
    });
    infoRow->addWidget(m_logLiveToggle);
    layout->addLayout(infoRow);

    m_logViewer = new QPlainTextEdit(page);
    m_logViewer->setReadOnly(true);
    m_logViewer->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logViewer->setMaximumBlockCount(2500);
    m_logViewer->setStyleSheet(
        "QPlainTextEdit { background: #0a0a14; color: #a0b0c0; "
        "font-family: monospace; font-size: 11px; border: 1px solid #203040; }");
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
            ++bucketSampleCount;
        }
    }
    m_samples = std::move(compacted);
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
        m_audioBufferLabel->setText(formatAudioBuffer(m_audio->rxBufferBytes(), sampleRate));
        m_overviewAudioValue->setText(QString("%1 ms").arg(audioBufferMs(m_audio->rxBufferBytes(), sampleRate), 0, 'f', 1));
        m_audioBufferPeakLabel->setText(formatAudioBuffer(m_audio->rxBufferPeakBytes(), sampleRate));
        m_audioUnderrunLabel->setText(QString::number(underruns));
        m_audioUnderrunRateLabel->setText(QString::number(sample.underrunsPerSecond, 'f', 0));

        m_audioPacketGapLabel->setText(formatMsValue(m_model->audioPacketGapMs()));
        m_audioPacketGapMaxLabel->setText(formatMsValue(m_model->audioPacketGapMaxMs()));
        m_audioJitterLabel->setText(formatMsValue(m_model->audioPacketJitterMs()));
    } else {
        m_audioBufferLabel->setText("Unavailable");
        m_audioBufferPeakLabel->setText("Unavailable");
        m_audioUnderrunLabel->setText("Unavailable");
        m_audioUnderrunRateLabel->setText("Unavailable");
        m_audioPacketGapLabel->setText("Unavailable");
        m_audioPacketGapMaxLabel->setText("Unavailable");
        m_audioJitterLabel->setText("Unavailable");
        m_overviewAudioValue->setText("Unavailable");
    }

    // Color the status label
    const QString q = m_model->networkQuality();
    if (q == "Excellent" || q == "Very Good") {
        m_statusLabel->setStyleSheet("QLabel { color: #00cc66; font-weight: bold; }");
        m_overviewStatusValue->setStyleSheet("QLabel { color: #00cc66; font-weight: bold; font-size: 18px; }");
    } else if (q == "Good") {
        m_statusLabel->setStyleSheet("QLabel { color: #88cc00; font-weight: bold; }");
        m_overviewStatusValue->setStyleSheet("QLabel { color: #88cc00; font-weight: bold; font-size: 18px; }");
    } else if (q == "Fair") {
        m_statusLabel->setStyleSheet("QLabel { color: #ccaa00; font-weight: bold; }");
        m_overviewStatusValue->setStyleSheet("QLabel { color: #ccaa00; font-weight: bold; font-size: 18px; }");
    } else if (q == "Poor") {
        m_statusLabel->setStyleSheet("QLabel { color: #cc3300; font-weight: bold; }");
        m_overviewStatusValue->setStyleSheet("QLabel { color: #cc3300; font-weight: bold; font-size: 18px; }");
    } else {
        m_statusLabel->setStyleSheet("QLabel { color: #c8d8e8; font-weight: bold; }");
        m_overviewStatusValue->setStyleSheet("QLabel { color: #c8d8e8; font-weight: bold; font-size: 18px; }");
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
        buildSeriesWithUnit("Underruns", QColor("#eb5757"), "/s", [](const NetworkDiagnosticsSample& s) { return s.underrunsPerSecond; })
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
}

// ──────────────────────────────────────────────────────────────────
// Frameless 8-axis resize + drag-to-move
// ──────────────────────────────────────────────────────────────────

Qt::Edges NetworkDiagnosticsDialog::edgesAt(const QPoint& pos) const
{
    if (!(windowFlags() & Qt::FramelessWindowHint)) {
        return {};
    }
    if (isMaximized() || isFullScreen()) {
        return {};
    }
    Qt::Edges edges;
    if (pos.x() <= kResizeMargin) {
        edges |= Qt::LeftEdge;
    } else if (pos.x() >= width() - kResizeMargin) {
        edges |= Qt::RightEdge;
    }
    if (pos.y() <= kResizeMargin) {
        edges |= Qt::TopEdge;
    } else if (pos.y() >= height() - kResizeMargin) {
        edges |= Qt::BottomEdge;
    }
    return edges;
}

void NetworkDiagnosticsDialog::updateResizeCursor(const QPoint& pos)
{
    const Qt::Edges edges = edgesAt(pos);
    Qt::CursorShape shape = Qt::ArrowCursor;
    if ((edges & (Qt::LeftEdge | Qt::TopEdge))     == (Qt::LeftEdge | Qt::TopEdge)
        || (edges & (Qt::RightEdge | Qt::BottomEdge)) == (Qt::RightEdge | Qt::BottomEdge)) {
        shape = Qt::SizeFDiagCursor;
    } else if ((edges & (Qt::RightEdge | Qt::TopEdge))    == (Qt::RightEdge | Qt::TopEdge)
        ||     (edges & (Qt::LeftEdge | Qt::BottomEdge)) == (Qt::LeftEdge | Qt::BottomEdge)) {
        shape = Qt::SizeBDiagCursor;
    } else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
        shape = Qt::SizeHorCursor;
    } else if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
        shape = Qt::SizeVerCursor;
    }
    setCursor(shape);
}

void NetworkDiagnosticsDialog::mouseMoveEvent(QMouseEvent* ev)
{
    if (!(ev->buttons() & Qt::LeftButton)) {
        updateResizeCursor(ev->pos());
    }
    QDialog::mouseMoveEvent(ev);
}

void NetworkDiagnosticsDialog::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() == Qt::LeftButton) {
        const Qt::Edges edges = edgesAt(ev->pos());
        if (edges) {
            if (auto* h = windowHandle()) {
                h->startSystemResize(edges);
                ev->accept();
                return;
            }
        }
    }
    QDialog::mousePressEvent(ev);
}

void NetworkDiagnosticsDialog::leaveEvent(QEvent* ev)
{
    setCursor(Qt::ArrowCursor);
    QDialog::leaveEvent(ev);
}

bool NetworkDiagnosticsDialog::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_titleBar && ev->type() == QEvent::MouseMove) {
        return FramelessMoveHelper::move(m_titleBar, static_cast<QMouseEvent*>(ev));
    }
    if (obj == m_titleBar && ev->type() == QEvent::MouseButtonRelease) {
        return FramelessMoveHelper::finish(m_titleBar, static_cast<QMouseEvent*>(ev));
    }

    // Drag-to-move via the custom title bar.  The trio buttons are
    // their own QPushButtons that consume the press themselves, so
    // this only fires on the bare title-bar background.
    if (obj == m_titleBar && ev->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(ev);
        return FramelessMoveHelper::start(m_titleBar, me);
    }
    if (obj == m_titleBar && ev->type() == QEvent::MouseButtonDblClick) {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
        ev->accept();
        return true;
    }
    return QDialog::eventFilter(obj, ev);
}

} // namespace AetherSDR
