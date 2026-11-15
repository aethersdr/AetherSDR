#include "gui/AprsRateGraph.h"

#include "core/ThemeManager.h"

#include <QColor>
#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>

namespace AetherSDR {

AprsRateGraph::AprsRateGraph(QWidget* parent)
    : QWidget(parent)
    , m_counts(kBuckets, 0)
{
    m_headStartMs = QDateTime::currentMSecsSinceEpoch();
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void AprsRateGraph::setTitle(const QString& title)
{
    m_title = title;
    setAccessibleName(title);
    setAccessibleDescription(QStringLiteral("Packet activity histogram over the selected time window."));
    update();
}

void AprsRateGraph::setAccentToken(const QString& token)
{
    m_accentToken = token.isEmpty() ? QStringLiteral("color.accent") : token;
    update();
}

void AprsRateGraph::setWindowMinutes(int minutes)
{
    m_windowMin = qBound(1, minutes, kMaxHours * 60);
    update();
}

void AprsRateGraph::advanceBucket()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsed = now - m_headStartMs;
    const int steps = int(elapsed / (kBucketSecs * 1000));
    if (steps <= 0)
        return;
    const int advance = qMin(steps, kBuckets);
    for (int i = 0; i < advance; ++i) {
        m_head = (m_head + 1) % kBuckets;
        m_counts[m_head] = 0;
    }
    m_headStartMs += qint64(advance) * kBucketSecs * 1000;
}

void AprsRateGraph::recordEvent()
{
    advanceBucket();
    m_counts[m_head] += 1;
    update();
}

int AprsRateGraph::eventsInWindow() const
{
    const int want = qBound(1, (m_windowMin * 60) / kBucketSecs, kBuckets);
    int sum = 0;
    for (int i = 0; i < want; ++i) {
        const int idx = (m_head - i + kBuckets) % kBuckets;
        sum += m_counts.at(idx);
    }
    return sum;
}

void AprsRateGraph::paintEvent(QPaintEvent*)
{
    const_cast<AprsRateGraph*>(this)->advanceBucket();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    const auto& theme = ThemeManager::instance();
    const QColor accent = theme.color(this, m_accentToken);
    QColor border = accent;
    border.setAlpha(50);
    p.setPen(QPen(border, 1));
    p.setBrush(theme.color(this, QStringLiteral("color.background.0")));
    p.drawRoundedRect(r, 4, 4);

    const int want = qBound(1, (m_windowMin * 60) / kBucketSecs, kBuckets);
    QVector<int> window;
    window.reserve(want);
    int peak = 1;
    for (int i = want - 1; i >= 0; --i) {
        const int idx = (m_head - i + kBuckets) % kBuckets;
        const int v = m_counts.at(idx);
        window.push_back(v);
        peak = qMax(peak, v);
    }

    const QRectF plot = r.adjusted(10, 22, -10, -14);
    if (plot.width() > 2 && plot.height() > 2 && !window.isEmpty()) {
        QPainterPath path;
        QPainterPath fill;
        const qreal step = plot.width() / qMax(1, window.size() - 1);
        for (int i = 0; i < window.size(); ++i) {
            const qreal x = plot.left() + i * step;
            const qreal y = plot.bottom()
                - (double(window.at(i)) / double(peak)) * plot.height();
            if (i == 0) {
                path.moveTo(x, y);
                fill.moveTo(x, plot.bottom());
                fill.lineTo(x, y);
            } else {
                path.lineTo(x, y);
                fill.lineTo(x, y);
            }
        }
        fill.lineTo(plot.right(), plot.bottom());
        fill.closeSubpath();
        QColor wash = accent;
        wash.setAlpha(50);
        p.fillPath(fill, wash);
        QPen pen(accent, 1.6);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    p.setPen(theme.color(this, QStringLiteral("color.text.secondary")));
    QFont f = font();
    f.setPixelSize(11);
    f.setLetterSpacing(QFont::PercentageSpacing, 108);
    p.setFont(f);
    const QString header = m_title.isEmpty()
        ? QStringLiteral("%1  ·  %2 min")
              .arg(eventsInWindow())
              .arg(m_windowMin)
        : QStringLiteral("%1  ·  %2  ·  %3 min")
              .arg(m_title, QString::number(eventsInWindow()),
                   QString::number(m_windowMin));
    p.drawText(r.adjusted(10, 6, -10, 0), Qt::AlignLeft | Qt::AlignTop, header);
}

} // namespace AetherSDR
