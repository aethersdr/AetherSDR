#include "WfmLockScope.h"

#include "core/ThemeManager.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QHideEvent>
#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace AetherSDR {
namespace {
class WfmLockScopeAccessible final : public QAccessibleWidget {
public:
    explicit WfmLockScopeAccessible(QWidget* widget)
        : QAccessibleWidget(widget, QAccessible::Indicator) {}
    QString text(QAccessible::Text kind) const override
    {
        if (kind == QAccessible::Value && widget()) {
            return widget()->accessibleDescription();
        }
        return QAccessibleWidget::text(kind);
    }
};

QAccessibleInterface* scopeAccessibleFactory(const QString& key, QObject* object)
{
    if (key == QLatin1String("AetherSDR::WfmLockScope")) {
        return new WfmLockScopeAccessible(qobject_cast<QWidget*>(object));
    }
    return nullptr;
}

void publishAccessibleValue(QWidget* widget, const QString& value)
{
    if (widget->accessibleDescription() == value) { return; }
    widget->setAccessibleDescription(value);
    // Observation publication is bounded to 4 Hz plus real state changes.
    QAccessibleValueChangeEvent event(widget, value);
    QAccessible::updateAccessibility(&event);
}

QString stateText(WfmStereoStatus state, bool forceMono)
{
    if (forceMono) {
        return QStringLiteral("Forced mono");
    }
    switch (state) {
    case WfmStereoStatus::Stereo: return QStringLiteral("Stereo locked");
    case WfmStereoStatus::Mono: return QStringLiteral("Mono fallback");
    case WfmStereoStatus::Acquiring: return QStringLiteral("Acquiring");
    case WfmStereoStatus::Unavailable: return QStringLiteral("Unavailable");
    }
    return QStringLiteral("Unavailable");
}
}

WfmLockScope::WfmLockScope(QWidget* parent) : QWidget(parent)
{
    static const bool factoryInstalled = [] {
        QAccessible::installFactory(scopeAccessibleFactory);
        return true;
    }();
    Q_UNUSED(factoryInstalled);
    setObjectName(QStringLiteral("wfmLockScope"));
    setAccessibleName(QStringLiteral("WFM pilot lock scope"));
    setToolTip(QStringLiteral(
        "Measured 19 kHz pilot magnitude and the decoder's acquire/release "
        "thresholds, in relative detector units. Stereo also requires consecutive "
        "decoder blocks above or below the threshold. Up to 40 seconds of current "
        "receiver observations; state strip shows actual stereo/mono output. "
        "This is not SNR or calibrated RF power."));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMinimumSize(minimumSizeHint());
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, qOverload<>(&WfmLockScope::update));
    clear();
}

QSize WfmLockScope::sizeHint() const { return {260, 136}; }
QSize WfmLockScope::minimumSizeHint() const { return {210, 116}; }

void WfmLockScope::appendSample(double pilotMagnitude, double acquireThreshold,
                              double releaseThreshold, WfmStereoStatus status,
                              bool forceMono)
{
    // Hidden scope means no history growth, timer reads or repaint requests.
    // A show starts with fresh telemetry, never a retained green strip.
    if (!isVisible()) {
        return;
    }
    if (!std::isfinite(pilotMagnitude) || pilotMagnitude < 0.0
        || !std::isfinite(acquireThreshold) || acquireThreshold <= 0.0
        || !std::isfinite(releaseThreshold) || releaseThreshold < 0.0
        || releaseThreshold > acquireThreshold
        || status == WfmStereoStatus::Unavailable) {
        clear();
        return;
    }
    if (!m_clock.isValid()) {
        m_clock.start();
    }
    const qint64 now = m_clock.elapsed();
    while (!m_samples.isEmpty()
           && (m_samples.size() >= kCapacity
               || now - m_samples.first().milliseconds > kWindowMs)) {
        m_samples.removeFirst();
    }
    m_samples.append({now, pilotMagnitude, acquireThreshold, releaseThreshold,
                      status, forceMono});
    publishAccessibleValue(this,
        QStringLiteral("19 kHz pilot %1 relative units. Acquire threshold %2; "
                       "release threshold %3. %4. %5 current receiver observations. "
                       "Threshold crossings require consecutive decoder blocks.")
            .arg(pilotMagnitude, 0, 'g', 4).arg(acquireThreshold, 0, 'g', 4)
            .arg(releaseThreshold, 0, 'g', 4).arg(stateText(status, forceMono))
            .arg(m_samples.size()));
    update();
}

void WfmLockScope::clear()
{
    m_samples.clear();
    m_clock.invalidate();
    publishAccessibleValue(this, QStringLiteral("Awaiting current receiver pilot telemetry."));
    if (isVisible()) {
        update();
    }
}

void WfmLockScope::hideEvent(QHideEvent* event)
{
    clear();
    QWidget::hideEvent(event);
}

void WfmLockScope::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    ThemeManager& theme = ThemeManager::instance();
    const QColor text = theme.color(this, "color.text.secondary");
    const QColor trace = theme.color(this, "color.text.primary");
    const QColor success = theme.color(this, "color.accent.success");
    const QColor warning = theme.color(this, "color.accent.warning");
    const QColor neutral = theme.color(this, "color.text.disabled");
    painter.fillRect(rect(), theme.brush(this, "color.background.0", rect()));
    painter.setPen(theme.color(this, "color.border.subtle"));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
    painter.setPen(text);
    const int lineHeight = fontMetrics().height();
    painter.drawText(QRect(5, 3, width() - 10, lineHeight), Qt::AlignLeft,
                     QStringLiteral("19 kHz pilot · relative units"));
    if (m_samples.isEmpty()) {
        painter.drawText(rect().adjusted(5, lineHeight + 5, -5, -5),
                         Qt::AlignCenter, QStringLiteral("Awaiting pilot telemetry"));
        return;
    }
    const QRectF plot(6, lineHeight + 7, width() - 12,
                      std::max(20, height() - 3 * lineHeight - 28));
    const Sample& latest = m_samples.last();
    double maximum = latest.acquire;
    for (const Sample& sample : m_samples) {
        maximum = std::max(maximum, sample.magnitude);
        maximum = std::max(maximum, sample.acquire);
    }
    const auto yOf = [&](double value) {
        return plot.bottom() - std::clamp(value / maximum, 0.0, 1.0) * plot.height();
    };
    const auto xOf = [&](const Sample& sample) {
        return plot.right() - double(latest.milliseconds - sample.milliseconds)
                                  / double(kWindowMs) * plot.width();
    };
    // Threshold labels are textual as well as differently styled lines.
    painter.setPen(QPen(success, 1, Qt::DashLine));
    painter.drawLine(QPointF(plot.left(), yOf(latest.acquire)),
                     QPointF(plot.right(), yOf(latest.acquire)));
    painter.drawText(QPointF(plot.left() + 2, yOf(latest.acquire) - 2),
                     QStringLiteral("Acquire %1").arg(latest.acquire, 0, 'g', 3));
    painter.setPen(QPen(warning, 1, Qt::DotLine));
    painter.drawLine(QPointF(plot.left(), yOf(latest.release)),
                     QPointF(plot.right(), yOf(latest.release)));
    painter.drawText(QPointF(plot.left() + 2, yOf(latest.release) - 2),
                     QStringLiteral("Release %1").arg(latest.release, 0, 'g', 3));
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(trace, 1.5));
    QPolygonF points;
    points.reserve(m_samples.size());
    for (const Sample& sample : m_samples) {
        points.append(QPointF(xOf(sample), yOf(sample.magnitude)));
    }
    painter.drawPolyline(points);
    painter.drawEllipse(points.last(), 2.0, 2.0);
    // Status is observed decoder output, not inferred from one amplitude.
    const qreal stripY = plot.bottom() + 4;
    for (qsizetype i = 0; i < m_samples.size(); ++i) {
        const Sample& sample = m_samples[i];
        const QColor color = !sample.forceMono && sample.status == WfmStereoStatus::Stereo
            ? success : (!sample.forceMono && sample.status == WfmStereoStatus::Acquiring
                ? warning : neutral);
        const qreal left = xOf(sample);
        const qreal right = i + 1 < m_samples.size()
            ? xOf(m_samples[i + 1]) : plot.right();
        painter.fillRect(QRectF(left, stripY, std::max(2.0, right - left), 4), color);
    }
    painter.setPen(text);
    painter.drawText(QRect(6, int(stripY) + 8, width() - 12, lineHeight),
                     Qt::AlignLeft, stateText(latest.status, latest.forceMono));
    painter.drawText(QRect(6, height() - lineHeight - 2, width() - 12, lineHeight),
                     Qt::AlignRight, QStringLiteral("← up to 40 s · now"));
}

} // namespace AetherSDR
