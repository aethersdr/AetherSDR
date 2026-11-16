#include "AetherPDMeasurementSpectrum.h"
#include "core/ThemeManager.h"

#include <QAccessibleWidget>
#include <QBuffer>
#include <QColor>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QPaintEvent>
#include <QPixmap>
#include <QPolygonF>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace AetherSDR {
namespace {
class AetherPDMeasurementSpectrumAccessible final : public QAccessibleWidget {
public:
    explicit AetherPDMeasurementSpectrumAccessible(QWidget* widget)
        : QAccessibleWidget(widget, QAccessible::Graphic) {}
    QString text(QAccessible::Text type) const override
    {
        return type == QAccessible::Value && widget() ? widget()->accessibleDescription()
                                                     : QAccessibleWidget::text(type);
    }
};
QAccessibleInterface* spectrumAccessible(const QString&, QObject* object)
{
    if (auto* view = qobject_cast<AetherPDMeasurementSpectrum*>(object)) {
        return new AetherPDMeasurementSpectrumAccessible(view);
    }
    return nullptr;
}
}

AetherPDMeasurementSpectrum::AetherPDMeasurementSpectrum(QWidget* parent) : QWidget(parent)
{
    static const bool registered = [] { QAccessible::installFactory(spectrumAccessible); return true; }();
    Q_UNUSED(registered);
    setObjectName(QStringLiteral("pdMeasureSpectrum"));
    setAccessibleName(tr("AetherPD measurement spectrum"));
    setMinimumSize(440, 270);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { update(); });
    setResult(nullptr);
}

void AetherPDMeasurementSpectrum::setResult(const AetherPDMeasurementModel::Result* result)
{
    m_result.reset();
    m_caption = tr("No valid completed measurement.");
    QString summary = m_caption;
    if (result && result->measurement.measurement) {
        const txlin::LinearityResult& value = *result->measurement.measurement;
        const auto finiteTone = [](const txlin::TonePeak& tone) {
            return tone.valid && std::isfinite(tone.freqHz) && std::isfinite(tone.powerDb)
                && std::abs(tone.powerDb) <= 1000;
        };
        if (value.valid && value.spectrumDb.size() >= 2 && value.spectrumDb.size() <= 1048576
            && value.fftSize == static_cast<int>(value.spectrumDb.size())
            && std::isfinite(value.binHz) && value.binHz > 0 && std::isfinite(value.sampleRateHz)
            && value.sampleRateHz > 0 && value.sampleRateHz <= 1e9
            && std::abs(value.binHz * value.fftSize - value.sampleRateHz) <= value.sampleRateHz * 1e-6
            && finiteTone(value.tone1) && finiteTone(value.tone2)
            && std::abs(value.tone1.freqHz) <= value.sampleRateHz / 2
            && std::abs(value.tone2.freqHz) <= value.sampleRateHz / 2
            && std::isfinite(value.toneSpacingHz) && value.toneSpacingHz > 0
            && std::isfinite(value.noiseFloorDb) && std::abs(value.noiseFloorDb) <= 1000
            && std::ranges::all_of(value.spectrumDb, [](float db) { return std::isfinite(db); })) {
            m_result = value;
            m_centerHz = (value.tone1.freqHz + value.tone2.freqHz) / 2;
            double reach = value.toneSpacingHz * 2;
            for (const txlin::ImdProduct& product : value.products) {
                if (std::isfinite(product.freqHz)) {
                    reach = std::max(reach, std::abs(product.freqHz - m_centerHz) + value.toneSpacingHz);
                }
            }
            for (const std::optional<txlin::BandPower>* band : {&value.shoulderLow, &value.shoulderHigh, &value.acprLow, &value.acprHigh}) {
                if (*band && std::isfinite((*band)->lowHz) && std::isfinite((*band)->highHz)) {
                    reach = std::max({reach, std::abs((*band)->lowHz - m_centerHz), std::abs((*band)->highHz - m_centerHz)});
                }
            }
            m_spanHz = std::min(reach * 1.05, value.sampleRateHz / 2);
            m_topDb = std::max(value.tone1.powerDb, value.tone2.powerDb) + 12;
            m_bottomDb = std::min(value.noiseFloorDb - 12, m_topDb - 60);
            m_caption = tr("%1 MHz · %2 W setting · correction %3 · %4 UTC")
                .arg(result->config.capture.centerFreqHz / 1e6, 0, 'f', 6).arg(result->powerSetting)
                .arg(result->measurement.correctionActive ? (*result->measurement.correctionActive ? tr("on") : tr("off")) : tr("unknown"))
                .arg(result->completedAtUtc.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
            m_caption += tr(" · timeout %1 s · ").arg(result->config.timeoutMs / 1000) + result->sourceSummary();
            summary = m_caption + tr(". Uncalibrated raw IQ level; ADC headroom unknown. Tone spacing %1 Hz. Noise floor %2 dB raw IQ per bin. ")
                .arg(value.toneSpacingHz, 0, 'f', 1).arg(value.noiseFloorDb, 0, 'f', 1);
            for (const txlin::ImdProduct& product : value.products) {
                summary += tr("IMD%1 %2: %3. ").arg(product.order).arg(product.upper ? tr("upper") : tr("lower"))
                    .arg(product.limitedByNoiseFloor || !std::isfinite(product.dbcPerTone)
                        ? tr("noise limited or unavailable") : tr("%1 dBc per tone").arg(product.dbcPerTone, 0, 'f', 1));
            }
            const auto describeBand = [&summary](const std::optional<txlin::BandPower>& band, const QString& name) {
                if (!band) { summary += tr("%1 unavailable. ").arg(name); return; }
                summary += tr("%1: %2 to %3 Hz from capture center. ").arg(name)
                    .arg(band->lowHz, 0, 'f', 1).arg(band->highHz, 0, 'f', 1);
            };
            describeBand(value.shoulderLow, tr("Lower shoulder"));
            describeBand(value.shoulderHigh, tr("Upper shoulder"));
            describeBand(value.acprLow, tr("Lower ACPR"));
            describeBand(value.acprHigh, tr("Upper ACPR"));
        } else if (!value.invalidReason.empty()) {
            m_caption = QString::fromStdString(value.invalidReason);
            summary = m_caption;
        }
    }
    setAccessibleDescription(summary);
    update();
    QAccessibleEvent event(this, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&event);
}

QByteArray AetherPDMeasurementSpectrum::exportPng()
{
    if (!m_result) { return {}; }
    QByteArray output;
    QBuffer buffer(&output);
    QImage image = grab().toImage();
    image.setText(QStringLiteral("Description"), accessibleDescription());
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) { return {}; }
    return output;
}

double AetherPDMeasurementSpectrum::x(double hz, const QRectF& area) const
{
    return area.left() + area.width() * (hz - m_centerHz + m_spanHz) / (2 * m_spanHz);
}
double AetherPDMeasurementSpectrum::y(double db, const QRectF& area) const
{
    return area.top() + area.height() * (m_topDb - std::clamp(db, m_bottomDb, m_topDb)) / (m_topDb - m_bottomDb);
}

void AetherPDMeasurementSpectrum::drawSpectrum(QPainter& painter, const QRectF& area) const
{
    const txlin::LinearityResult& value = *m_result;
    ThemeManager& theme = ThemeManager::instance();
    const QColor ink = theme.color(this, QStringLiteral("color.canon.inkSoft"));
    const QColor cyan = theme.color(this, QStringLiteral("color.canon.cyan"));
    const QColor grid = theme.color(this, QStringLiteral("color.canon.lineHi"));
    const QFontMetricsF metrics(painter.font());
    painter.setPen(ink);
    painter.drawText(QPointF(8, 35), tr("dB raw IQ (uncalibrated) · products: dBc per tone"));
    for (double db = std::ceil(m_bottomDb / 20) * 20; db <= m_topDb; db += 20) {
        painter.setPen(QPen(grid, 1, Qt::DotLine));
        painter.drawLine(QPointF(area.left(), y(db, area)), QPointF(area.right(), y(db, area)));
        painter.setPen(ink);
        painter.drawText(QRectF(0, y(db, area) - metrics.height() / 2, area.left() - 6, metrics.height()),
                         Qt::AlignRight, QString::number(db, 'f', 0));
    }
    for (int i = -2; i <= 2; ++i) {
        const double offset = i * m_spanHz / 2;
        const double px = x(m_centerHz + offset, area);
        painter.setPen(QPen(grid, 1, Qt::DotLine));
        painter.drawLine(QPointF(px, area.top()), QPointF(px, area.bottom()));
        painter.setPen(ink);
        painter.drawText(QRectF(px - 35, area.bottom() + 3, 70, metrics.height()), Qt::AlignHCenter,
                         QString::number(offset / 1000, 'f', 1));
    }
    painter.drawText(QPointF(area.left(), height() - 6), tr("kHz from tone midpoint · ADC headroom unknown"));
    painter.save();
    painter.setClipRect(area.adjusted(-1, -1, 1, 1));
    const auto band = [&](const std::optional<txlin::BandPower>& b, const QString& name) {
        if (!b || !std::isfinite(b->lowHz) || !std::isfinite(b->highHz) || b->highHz <= b->lowHz) { return; }
        const QRectF rect(QPointF(x(b->lowHz, area), area.top()), QPointF(x(b->highHz, area), area.bottom()));
        const QRectF visible = rect.intersected(area);
        if (visible.isEmpty()) { return; }
        painter.fillRect(visible, grid);
        painter.setPen(ink);
        painter.drawText(visible.adjusted(2, 0, -2, -3), Qt::AlignBottom | Qt::AlignHCenter, name);
    };
    band(value.acprLow, tr("ACPR")); band(value.acprHigh, tr("ACPR"));
    band(value.shoulderLow, tr("Shoulder")); band(value.shoulderHigh, tr("Shoulder"));
    // Keep the maximum in each display column: sampling bins can hide a narrow IMD peak.
    const int columns = std::max(1, static_cast<int>(area.width()));
    std::vector<double> peaks(static_cast<size_t>(columns), -std::numeric_limits<double>::infinity());
    for (size_t i = 0; i < value.spectrumDb.size(); ++i) {
        const double hz = (static_cast<double>(i) - value.fftSize / 2) * value.binHz;
        const double px = x(hz, area) - area.left();
        if (px < 0 || px >= columns) { continue; }
        const size_t column = static_cast<size_t>(px);
        peaks[column] = std::max(peaks[column], static_cast<double>(value.spectrumDb[i]));
    }
    QPolygonF trace;
    for (int i = 0; i < columns; ++i) {
        if (std::isfinite(peaks[static_cast<size_t>(i)])) {
            trace << QPointF(area.left() + i, y(peaks[static_cast<size_t>(i)], area));
        }
    }
    painter.setPen(QPen(cyan, 1.4)); painter.drawPolyline(trace);
    const double floorY = y(value.noiseFloorDb, area);
    painter.setPen(QPen(theme.color(this, QStringLiteral("color.accent.warning")), 1, Qt::DashLine));
    painter.drawLine(QPointF(area.left(), floorY), QPointF(area.right(), floorY));
    painter.drawText(QPointF(area.left() + 4, floorY - 4), tr("noise floor"));
    const auto mark = [&](double hz, double db, const QString& name, const QColor& color) {
        if (!std::isfinite(hz) || !std::isfinite(db)) { return; }
        const double px = x(hz, area), py = y(db, area);
        if (px < area.left() || px > area.right()) { return; }
        painter.setPen(color);
        painter.drawLine(QPointF(px, py), QPointF(px, py - 8));
        const double labelWidth = metrics.horizontalAdvance(name);
        const double left = std::clamp(px - labelWidth / 2, area.left(), std::max(area.left(), area.right() - labelWidth));
        painter.drawText(QPointF(left, std::max(area.top() + metrics.ascent(), py - 11)), name);
    };
    mark(value.tone1.freqHz, value.tone1.powerDb, QStringLiteral("f1"), cyan);
    mark(value.tone2.freqHz, value.tone2.powerDb, QStringLiteral("f2"), cyan);
    for (const txlin::ImdProduct& product : value.products) {
        const QString level = product.limitedByNoiseFloor || !std::isfinite(product.dbcPerTone)
            ? tr("noise") : QString::number(product.dbcPerTone, 'f', 1);
        mark(product.freqHz, product.powerDb, tr("IMD%1 %2").arg(product.order).arg(level), ink);
    }
    painter.restore();
}

void AetherPDMeasurementSpectrum::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    ThemeManager& theme = ThemeManager::instance();
    painter.fillRect(rect(), theme.color(this, QStringLiteral("color.canon.nested")));
    painter.setPen(theme.color(this, QStringLiteral("color.canon.inkSoft")));
    if (!m_result) {
        painter.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap, m_caption);
        return;
    }
    painter.drawText(QRectF(8, 3, width() - 16, 24), Qt::AlignLeft, fontMetrics().elidedText(m_caption, Qt::ElideRight, width() - 16));
    drawSpectrum(painter, QRectF(52, 48, std::max(1, width() - 74), std::max(1, height() - 96)));
}
} // namespace AetherSDR
