#include "WaveformWidget.h"

#include "core/AppSettings.h"

#include <QApplication>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

// WAVE is intentionally QPainter-only for v1: the sidebar scope is small,
// cross-platform, and cheap to repaint; future QRhi support can follow the
// SpectrumWidget path without making GPU builds depend on another renderer now.
namespace {

constexpr int kDefaultSampleRate = 24000;
constexpr int kMinSampleRate = 8000;
constexpr int kMaxSampleRate = 192000;
constexpr int kNoAudioTimeoutMs = 1000;
constexpr int kMinWindowMs = 40;
// Lifted from 240 ms to 20 s so the WaveApplet drawer's "Window" slider
// (1–20 s, matches StripWaveformPanel) can take effect.  ensureCapacity
// scales the ring buffer to fit the current window, so larger values
// only allocate when the user picks them.
constexpr int kMaxWindowMs = 20000;
constexpr int kMinRefreshRateHz = 5;
constexpr int kMaxRefreshRateHz = 30;
constexpr double kPi = 3.14159265358979323846;

const QColor kBackground(0x0a, 0x0a, 0x14);
const QColor kGridMajor(0x30, 0x40, 0x50, 130);
const QColor kGridMinor(0x18, 0x28, 0x38, 150);
const QColor kCenterLine(0x58, 0x78, 0x90, 150);
const QColor kWaveFallback(0x00, 0xe5, 0xff);
const QColor kPeakColor(0x00, 0xb4, 0xd8, 180);
const QColor kRmsColor(0x20, 0xc0, 0x60, 210);
const QColor kLabelColor(0xd8, 0xe6, 0xf0);
const QColor kMutedLabel(0x90, 0xa0, 0xb0);
const QColor kClipColor(0xff, 0x50, 0x50);
const QColor kBarEmpty(0x14, 0x24, 0x32, 170);
const QColor kBarPeakHold(0xff, 0xd1, 0x66);

QColor waveformColor()
{
    QColor c(AppSettings::instance().value("DisplayFftFillColor", "#00e5ff").toString());
    return c.isValid() ? c : kWaveFallback;
}

float waveformLineWidth()
{
    const float w = AppSettings::instance().value("DisplayFftLineWidth", "2.0").toFloat();
    return std::clamp(w, 1.0f, 3.0f);
}

bool showGrid()
{
    return AppSettings::instance().value("DisplayShowGrid", "True").toString() == "True";
}

QString formatSampleRate(int sampleRate)
{
    if (sampleRate % 1000 == 0)
        return QStringLiteral("%1 kHz").arg(sampleRate / 1000);
    return QStringLiteral("%1 kHz").arg(sampleRate / 1000.0, 0, 'f', 1);
}

} // namespace

WaveformWidget::WaveformWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(minimumSizeHint());
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setToolTip("Click to pause/resume waveform capture; double-click for WAVE settings");

    m_clickTimer.setSingleShot(true);
    m_clickTimer.setInterval(QApplication::doubleClickInterval());
    connect(&m_clickTimer, &QTimer::timeout, this, [this]() {
        setPaused(!m_paused);
    });
}

void WaveformWidget::appendScopeSamples(const QByteArray& monoFloat32Pcm,
                                        int sampleRate,
                                        bool tx)
{
    if (m_paused || monoFloat32Pcm.isEmpty())
        return;

    const int samples = monoFloat32Pcm.size() / static_cast<int>(sizeof(float));
    if (samples <= 0)
        return;

    const auto* src = reinterpret_cast<const float*>(monoFloat32Pcm.constData());
    appendToRing(tx ? m_tx : m_rx, src, samples, sanitizeSampleRate(sampleRate));
    scheduleRepaint();
}

void WaveformWidget::setTransmitting(bool tx)
{
    if (m_transmitting == tx)
        return;
    m_transmitting = tx;
    update();
}

void WaveformWidget::clear()
{
    clearRing(m_rx);
    clearRing(m_tx);
    update();
}

void WaveformWidget::setViewMode(ViewMode mode)
{
    if (m_viewMode == mode)
        return;
    m_viewMode = mode;
    update();
}

void WaveformWidget::setZoomWindowMs(int windowMs)
{
    const int sanitized = sanitizeWindowMs(windowMs);
    if (m_windowMs == sanitized)
        return;

    m_windowMs = sanitized;
    if (m_paused) {
        const RingBuffer& buffer = activeBuffer();
        m_pausedSampleRate = sanitizeSampleRate(buffer.sampleRate);
        const int windowSamples = std::max(1, m_pausedSampleRate * m_windowMs / 1000);
        copyLatest(buffer, windowSamples, m_pausedSamples);
    }
    update();
}

void WaveformWidget::setRefreshRateHz(int hz)
{
    const int sanitized = sanitizeRefreshRateHz(hz);
    if (m_refreshRateHz == sanitized)
        return;
    m_refreshRateHz = sanitized;
    update();
}

void WaveformWidget::setAmplitudeZoom(float zoom)
{
    const float sanitized = sanitizeAmplitudeZoom(zoom);
    if (std::abs(m_amplitudeZoom - sanitized) < 0.01f)
        return;
    m_amplitudeZoom = sanitized;
    update();
}

void WaveformWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(rect(), kBackground);

    QRectF plotRect = QRectF(rect()).adjusted(5.0, 18.0, -34.0, -17.0);
    if (plotRect.width() < 24.0 || plotRect.height() < 36.0)
        plotRect = QRectF(rect()).adjusted(4.0, 17.0, -30.0, -16.0);

    const RingBuffer& buffer = activeBuffer();
    const QString source = (m_paused ? m_pausedTransmitting : m_transmitting)
        ? QStringLiteral("TX")
        : QStringLiteral("RX");
    const int sampleRate = sanitizeSampleRate(m_paused
        ? m_pausedSampleRate
        : buffer.sampleRate);
    if (m_paused) {
        m_displaySamples = m_pausedSamples;
    } else {
        const int windowSamples = std::max(1, sampleRate * m_windowMs / 1000);
        copyLatest(buffer, windowSamples, m_displaySamples);
    }

    float peak = 0.0f;
    double sumSq = 0.0;
    int clipCount = 0;
    for (float s : m_displaySamples) {
        const float a = std::abs(s);
        peak = std::max(peak, a);
        sumSq += static_cast<double>(s) * s;
        if (a >= 0.98f)
            ++clipCount;
    }
    const float rms = m_displaySamples.isEmpty()
        ? 0.0f
        : static_cast<float>(std::sqrt(sumSq / m_displaySamples.size()));
    const float peakDb = linearToDb(peak);
    const float rmsDb = linearToDb(rms);

    if (m_viewMode == ViewMode::VerticalBars) {
        drawBarsGrid(painter, plotRect);
        if (!m_displaySamples.isEmpty())
            drawVerticalBars(painter, plotRect, sampleRate);
    } else if (m_viewMode == ViewMode::Envelope) {
        drawGrid(painter, plotRect, sampleRate);
        if (!m_displaySamples.isEmpty())
            drawEnvelope(painter, plotRect, clipCount);
    } else if (m_viewMode == ViewMode::Bars) {
        drawBarsGrid(painter, plotRect);
        if (!m_displaySamples.isEmpty())
            drawBars(painter, plotRect);
    } else {
        drawGrid(painter, plotRect, sampleRate);
        if (!m_displaySamples.isEmpty())
            drawGraph(painter, plotRect, clipCount);
    }

    QFont labelFont = font();
    labelFont.setPointSizeF(std::max(7.0, labelFont.pointSizeF() - 1.0));
    painter.setFont(labelFont);
    painter.setPen(kLabelColor);
    const QString readout = QStringLiteral("%1  RMS %2 dBFS  PK %3 dBFS")
        .arg(source)
        .arg(rmsDb, 0, 'f', 1)
        .arg(peakDb, 0, 'f', 1);
    painter.drawText(QRectF(7.0, 3.0, width() - 14.0, 16.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     readout);

    if (clipCount > 0) {
        QFont clipFont = labelFont;
        clipFont.setBold(true);
        painter.setFont(clipFont);
        painter.setPen(kClipColor);
        painter.drawText(QRectF(7.0, 3.0, width() - 14.0, 16.0),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("CLIP %1").arg(clipCount));
        painter.setFont(labelFont);
    }

    painter.setPen(kMutedLabel);
    const QString timeText = m_viewMode == ViewMode::VerticalBars
        ? QString::fromUtf8("%1 \xc2\xb7 %2 ms \xc2\xb7 frequency bands")
            .arg(formatSampleRate(sampleRate))
            .arg(m_windowMs)
        : QString::fromUtf8("%1 \xc2\xb7 %2 ms \xc2\xb7 %3 ms/div")
            .arg(formatSampleRate(sampleRate))
            .arg(m_windowMs)
            .arg(std::max(1, m_windowMs / 10));
    const QRectF footerRect(plotRect.left(), plotRect.bottom() + 2.0,
                            plotRect.width(), 15.0);
    painter.drawText(m_paused ? footerRect.adjusted(0.0, 0.0, -52.0, 0.0) : footerRect,
                     Qt::AlignLeft | Qt::AlignVCenter,
                     timeText);

    const bool stale = !m_paused
        && (!buffer.lastSamples.isValid()
        || buffer.lastSamples.elapsed() > kNoAudioTimeoutMs);
    if (m_displaySamples.isEmpty() || stale)
        drawNoAudio(painter, plotRect, source);

    if (m_paused)
        drawPausedBadge(painter, footerRect);
}

void WaveformWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_ignoreNextRelease) {
            m_ignoreNextRelease = false;
        } else if (!m_clickTimer.isActive()) {
            m_clickTimer.start();
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void WaveformWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_clickTimer.stop();
        m_ignoreNextRelease = true;
        emit settingsDrawerToggleRequested();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

WaveformWidget::RingBuffer& WaveformWidget::activeBuffer()
{
    return m_transmitting ? m_tx : m_rx;
}

const WaveformWidget::RingBuffer& WaveformWidget::activeBuffer() const
{
    return m_transmitting ? m_tx : m_rx;
}

void WaveformWidget::ensureCapacity(RingBuffer& buffer, int sampleRate)
{
    const int rate = sanitizeSampleRate(sampleRate);
    // Size the ring to hold the current window plus 1 s headroom.  Floor
    // at 1 s so a freshly-applied small window doesn't truncate samples
    // that arrived just before the change.  Round up to 4096-sample
    // chunks so a slider drag doesn't realloc on every notch.
    const int windowSamples = static_cast<int>(
        static_cast<int64_t>(rate) * std::max(1000, m_windowMs) / 1000);
    const int needed   = std::max(rate, windowSamples) + rate;
    const int capacity = ((needed + 4095) / 4096) * 4096;
    if (buffer.samples.size() >= capacity) {
        buffer.sampleRate = rate;
        return;
    }

    QVector<float> preserved;
    copyLatest(buffer, std::min(buffer.filled, capacity), preserved);

    buffer.samples = QVector<float>(capacity, 0.0f);
    buffer.writeIndex = 0;
    buffer.filled = 0;
    buffer.sampleRate = rate;

    const int newCapacity = buffer.samples.size();
    for (float s : preserved) {
        buffer.samples[buffer.writeIndex] = s;
        buffer.writeIndex = (buffer.writeIndex + 1) % newCapacity;
        buffer.filled = std::min(buffer.filled + 1, newCapacity);
    }
}

void WaveformWidget::appendToRing(RingBuffer& buffer,
                                  const float* samples,
                                  int count,
                                  int sampleRate)
{
    ensureCapacity(buffer, sampleRate);
    if (buffer.samples.isEmpty())
        return;

    const int capacity = buffer.samples.size();
    for (int i = 0; i < count; ++i) {
        buffer.samples[buffer.writeIndex] = clampSample(samples[i]);
        buffer.writeIndex = (buffer.writeIndex + 1) % capacity;
        buffer.filled = std::min(buffer.filled + 1, capacity);
    }
    buffer.lastSamples.restart();
}

void WaveformWidget::clearRing(RingBuffer& buffer)
{
    buffer.samples.fill(0.0f);
    buffer.writeIndex = 0;
    buffer.filled = 0;
    buffer.lastSamples.invalidate();
}

void WaveformWidget::copyLatest(const RingBuffer& buffer, int count, QVector<float>& out) const
{
    out.clear();
    if (count <= 0 || buffer.filled <= 0 || buffer.samples.isEmpty())
        return;

    count = std::min(count, buffer.filled);
    out.resize(count);

    const int capacity = buffer.samples.size();
    int start = buffer.writeIndex - count;
    while (start < 0)
        start += capacity;

    for (int i = 0; i < count; ++i)
        out[i] = buffer.samples[(start + i) % capacity];
}

void WaveformWidget::setPaused(bool paused)
{
    if (m_paused == paused)
        return;

    if (paused) {
        const RingBuffer& buffer = activeBuffer();
        m_pausedTransmitting = m_transmitting;
        m_pausedSampleRate = sanitizeSampleRate(buffer.sampleRate);
        const int windowSamples = std::max(1, m_pausedSampleRate * m_windowMs / 1000);
        copyLatest(buffer, windowSamples, m_pausedSamples);
    } else {
        m_pausedSamples.clear();
    }

    m_paused = paused;
    update();
}

void WaveformWidget::buildColumns(int columnCount)
{
    m_columns.clear();
    if (columnCount <= 0 || m_displaySamples.isEmpty())
        return;

    m_columns.resize(columnCount);
    const int n = m_displaySamples.size();

    for (int x = 0; x < columnCount; ++x) {
        int start = (x * n) / columnCount;
        int end = ((x + 1) * n) / columnCount;
        if (end <= start)
            end = start + 1;
        start = std::clamp(start, 0, n - 1);
        end = std::clamp(end, start + 1, n);

        float mn = 1.0f;
        float mx = -1.0f;
        float peak = 0.0f;
        double sumSq = 0.0;
        int clipped = 0;
        for (int i = start; i < end; ++i) {
            const float s = clampSample(m_displaySamples[i]);
            mn = std::min(mn, s);
            mx = std::max(mx, s);
            const float a = std::abs(s);
            peak = std::max(peak, a);
            sumSq += static_cast<double>(s) * s;
            if (a >= 0.98f)
                ++clipped;
        }

        ColumnStats& c = m_columns[x];
        c.min = mn;
        c.max = mx;
        c.peak = peak;
        c.rms = static_cast<float>(std::sqrt(sumSq / (end - start)));
        c.clipped = clipped;
    }
}

void WaveformWidget::drawGraph(QPainter& painter,
                               const QRectF& plotRect,
                               int clipCount)
{
    const int columnCount = std::max(1, static_cast<int>(std::floor(plotRect.width())));
    buildColumns(columnCount);
    if (m_columns.isEmpty())
        return;

    const qreal centerY = plotRect.center().y();
    const qreal halfHeight = std::max<qreal>(1.0, plotRect.height() * 0.5 - 2.0);
    const qreal left = plotRect.left();
    const QColor wave = waveformColor();

    QPainterPath peakTop;
    QPainterPath peakBottom;
    QPainterPath rmsTop;
    QPainterPath rmsBottom;

    painter.setPen(QPen(wave, waveformLineWidth(), Qt::SolidLine, Qt::RoundCap));
    for (int x = 0; x < m_columns.size(); ++x) {
        const ColumnStats& c = m_columns[x];
        const qreal px = left + x + 0.5;
        const qreal minY = centerY - std::clamp(c.min * m_amplitudeZoom, -1.0f, 1.0f) * halfHeight;
        const qreal maxY = centerY - std::clamp(c.max * m_amplitudeZoom, -1.0f, 1.0f) * halfHeight;
        painter.drawLine(QPointF(px, minY), QPointF(px, maxY));

        const qreal peak = std::clamp(c.peak * m_amplitudeZoom, 0.0f, 1.0f);
        const qreal rms = std::clamp(c.rms * m_amplitudeZoom, 0.0f, 1.0f);
        const qreal peakTopY = centerY - peak * halfHeight;
        const qreal peakBottomY = centerY + peak * halfHeight;
        const qreal rmsTopY = centerY - rms * halfHeight;
        const qreal rmsBottomY = centerY + rms * halfHeight;
        if (x == 0) {
            peakTop.moveTo(px, peakTopY);
            peakBottom.moveTo(px, peakBottomY);
            rmsTop.moveTo(px, rmsTopY);
            rmsBottom.moveTo(px, rmsBottomY);
        } else {
            peakTop.lineTo(px, peakTopY);
            peakBottom.lineTo(px, peakBottomY);
            rmsTop.lineTo(px, rmsTopY);
            rmsBottom.lineTo(px, rmsBottomY);
        }
    }

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(kPeakColor, 1.0));
    painter.drawPath(peakTop);
    painter.drawPath(peakBottom);
    painter.setPen(QPen(kRmsColor, 1.4));
    painter.drawPath(rmsTop);
    painter.drawPath(rmsBottom);
    painter.setRenderHint(QPainter::Antialiasing, false);

    if (clipCount > 0) {
        painter.setPen(QPen(kClipColor, 1.0));
        for (int x = 0; x < m_columns.size(); ++x) {
            if (m_columns[x].clipped <= 0)
                continue;
            const qreal px = left + x + 0.5;
            painter.drawLine(QPointF(px, plotRect.top()),
                             QPointF(px, plotRect.top() + 4.0));
            painter.drawLine(QPointF(px, plotRect.bottom() - 4.0),
                             QPointF(px, plotRect.bottom()));
        }
    }
}

void WaveformWidget::drawEnvelope(QPainter& painter,
                                  const QRectF& plotRect,
                                  int clipCount)
{
    const int columnCount = std::max(1, static_cast<int>(std::floor(plotRect.width())));
    buildColumns(columnCount);
    if (m_columns.isEmpty())
        return;

    const qreal centerY = plotRect.center().y();
    const qreal halfHeight = std::max<qreal>(1.0, plotRect.height() * 0.5 - 2.0);
    const qreal left = plotRect.left();
    const QColor wave = waveformColor();

    QVector<QPointF> rmsTop;
    QVector<QPointF> rmsBottom;
    QPainterPath peakTop;
    QPainterPath peakBottom;
    QPainterPath rmsLineTop;
    QPainterPath rmsLineBottom;

    rmsTop.reserve(m_columns.size());
    rmsBottom.reserve(m_columns.size());

    for (int x = 0; x < m_columns.size(); ++x) {
        const ColumnStats& c = m_columns[x];
        const qreal px = left + x + 0.5;
        const qreal peak = std::clamp(c.peak * m_amplitudeZoom, 0.0f, 1.0f);
        const qreal rms = std::clamp(c.rms * m_amplitudeZoom, 0.0f, 1.0f);
        const qreal peakTopY = centerY - peak * halfHeight;
        const qreal peakBottomY = centerY + peak * halfHeight;
        const qreal rmsTopY = centerY - rms * halfHeight;
        const qreal rmsBottomY = centerY + rms * halfHeight;

        rmsTop.append(QPointF(px, rmsTopY));
        rmsBottom.append(QPointF(px, rmsBottomY));

        if (x == 0) {
            peakTop.moveTo(px, peakTopY);
            peakBottom.moveTo(px, peakBottomY);
            rmsLineTop.moveTo(px, rmsTopY);
            rmsLineBottom.moveTo(px, rmsBottomY);
        } else {
            peakTop.lineTo(px, peakTopY);
            peakBottom.lineTo(px, peakBottomY);
            rmsLineTop.lineTo(px, rmsTopY);
            rmsLineBottom.lineTo(px, rmsBottomY);
        }
    }

    QPainterPath fillPath;
    fillPath.moveTo(rmsTop.first());
    for (int i = 1; i < rmsTop.size(); ++i)
        fillPath.lineTo(rmsTop[i]);
    for (int i = rmsBottom.size() - 1; i >= 0; --i)
        fillPath.lineTo(rmsBottom[i]);
    fillPath.closeSubpath();

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    QColor fill = wave;
    fill.setAlpha(65);
    painter.fillPath(fillPath, fill);

    QColor centerFill = kRmsColor;
    centerFill.setAlpha(55);
    painter.setPen(QPen(centerFill, 1.0));
    painter.drawLine(QPointF(plotRect.left(), centerY),
                     QPointF(plotRect.right(), centerY));

    painter.setPen(QPen(kRmsColor, 1.3));
    painter.drawPath(rmsLineTop);
    painter.drawPath(rmsLineBottom);

    QColor peak = kPeakColor;
    peak.setAlpha(210);
    painter.setPen(QPen(peak, 1.0));
    painter.drawPath(peakTop);
    painter.drawPath(peakBottom);

    painter.restore();

    if (clipCount > 0) {
        painter.setPen(QPen(kClipColor, 1.0));
        for (int x = 0; x < m_columns.size(); ++x) {
            if (m_columns[x].clipped <= 0)
                continue;
            const qreal px = left + x + 0.5;
            painter.drawLine(QPointF(px, plotRect.top()),
                             QPointF(px, plotRect.top() + 4.0));
            painter.drawLine(QPointF(px, plotRect.bottom() - 4.0),
                             QPointF(px, plotRect.bottom()));
        }
    }
}

void WaveformWidget::drawBars(QPainter& painter, const QRectF& plotRect)
{
    const int targetBars = std::clamp(static_cast<int>(plotRect.width() / 5.0), 12, 64);
    buildColumns(targetBars);
    if (m_columns.isEmpty())
        return;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QColor wave = waveformColor();
    const qreal slot = plotRect.width() / m_columns.size();
    const qreal barWidth = std::max<qreal>(2.0, slot - 1.5);
    const qreal bottom = plotRect.bottom();
    const qreal maxHeight = std::max<qreal>(1.0, plotRect.height() - 1.0);

    for (int i = 0; i < m_columns.size(); ++i) {
        const ColumnStats& c = m_columns[i];
        const qreal x = plotRect.left() + i * slot + (slot - barWidth) * 0.5;
        const QRectF rail(x, plotRect.top(), barWidth, maxHeight);
        painter.fillRect(rail, kBarEmpty);

        const qreal peak = std::clamp(c.peak * m_amplitudeZoom, 0.0f, 1.0f);
        const qreal rms = std::clamp(c.rms * m_amplitudeZoom, 0.0f, 1.0f);
        if (peak <= 0.0)
            continue;

        QColor fill = wave;
        if (c.clipped > 0 || peak >= 0.96)
            fill = kClipColor;
        else if (peak >= 0.78)
            fill = QColor(0xff, 0xd1, 0x66);
        else if (peak < 0.42)
            fill = kRmsColor;

        const qreal h = std::max<qreal>(1.0, peak * maxHeight);
        const QRectF bar(x, bottom - h, barWidth, h);
        QLinearGradient grad(bar.topLeft(), bar.bottomLeft());
        grad.setColorAt(0.0, fill.lighter(125));
        grad.setColorAt(1.0, fill.darker(150));
        painter.fillRect(bar, grad);

        const qreal rmsY = bottom - std::max<qreal>(1.0, rms * maxHeight);
        painter.setPen(QPen(kRmsColor.lighter(115), 1.0));
        painter.drawLine(QPointF(x, rmsY), QPointF(x + barWidth, rmsY));

        const qreal capY = std::max(plotRect.top(), bar.top() - 2.0);
        painter.setPen(QPen(c.clipped > 0 ? kClipColor : kBarPeakHold, 1.0));
        painter.drawLine(QPointF(x, capY), QPointF(x + barWidth, capY));
    }

    painter.restore();
}

void WaveformWidget::drawVerticalBars(QPainter& painter,
                                      const QRectF& plotRect,
                                      int sampleRate)
{
    if (m_displaySamples.isEmpty())
        return;

    const int bandCount = std::clamp(static_cast<int>(plotRect.width() / 12.0), 10, 18);
    const int analysisCount = std::min(
        static_cast<int>(m_displaySamples.size()),
        std::clamp(sampleRate / 25, 256, 1536));
    if (analysisCount < 32)
        return;

    const int start = m_displaySamples.size() - analysisCount;
    const double lowHz = 70.0;
    const double highHz = std::max(lowHz * 2.0, std::min(sampleRate * 0.45, 7000.0));
    const double logLow = std::log(lowHz);
    const double logHigh = std::log(highHz);
    double windowSum = 0.0;
    for (int i = 0; i < analysisCount; ++i)
        windowSum += 0.5 - 0.5 * std::cos(2.0 * kPi * i / std::max(1, analysisCount - 1));
    windowSum = std::max(windowSum, 1.0);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);

    const qreal slot = plotRect.width() / bandCount;
    const qreal barWidth = std::max<qreal>(4.0, slot - 3.0);
    const qreal bottom = plotRect.bottom();
    const qreal maxHeight = std::max<qreal>(1.0, plotRect.height() - 1.0);
    const QColor wave = waveformColor();

    for (int band = 0; band < bandCount; ++band) {
        const double t = bandCount == 1
            ? 0.0
            : static_cast<double>(band) / (bandCount - 1);
        const double frequency = std::exp(logLow + (logHigh - logLow) * t);
        const double omega = 2.0 * kPi * frequency / sampleRate;
        const double coeff = 2.0 * std::cos(omega);
        double q1 = 0.0;
        double q2 = 0.0;

        for (int i = 0; i < analysisCount; ++i) {
            const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * i / std::max(1, analysisCount - 1));
            const double sample = clampSample(m_displaySamples[start + i]) * window;
            const double q0 = sample + coeff * q1 - q2;
            q2 = q1;
            q1 = q0;
        }

        const double power = std::max(0.0, q1 * q1 + q2 * q2 - coeff * q1 * q2);
        const double amplitude = std::clamp((2.0 * std::sqrt(power) / windowSum) * m_amplitudeZoom, 0.0, 1.0);
        const double db = std::max(20.0 * std::log10(std::max(amplitude, 1e-9)), -60.0);
        const qreal level = std::clamp((db + 60.0) / 60.0, 0.0, 1.0);

        const qreal x = plotRect.left() + band * slot + (slot - barWidth) * 0.5;
        const QRectF rail(x, plotRect.top(), barWidth, maxHeight);
        painter.fillRect(rail, kBarEmpty);

        QColor fill = wave;
        if (amplitude >= 0.96)
            fill = kClipColor;
        else if (level >= 0.82)
            fill = QColor(0xff, 0xd1, 0x66);
        else if (level < 0.42)
            fill = kRmsColor;

        const qreal h = std::max<qreal>(1.0, level * maxHeight);
        const QRectF bar(x, bottom - h, barWidth, h);
        QLinearGradient grad(bar.topLeft(), bar.bottomLeft());
        grad.setColorAt(0.0, fill.lighter(125));
        grad.setColorAt(0.55, fill);
        grad.setColorAt(1.0, fill.darker(145));
        painter.fillRect(bar, grad);

        const qreal capY = std::max(plotRect.top(), bar.top() - 2.0);
        painter.setPen(QPen(amplitude >= 0.96 ? kClipColor : kBarPeakHold, 1.0));
        painter.drawLine(QPointF(x, capY), QPointF(x + barWidth, capY));
    }

    painter.restore();
}

void WaveformWidget::drawBarsGrid(QPainter& painter, const QRectF& plotRect) const
{
    painter.save();

    if (showGrid()) {
        painter.setPen(QPen(kGridMinor, 1.0));
        for (int i = 0; i <= 10; ++i) {
            const qreal x = plotRect.left() + plotRect.width() * i / 10.0;
            painter.drawLine(QPointF(x, plotRect.top()),
                             QPointF(x, plotRect.bottom()));
        }
    }

    const int refs[] = {0, -6, -12, -24, -48};

    QFont labelFont = font();
    labelFont.setPointSizeF(std::max(7.0, labelFont.pointSizeF() - 2.0));
    painter.setFont(labelFont);

    for (int db : refs) {
        const qreal rawHeight = dbToAmplitude(static_cast<float>(db)) * m_amplitudeZoom * plotRect.height();
        if (db <= -48 && rawHeight < 3.0)
            continue;
        const qreal y = plotRect.bottom() - std::min(rawHeight, plotRect.height());

        painter.setPen(QPen(db >= -12 ? kGridMajor : kGridMinor, 1.0));
        painter.drawLine(QPointF(plotRect.left(), y), QPointF(plotRect.right(), y));

        painter.setPen(kMutedLabel);
        painter.drawText(QRectF(plotRect.right() + 4.0, y - 7.0,
                                width() - plotRect.right() - 5.0, 14.0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(db));
    }

    painter.setPen(kMutedLabel);
    painter.drawText(QRectF(plotRect.right() + 4.0, plotRect.bottom() - 12.0,
                            width() - plotRect.right() - 5.0, 12.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("dBFS"));

    painter.restore();
}

void WaveformWidget::drawGrid(QPainter& painter,
                              const QRectF& plotRect,
                              int sampleRate) const
{
    Q_UNUSED(sampleRate);

    painter.save();

    if (showGrid()) {
        painter.setPen(QPen(kGridMinor, 1.0));
        for (int i = 0; i <= 10; ++i) {
            const qreal x = plotRect.left() + plotRect.width() * i / 10.0;
            painter.drawLine(QPointF(x, plotRect.top()),
                             QPointF(x, plotRect.bottom()));
        }
    }

    const qreal centerY = plotRect.center().y();
    const qreal halfHeight = std::max<qreal>(1.0, plotRect.height() * 0.5 - 2.0);
    const int refs[] = {0, -6, -12, -24, -48};

    QFont labelFont = font();
    labelFont.setPointSizeF(std::max(7.0, labelFont.pointSizeF() - 2.0));
    painter.setFont(labelFont);

    for (int db : refs) {
        const qreal rawOffset = dbToAmplitude(static_cast<float>(db)) * m_amplitudeZoom * halfHeight;
        if (db <= -48 && rawOffset < 3.0)
            continue;
        const qreal offset = std::min(rawOffset, halfHeight);

        painter.setPen(QPen(db >= -12 ? kGridMajor : kGridMinor, 1.0));
        const qreal yTop = centerY - offset;
        const qreal yBottom = centerY + offset;
        if (rawOffset <= halfHeight + 0.5) {
            painter.drawLine(QPointF(plotRect.left(), yTop), QPointF(plotRect.right(), yTop));
            painter.drawLine(QPointF(plotRect.left(), yBottom), QPointF(plotRect.right(), yBottom));
        } else {
            painter.drawLine(QPointF(plotRect.left(), plotRect.top()),
                             QPointF(plotRect.right(), plotRect.top()));
            painter.drawLine(QPointF(plotRect.left(), plotRect.bottom()),
                             QPointF(plotRect.right(), plotRect.bottom()));
        }

        painter.setPen(kMutedLabel);
        painter.drawText(QRectF(plotRect.right() + 4.0, yTop - 7.0,
                                width() - plotRect.right() - 5.0, 14.0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(db));
    }

    painter.setPen(QPen(kCenterLine, 1.0));
    painter.drawLine(QPointF(plotRect.left(), centerY),
                     QPointF(plotRect.right(), centerY));

    painter.setPen(kMutedLabel);
    painter.drawText(QRectF(plotRect.right() + 4.0, plotRect.bottom() - 12.0,
                            width() - plotRect.right() - 5.0, 12.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("dBFS"));

    painter.restore();
}

void WaveformWidget::drawNoAudio(QPainter& painter,
                                 const QRectF& plotRect,
                                 const QString& source) const
{
    painter.save();
    QFont f = font();
    f.setPointSizeF(std::max(8.0, f.pointSizeF() - 1.0));
    painter.setFont(f);
    painter.setPen(kMutedLabel);
    painter.drawText(plotRect, Qt::AlignCenter,
                     QStringLiteral("no %1 audio").arg(source));
    painter.restore();
}

void WaveformWidget::drawPausedBadge(QPainter& painter, const QRectF& footerRect) const
{
    painter.save();
    QFont f = font();
    f.setBold(true);
    f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1.0));
    painter.setFont(f);
    painter.setPen(QColor(0xff, 0xd1, 0x66));
    painter.drawText(footerRect, Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("PAUSED"));
    painter.restore();
}

void WaveformWidget::scheduleRepaint()
{
    if (!isVisible())
        return;

    if (!m_repaintThrottle.isValid()
        || m_repaintThrottle.elapsed() >= std::max(1, 1000 / m_refreshRateHz)) {
        update();
        m_repaintThrottle.restart();
    }
}

int WaveformWidget::sanitizeSampleRate(int sampleRate)
{
    if (sampleRate <= 0)
        sampleRate = kDefaultSampleRate;
    return std::clamp(sampleRate, kMinSampleRate, kMaxSampleRate);
}

int WaveformWidget::sanitizeWindowMs(int windowMs)
{
    return std::clamp(windowMs, kMinWindowMs, kMaxWindowMs);
}

int WaveformWidget::sanitizeRefreshRateHz(int hz)
{
    return std::clamp(hz, kMinRefreshRateHz, kMaxRefreshRateHz);
}

float WaveformWidget::sanitizeAmplitudeZoom(float zoom)
{
    if (!std::isfinite(zoom))
        zoom = 1.7f;
    return std::clamp(zoom, 1.0f, 6.0f);
}

float WaveformWidget::clampSample(float sample)
{
    if (!std::isfinite(sample))
        return 0.0f;
    return std::clamp(sample, -1.0f, 1.0f);
}

float WaveformWidget::dbToAmplitude(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

float WaveformWidget::linearToDb(float value)
{
    const float db = 20.0f * std::log10(std::max(value, 1e-9f));
    return std::max(db, -120.0f);
}

} // namespace AetherSDR
