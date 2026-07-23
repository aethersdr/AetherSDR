#include "WaveformWidget.h"

#include "InteractionSettings.h"
#include "NativeWidgetTopology.h"
#include "core/AppSettings.h"

#include <QApplication>
#include <QFile>
#include <QFontMetrics>
#include <QHashFunctions>
#include <QLinearGradient>
#include <QLineF>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPolygonF>
#include <QSizePolicy>

#include <algorithm>
#include <cmath>
#include "core/ThemeManager.h"

namespace AetherSDR {

// WAVE is QPainter-only for now, but reduction is already incremental
// (WaveformScopeModel): repaints merge pre-folded bins instead of rescanning
// the raw window, so paint cost no longer scales with the time window.
// A QRhi render path can follow the SpectrumWidget pattern by uploading the
// merged ColumnStats array as a 1-D texture; the model's generation()
// counter is the dirty flag for that upload (#3351-adjacent, Phase 2).
namespace {

constexpr int kDefaultSampleRate = 24000;
constexpr int kMinSampleRate = 8000;
constexpr int kMaxSampleRate = 192000;
constexpr int kNoAudioTimeoutMs = 1000;
constexpr int kMinWindowMs = 40;
// Per-profile window ceilings — the sidebar applet's Window slider tops out
// at 20 s; the strip fork historically allowed 30 s for CE-SSB envelope
// monitoring on voice TX.
constexpr int kAppletMaxWindowMs = 20000;
constexpr int kStripMaxWindowMs = 30000;
constexpr int kMinRefreshRateHz = 5;
// One shared repaint ceiling. The old sidebar cap of 30 Hz existed to keep
// QPainter cost down; with incremental reduction the applet can afford the
// strip's 120 Hz ceiling (the applet UI defaults to 60).
constexpr int kMaxRefreshRateHz = 120;
constexpr double kPi = 3.14159265358979323846;

inline QColor kBackground() { return AetherSDR::ThemeManager::instance().color("color.background.0"); }
inline QColor kGridMajor() { return AetherSDR::theme::withAlpha("color.background.2", 130); }
inline QColor kGridMinor() { return AetherSDR::theme::withAlpha("color.background.1", 150); }
inline QColor kCenterLine() { return AetherSDR::theme::withAlpha("color.text.label", 150); }
inline QColor kWaveFallback() { return AetherSDR::ThemeManager::instance().color("color.accent.bright"); }
inline QColor kPeakColor() { return AetherSDR::theme::withAlpha("color.accent", 180); }
inline QColor kRmsColor() { return AetherSDR::theme::withAlpha("color.accent.success", 210); }
inline QColor kLabelColor() { return AetherSDR::ThemeManager::instance().color("color.text.primary"); }
inline QColor kMutedLabel() { return AetherSDR::ThemeManager::instance().color("color.text.secondary"); }
inline QColor kClipColor() { return AetherSDR::ThemeManager::instance().color("color.accent.danger"); }
inline QColor kBarEmpty() { return AetherSDR::theme::withAlpha("color.background.1", 170); }
inline QColor kBarPeakHold() { return AetherSDR::ThemeManager::instance().color("color.accent.warning"); }
QColor waveformColor()
{
    QColor c(AppSettings::instance().value("DisplayFftFillColor", "#00e5ff").toString());
    return c.isValid() ? c : kWaveFallback();
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
    : WaveformWidget(Profile::Applet, parent)
{
}

WaveformWidget::WaveformWidget(Profile profile, QWidget* parent)
    : WAVEFORM_BASE_CLASS(parent)
    , m_maxWindowMs(profile == Profile::Strip ? kStripMaxWindowMs
                                              : kAppletMaxWindowMs)
    // The strip fork drew with antialiased strokes (visibly smoother at its
    // higher refresh / longer windows); the sidebar applet keeps the crisp
    // unantialiased trace. Preserved per-profile through the merge.
    , m_antialiasedStroke(profile == Profile::Strip)
{
    setMinimumSize(minimumSizeHint());
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
#if defined(AETHER_GPU_SPECTRUM) && defined(Q_OS_MAC)
    // Unlike the panadapter, every waveform scope lives inside a QScrollArea.
    // Making that child a native NSView while deliberately keeping its
    // ancestors non-native disconnects the native surface from QWidget
    // geometry: it stays behind when the scroll area moves or clips it. Keep
    // the waveform composited into its ancestor backing store so resize,
    // scrolling, and clipping remain QWidget-owned.
    setApi(QRhiWidget::Api::Metal);
#endif
    setToolTip("Click to pause/resume waveform capture; double-click for WAVE settings");
    setObjectName(profile == Profile::Strip ? QStringLiteral("stripWaveformScope")
                                            : QStringLiteral("waveAppletScope"));
    setAccessibleName(profile == Profile::Strip
                          ? QStringLiteral("Strip waveform scope")
                          : QStringLiteral("WAVE waveform scope"));

    m_perfSince.start();

    // Size each model's raw ring to this profile's window ceiling so widening
    // the window (live or paused) reveals captured history, not a blank plot
    // that refills over the next N seconds (#3955 regression vs the old
    // StripWaveform buffer). m_pausedModel inherits the ceiling on copy but is
    // set here too for the pause-before-first-append case.
    m_rx.setMaxWindowMs(m_maxWindowMs);
    m_tx.setMaxWindowMs(m_maxWindowMs);
    m_pausedModel.setMaxWindowMs(m_maxWindowMs);

    m_clickTimer.setSingleShot(true);
    // Interval read at click time from clickDiscriminationIntervalMs() so
    // a user-adjusted Radio Setup value propagates without an app restart.
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
    WaveformScopeModel& model = tx ? m_tx : m_rx;
    model.configure(sanitizeSampleRate(sampleRate), m_windowMs);
    model.append(src, samples);

    ++m_perfAppendCount;
    m_perfAppendedSamples += static_cast<quint64>(samples);

    scheduleRepaint();
}

void WaveformWidget::invalidateRenderCaches()
{
    m_bandCacheGen = ~0ull;
#ifdef AETHER_GPU_SPECTRUM
    m_lastColUploadGen = ~0ull;
#endif
}

void WaveformWidget::setTransmitting(bool tx)
{
    if (m_transmitting == tx)
        return;
    m_transmitting = tx;
    // displayModel() now returns a different model instance whose per-model
    // generation() is unrelated to the last-uploaded one — force a refresh.
    invalidateRenderCaches();
    update();
}

void WaveformWidget::clear()
{
    m_rx.clear();
    m_tx.clear();
    update();
}

void WaveformWidget::setViewMode(ViewMode mode)
{
    if (m_viewMode == mode)
        return;
    m_viewMode = mode;
    // Column-texture channel packing differs by mode (Bands packs
    // [level,amplitude,0,0] vs Scope [min,max,rms,peak]); a mode switch with
    // unchanged columnCount + frozen generation must still re-upload.
    invalidateRenderCaches();
    update();
}

void WaveformWidget::setZoomWindowMs(int windowMs)
{
    const int sanitized = sanitizeWindowMs(windowMs);
    if (m_windowMs == sanitized)
        return;

    m_windowMs = sanitized;
    m_rx.configure(m_rx.sampleRate(), sanitized);
    m_tx.configure(m_tx.sampleRate(), sanitized);
    if (m_paused) {
        // Re-window the frozen audio — the paused raw ring holds the old
        // window plus headroom, so shrinking (or modestly growing) the
        // window re-bins from the snapshot rather than pulling live data.
        m_pausedModel.configure(m_pausedModel.sampleRate(), sanitized);
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

WaveformWidget::PerfStats WaveformWidget::perfStats() const
{
    PerfStats s;
    s.paintCount = m_perfPaintCount;
    s.paintUsTotal = m_perfPaintUsTotal;
    s.paintUsMax = m_perfPaintUsMax;
    s.appendCount = m_perfAppendCount;
    s.appendedSamples = m_perfAppendedSamples;
    s.sinceMs = m_perfSince.isValid() ? m_perfSince.elapsed() : 0;
    return s;
}

void WaveformWidget::resetPerfStats()
{
    m_perfPaintCount = 0;
    m_perfPaintUsTotal = 0;
    m_perfPaintUsMax = 0;
    m_perfAppendCount = 0;
    m_perfAppendedSamples = 0;
    m_perfSince.restart();
}

int WaveformWidget::activeSampleRate() const
{
    return displayModel().sampleRate();
}

QVariantMap WaveformWidget::wavestatsSnapshot(bool reset)
{
    const PerfStats s = perfStats();
    const double secs = std::max(0.001, s.sinceMs / 1000.0);

    QVariantMap m;
    m[QStringLiteral("name")] = objectName();
    m[QStringLiteral("windowTitle")] = window() ? window()->windowTitle() : QString();
    // Which top-level surface hosts this scope: the main window (docked),
    // a FloatingContainerWindow (popped-out applet), or the strip.
    m[QStringLiteral("windowClass")] = window()
        ? QString::fromLatin1(window()->metaObject()->className())
        : QString();
    m[QStringLiteral("floating")] =
        window() && !window()->inherits("QMainWindow");
    m[QStringLiteral("visible")] = isVisible();
    m[QStringLiteral("tx")] = m_transmitting;
    m[QStringLiteral("paused")] = m_paused;
    switch (m_viewMode) {
    case ViewMode::Graph:        m[QStringLiteral("mode")] = QStringLiteral("Scope"); break;
    case ViewMode::Envelope:     m[QStringLiteral("mode")] = QStringLiteral("Envelope"); break;
    case ViewMode::Bars:         m[QStringLiteral("mode")] = QStringLiteral("History"); break;
    case ViewMode::VerticalBars: m[QStringLiteral("mode")] = QStringLiteral("Bands"); break;
    }
    m[QStringLiteral("fps")] = m_refreshRateHz;
    m[QStringLiteral("windowMs")] = m_windowMs;
    m[QStringLiteral("sampleRate")] = activeSampleRate();
    m[QStringLiteral("widthPx")] = width();
    m[QStringLiteral("heightPx")] = height();
    appendNativeWidgetTopology(m, *this);
    m[QStringLiteral("sinceMs")] = static_cast<qlonglong>(s.sinceMs);
    m[QStringLiteral("paintCount")] = static_cast<qulonglong>(s.paintCount);
    m[QStringLiteral("paintsPerSec")] = s.paintCount / secs;
    m[QStringLiteral("avgPaintUs")] = s.paintCount
        ? static_cast<double>(s.paintUsTotal) / s.paintCount : 0.0;
    m[QStringLiteral("maxPaintUs")] = static_cast<qulonglong>(s.paintUsMax);
    // Main-thread paint budget actually consumed, in "ms per wall second" —
    // the single number to compare before/after a rendering change.
    m[QStringLiteral("paintMsPerSec")] = (s.paintUsTotal / 1000.0) / secs;
    m[QStringLiteral("appendsPerSec")] = s.appendCount / secs;
    m[QStringLiteral("samplesPerSec")] = s.appendedSamples / secs;

    if (reset)
        resetPerfStats();
    return m;
}

void WaveformWidget::prepareForTopLevelChange()
{
#ifdef AETHER_GPU_SPECTRUM
    // Same #2495 guard as SpectrumWidget::prepareForTopLevelChange(): fire
    // WindowAboutToChangeInternal exactly once BEFORE the reparent so
    // QRhiWidgetPrivate deregisters its cleanup callback from the old QRhi.
    QEvent event(QEvent::WindowAboutToChangeInternal);
    QCoreApplication::sendEvent(this, &event);
#endif
}

#ifndef AETHER_GPU_SPECTRUM
// TODO(a11y): QAccessibleInterface needed — this scope custom-paints
// data-bearing content (min/max envelope, RMS band, peak, clip flags, and the
// peak/RMS/clip header readout) in both this CPU path and the GPU render()
// path. A screen reader sees only the accessibleName, not the live values.
// Expose via a WaveformWidget QAccessibleInterface. Tracked in
// aethersdr/AetherSDR#3959.
void WaveformWidget::paintEvent(QPaintEvent* event)
{
    QElapsedTimer paintTimer;
    paintTimer.start();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, m_antialiasedStroke);
    // fillRect is clipped to the update region, so a plot-only data
    // repaint doesn't re-fill (or re-composite) the header/footer bands.
    painter.fillRect(rect(), kBackground());

    const QRectF plotRect = plotArea();
    // Data-driven repaints (scheduleRepaint) invalidate only the plot
    // area; the header readout and footer are re-shaped and re-drawn only
    // when the update region reaches them (~5 Hz, or any full update()).
    const QRect updateRect = event->rect();
    const bool drawHeader = updateRect.top() < static_cast<int>(plotRect.top());
    const bool drawFooter =
        updateRect.bottom() > static_cast<int>(plotRect.bottom());

    const WaveformScopeModel& model = displayModel();
    const QString source = (m_paused ? m_pausedTransmitting : m_transmitting)
        ? QStringLiteral("TX")
        : QStringLiteral("RX");
    const int sampleRate = model.sampleRate();

    // Merge bins → pixel columns once, per the mode's column count; the
    // whole-window readout stats come from the same walk.
    WaveformScopeModel::WindowStats stats;
    if (m_viewMode == ViewMode::VerticalBars) {
        stats = model.windowStats();
        m_columns.clear();
    } else {
        int columnCount = 0;
        if (m_viewMode == ViewMode::Bars)
            columnCount = std::clamp(static_cast<int>(plotRect.width() / 5.0), 12, 64);
        else
            columnCount = std::max(1, static_cast<int>(std::floor(plotRect.width())));
        stats = model.mergeColumns(columnCount, m_columns);
    }

    const float peakDb = linearToDb(stats.peak);
    const float rmsDb = linearToDb(stats.rms);
    const int clipCount = stats.clipCount;

    const int gridKind =
        (m_viewMode == ViewMode::Bars || m_viewMode == ViewMode::VerticalBars)
            ? 1 : 0;
    ensureGridCache(gridKind, plotRect);
    painter.drawImage(QPointF(0, 0), m_gridCache);

    if (m_viewMode == ViewMode::VerticalBars) {
        if (!stats.empty)
            drawVerticalBars(painter, plotRect, sampleRate);
    } else if (m_viewMode == ViewMode::Envelope) {
        if (!stats.empty)
            drawEnvelope(painter, plotRect, clipCount);
    } else if (m_viewMode == ViewMode::Bars) {
        if (!stats.empty)
            drawBars(painter, plotRect);
    } else {
        if (!stats.empty)
            drawGraph(painter, plotRect, clipCount);
    }

    if (drawHeader) {
        QFont labelFont = font();
        labelFont.setPointSizeF(std::max(7.0, labelFont.pointSizeF() - 1.0));
        painter.setFont(labelFont);
        painter.setPen(kLabelColor());
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
            painter.setPen(kClipColor());
            painter.drawText(QRectF(7.0, 3.0, width() - 14.0, 16.0),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("CLIP %1").arg(clipCount));
        }
    }

    const QRectF footerRect(plotRect.left(), plotRect.bottom() + 2.0,
                            plotRect.width(), 15.0);
    if (drawFooter) {
        QFont labelFont = font();
        labelFont.setPointSizeF(std::max(7.0, labelFont.pointSizeF() - 1.0));
        painter.setFont(labelFont);
        painter.setPen(kMutedLabel());
        const QString timeText = m_viewMode == ViewMode::VerticalBars
            ? QString::fromUtf8("%1 \xc2\xb7 %2 ms \xc2\xb7 frequency bands")
                .arg(formatSampleRate(sampleRate))
                .arg(m_windowMs)
            : QString::fromUtf8("%1 \xc2\xb7 %2 ms \xc2\xb7 %3 ms/div")
                .arg(formatSampleRate(sampleRate))
                .arg(m_windowMs)
                .arg(std::max(1, m_windowMs / 10));
        painter.drawText(m_paused ? footerRect.adjusted(0.0, 0.0, -52.0, 0.0) : footerRect,
                         Qt::AlignLeft | Qt::AlignVCenter,
                         timeText);
    }

    const qint64 sinceAppend = activeModel().msSinceLastAppend();
    const bool stale = !m_paused
        && (sinceAppend < 0 || sinceAppend > kNoAudioTimeoutMs);
    if (stats.empty || stale)
        drawNoAudio(painter, plotRect, source);

    if (m_paused && drawFooter)
        drawPausedBadge(painter, footerRect);

    ++m_perfPaintCount;
    const quint64 us = static_cast<quint64>(paintTimer.nsecsElapsed() / 1000);
    m_perfPaintUsTotal += us;
    m_perfPaintUsMax = std::max(m_perfPaintUsMax, us);
}
#endif // !AETHER_GPU_SPECTRUM

void WaveformWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_ignoreNextRelease) {
            m_ignoreNextRelease = false;
        } else if (!m_clickTimer.isActive()) {
            m_clickTimer.start(clickDiscriminationIntervalMs());
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

WaveformScopeModel& WaveformWidget::activeModel()
{
    return m_transmitting ? m_tx : m_rx;
}

const WaveformScopeModel& WaveformWidget::activeModel() const
{
    return m_transmitting ? m_tx : m_rx;
}

const WaveformScopeModel& WaveformWidget::displayModel() const
{
    return m_paused ? m_pausedModel : activeModel();
}

QRectF WaveformWidget::plotArea() const
{
    QRectF plotRect = QRectF(rect()).adjusted(5.0, 18.0, -34.0, -17.0);
    if (plotRect.width() < 24.0 || plotRect.height() < 36.0)
        plotRect = QRectF(rect()).adjusted(4.0, 17.0, -30.0, -16.0);
    return plotRect;
}

void WaveformWidget::ensureGridCache(int kind, const QRectF& plotRect)
{
    const qreal dpr = devicePixelRatioF();
    const bool grid = showGrid();
    // Grid colors come from the theme; hash the five it uses into one key so
    // a theme switch invalidates the cache. qHashMulti (not an overlapping-
    // shift XOR, which loses bits and lets distinct color sets collide to the
    // same key, leaving the old theme's grid on screen) mixes all 32 bits of
    // each rgba().
    const quint64 themeKey = qHashMulti(0,
        kGridMinor().rgba(), kGridMajor().rgba(), kCenterLine().rgba(),
        kBackground().rgba(), kMutedLabel().rgba());
    const QString fontKey = font().key();

    if (m_gridCacheKind == kind && m_gridCacheSize == size()
        && qFuzzyCompare(m_gridCacheDpr, dpr)
        && qFuzzyCompare(m_gridCacheZoom, m_amplitudeZoom)
        && m_gridCacheShowGrid == grid
        && m_gridCacheThemeKey == themeKey
        && m_gridCacheFontKey == fontKey
        && !m_gridCache.isNull())
        return;

    m_gridCache = QImage(size() * dpr, QImage::Format_ARGB32_Premultiplied);
    m_gridCache.setDevicePixelRatio(dpr);
    // Opaque background baked in: the GPU build draws this image as the
    // base layer of the frame; the CPU build composites it identically over
    // its own fill.
    m_gridCache.fill(kBackground());
    {
        QPainter p(&m_gridCache);
        p.setRenderHint(QPainter::Antialiasing, m_antialiasedStroke);
        p.setFont(font());
        if (kind == 1)
            drawBarsGrid(p, plotRect);
        else
            drawGrid(p, plotRect, 0);
    }
    m_gridCacheDirty = true;
    m_gridCacheKind = kind;
    m_gridCacheSize = size();
    m_gridCacheDpr = dpr;
    m_gridCacheZoom = m_amplitudeZoom;
    m_gridCacheShowGrid = grid;
    m_gridCacheThemeKey = themeKey;
    m_gridCacheFontKey = fontKey;
}

void WaveformWidget::setPaused(bool paused)
{
    if (m_paused == paused)
        return;

    if (paused) {
        m_pausedTransmitting = m_transmitting;
        m_pausedModel = activeModel();
    } else {
        // Drop the snapshot — it carries a full copy of the raw ring.
        m_pausedModel = WaveformScopeModel();
    }

    m_paused = paused;
    // Pausing/resuming swaps displayModel() between the live model and the
    // frozen snapshot; force a refresh so the swap isn't skipped by a
    // coincidental generation() match.
    invalidateRenderCaches();
    update();
}

#ifndef AETHER_GPU_SPECTRUM
void WaveformWidget::drawGraph(QPainter& painter,
                               const QRectF& plotRect,
                               int clipCount)
{
    if (m_columns.isEmpty())
        return;

    const qreal centerY = plotRect.center().y();
    const qreal halfHeight = std::max<qreal>(1.0, plotRect.height() * 0.5 - 2.0);
    const qreal left = plotRect.left();
    const QColor wave = waveformColor();

    // One geometry pass, then batched draw calls: a single drawLines()
    // for the min/max columns and drawPolyline() for the envelopes,
    // instead of ~columnCount drawLine() calls plus four QPainterPaths
    // rebuilt per frame.
    m_lineScratch.clear();
    m_lineScratch.reserve(m_columns.size());
    m_peakTopPts.resize(m_columns.size());
    m_peakBottomPts.resize(m_columns.size());
    m_rmsTopPts.resize(m_columns.size());
    m_rmsBottomPts.resize(m_columns.size());

    for (int x = 0; x < m_columns.size(); ++x) {
        const WaveformScopeModel::ColumnStats& c = m_columns[x];
        const qreal px = left + x + 0.5;
        const qreal minY = centerY - std::clamp(c.min * m_amplitudeZoom, -1.0f, 1.0f) * halfHeight;
        const qreal maxY = centerY - std::clamp(c.max * m_amplitudeZoom, -1.0f, 1.0f) * halfHeight;
        m_lineScratch.append(QLineF(px, minY, px, maxY));

        const qreal peak = std::clamp(c.peak * m_amplitudeZoom, 0.0f, 1.0f);
        const qreal rms = std::clamp(c.rms * m_amplitudeZoom, 0.0f, 1.0f);
        m_peakTopPts[x] = QPointF(px, centerY - peak * halfHeight);
        m_peakBottomPts[x] = QPointF(px, centerY + peak * halfHeight);
        m_rmsTopPts[x] = QPointF(px, centerY - rms * halfHeight);
        m_rmsBottomPts[x] = QPointF(px, centerY + rms * halfHeight);
    }

    painter.setPen(QPen(wave, waveformLineWidth(), Qt::SolidLine, Qt::RoundCap));
    painter.drawLines(m_lineScratch.constData(), m_lineScratch.size());

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(kPeakColor(), 1.0));
    painter.drawPolyline(m_peakTopPts.constData(), m_peakTopPts.size());
    painter.drawPolyline(m_peakBottomPts.constData(), m_peakBottomPts.size());
    painter.setPen(QPen(kRmsColor(), 1.4));
    painter.drawPolyline(m_rmsTopPts.constData(), m_rmsTopPts.size());
    painter.drawPolyline(m_rmsBottomPts.constData(), m_rmsBottomPts.size());
    painter.setRenderHint(QPainter::Antialiasing, false);

    if (clipCount > 0) {
        m_lineScratch.clear();
        for (int x = 0; x < m_columns.size(); ++x) {
            if (m_columns[x].clipped <= 0)
                continue;
            const qreal px = left + x + 0.5;
            m_lineScratch.append(QLineF(px, plotRect.top(),
                                        px, plotRect.top() + 4.0));
            m_lineScratch.append(QLineF(px, plotRect.bottom() - 4.0,
                                        px, plotRect.bottom()));
        }
        painter.setPen(QPen(kClipColor(), 1.0));
        painter.drawLines(m_lineScratch.constData(), m_lineScratch.size());
    }
}

void WaveformWidget::drawEnvelope(QPainter& painter,
                                  const QRectF& plotRect,
                                  int clipCount)
{
    if (m_columns.isEmpty())
        return;

    const qreal centerY = plotRect.center().y();
    const qreal halfHeight = std::max<qreal>(1.0, plotRect.height() * 0.5 - 2.0);
    const qreal left = plotRect.left();
    const QColor wave = waveformColor();

    m_peakTopPts.resize(m_columns.size());
    m_peakBottomPts.resize(m_columns.size());
    m_rmsTopPts.resize(m_columns.size());
    m_rmsBottomPts.resize(m_columns.size());

    for (int x = 0; x < m_columns.size(); ++x) {
        const WaveformScopeModel::ColumnStats& c = m_columns[x];
        const qreal px = left + x + 0.5;
        const qreal peak = std::clamp(c.peak * m_amplitudeZoom, 0.0f, 1.0f);
        const qreal rms = std::clamp(c.rms * m_amplitudeZoom, 0.0f, 1.0f);
        m_peakTopPts[x] = QPointF(px, centerY - peak * halfHeight);
        m_peakBottomPts[x] = QPointF(px, centerY + peak * halfHeight);
        m_rmsTopPts[x] = QPointF(px, centerY - rms * halfHeight);
        m_rmsBottomPts[x] = QPointF(px, centerY + rms * halfHeight);
    }

    // RMS ribbon polygon: top edge forward, bottom edge reversed.
    QPolygonF fillPoly;
    fillPoly.reserve(m_rmsTopPts.size() * 2);
    for (const QPointF& p : m_rmsTopPts)
        fillPoly.append(p);
    for (int i = m_rmsBottomPts.size() - 1; i >= 0; --i)
        fillPoly.append(m_rmsBottomPts[i]);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    QColor fill = wave;
    fill.setAlpha(65);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawPolygon(fillPoly);
    painter.setBrush(Qt::NoBrush);

    QColor centerFill = kRmsColor();
    centerFill.setAlpha(55);
    painter.setPen(QPen(centerFill, 1.0));
    painter.drawLine(QPointF(plotRect.left(), centerY),
                     QPointF(plotRect.right(), centerY));

    painter.setPen(QPen(kRmsColor(), 1.3));
    painter.drawPolyline(m_rmsTopPts.constData(), m_rmsTopPts.size());
    painter.drawPolyline(m_rmsBottomPts.constData(), m_rmsBottomPts.size());

    QColor peak = kPeakColor();
    peak.setAlpha(210);
    painter.setPen(QPen(peak, 1.0));
    painter.drawPolyline(m_peakTopPts.constData(), m_peakTopPts.size());
    painter.drawPolyline(m_peakBottomPts.constData(), m_peakBottomPts.size());

    painter.restore();

    if (clipCount > 0) {
        m_lineScratch.clear();
        for (int x = 0; x < m_columns.size(); ++x) {
            if (m_columns[x].clipped <= 0)
                continue;
            const qreal px = left + x + 0.5;
            m_lineScratch.append(QLineF(px, plotRect.top(),
                                        px, plotRect.top() + 4.0));
            m_lineScratch.append(QLineF(px, plotRect.bottom() - 4.0,
                                        px, plotRect.bottom()));
        }
        painter.setPen(QPen(kClipColor(), 1.0));
        painter.drawLines(m_lineScratch.constData(), m_lineScratch.size());
    }
}

void WaveformWidget::drawBars(QPainter& painter, const QRectF& plotRect)
{
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
        const WaveformScopeModel::ColumnStats& c = m_columns[i];
        const qreal x = plotRect.left() + i * slot + (slot - barWidth) * 0.5;
        const QRectF rail(x, plotRect.top(), barWidth, maxHeight);
        painter.fillRect(rail, kBarEmpty());

        const qreal peak = std::clamp(c.peak * m_amplitudeZoom, 0.0f, 1.0f);
        const qreal rms = std::clamp(c.rms * m_amplitudeZoom, 0.0f, 1.0f);
        if (peak <= 0.0)
            continue;

        QColor fill = wave;
        if (c.clipped > 0 || peak >= 0.96)
            fill = kClipColor();
        else if (peak >= 0.78)
            fill = AetherSDR::ThemeManager::instance().color("color.accent.warning");
        else if (peak < 0.42)
            fill = kRmsColor();

        const qreal h = std::max<qreal>(1.0, peak * maxHeight);
        const QRectF bar(x, bottom - h, barWidth, h);
        QLinearGradient grad(bar.topLeft(), bar.bottomLeft());
        grad.setColorAt(0.0, fill.lighter(125));
        grad.setColorAt(1.0, fill.darker(150));
        painter.fillRect(bar, grad);

        const qreal rmsY = bottom - std::max<qreal>(1.0, rms * maxHeight);
        painter.setPen(QPen(kRmsColor().lighter(115), 1.0));
        painter.drawLine(QPointF(x, rmsY), QPointF(x + barWidth, rmsY));

        const qreal capY = std::max(plotRect.top(), bar.top() - 2.0);
        painter.setPen(QPen(c.clipped > 0 ? kClipColor() : kBarPeakHold(), 1.0));
        painter.drawLine(QPointF(x, capY), QPointF(x + barWidth, capY));
    }

    painter.restore();
}

#endif // !AETHER_GPU_SPECTRUM

void WaveformWidget::computeBandLevels(const WaveformScopeModel& model,
                                       int bandCount, QVector<float>& levels)
{
    // Cache on the model generation with a small floor interval so a 60 fps
    // render loop doesn't re-run the Goertzel bank per frame.
    if (m_bandCacheCount == bandCount && m_bandCacheGen == model.generation()
        && m_bandCacheAge.isValid() && m_bandCacheAge.elapsed() < 40) {
        levels = m_bandLevels;
        return;
    }

    levels.clear();
    const int sampleRate = model.sampleRate();
    const int wantSamples = std::clamp(sampleRate / 25, 256, 1536);
    model.copyTail(wantSamples, m_tailScratch);
    const int analysisCount = m_tailScratch.size();
    if (analysisCount < 32)
        return;

    const double lowHz = 70.0;
    const double highHz = std::max(lowHz * 2.0, std::min(sampleRate * 0.45, 7000.0));
    const double logLow = std::log(lowHz);
    const double logHigh = std::log(highHz);
    double windowSum = 0.0;
    for (int i = 0; i < analysisCount; ++i)
        windowSum += 0.5 - 0.5 * std::cos(2.0 * kPi * i / std::max(1, analysisCount - 1));
    windowSum = std::max(windowSum, 1.0);

    levels.resize(bandCount * 2);   // [level, amplitude] per band
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
            const double sample = m_tailScratch[i] * window;
            const double q0 = sample + coeff * q1 - q2;
            q2 = q1;
            q1 = q0;
        }
        const double power = std::max(0.0, q1 * q1 + q2 * q2 - coeff * q1 * q2);
        const double amplitude = std::clamp((2.0 * std::sqrt(power) / windowSum) * m_amplitudeZoom, 0.0, 1.0);
        const double db = std::max(20.0 * std::log10(std::max(amplitude, 1e-9)), -60.0);
        levels[band * 2] = static_cast<float>(std::clamp((db + 60.0) / 60.0, 0.0, 1.0));
        levels[band * 2 + 1] = static_cast<float>(amplitude);
    }

    m_bandLevels = levels;
    m_bandCacheGen = model.generation();
    m_bandCacheCount = bandCount;
    m_bandCacheAge.restart();
}

#ifndef AETHER_GPU_SPECTRUM
void WaveformWidget::drawVerticalBars(QPainter& painter,
                                      const QRectF& plotRect,
                                      int sampleRate)
{
    Q_UNUSED(sampleRate);
    const int bandCount = std::clamp(static_cast<int>(plotRect.width() / 12.0), 10, 18);
    QVector<float> bands;
    computeBandLevels(displayModel(), bandCount, bands);
    if (bands.size() < bandCount * 2)
        return;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);

    const qreal slot = plotRect.width() / bandCount;
    const qreal barWidth = std::max<qreal>(4.0, slot - 3.0);
    const qreal bottom = plotRect.bottom();
    const qreal maxHeight = std::max<qreal>(1.0, plotRect.height() - 1.0);
    const QColor wave = waveformColor();

    for (int band = 0; band < bandCount; ++band) {
        const qreal level = bands[band * 2];
        const qreal amplitude = bands[band * 2 + 1];
        const qreal x = plotRect.left() + band * slot + (slot - barWidth) * 0.5;
        const QRectF rail(x, plotRect.top(), barWidth, maxHeight);
        painter.fillRect(rail, kBarEmpty());

        QColor fill = wave;
        if (amplitude >= 0.96)
            fill = kClipColor();
        else if (level >= 0.82)
            fill = AetherSDR::ThemeManager::instance().color("color.accent.warning");
        else if (level < 0.42)
            fill = kRmsColor();

        const qreal h = std::max<qreal>(1.0, level * maxHeight);
        const QRectF bar(x, bottom - h, barWidth, h);
        QLinearGradient grad(bar.topLeft(), bar.bottomLeft());
        grad.setColorAt(0.0, fill.lighter(125));
        grad.setColorAt(0.55, fill);
        grad.setColorAt(1.0, fill.darker(145));
        painter.fillRect(bar, grad);

        const qreal capY = std::max(plotRect.top(), bar.top() - 2.0);
        painter.setPen(QPen(amplitude >= 0.96 ? kClipColor() : kBarPeakHold(), 1.0));
        painter.drawLine(QPointF(x, capY), QPointF(x + barWidth, capY));
    }

    painter.restore();
}
#endif // !AETHER_GPU_SPECTRUM

void WaveformWidget::drawBarsGrid(QPainter& painter, const QRectF& plotRect) const
{
    painter.save();

    if (showGrid()) {
        painter.setPen(QPen(kGridMinor(), 1.0));
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

        painter.setPen(QPen(db >= -12 ? kGridMajor() : kGridMinor(), 1.0));
        painter.drawLine(QPointF(plotRect.left(), y), QPointF(plotRect.right(), y));

        painter.setPen(kMutedLabel());
        painter.drawText(QRectF(plotRect.right() + 4.0, y - 7.0,
                                width() - plotRect.right() - 5.0, 14.0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(db));
    }

    painter.setPen(kMutedLabel());
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
        painter.setPen(QPen(kGridMinor(), 1.0));
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

        painter.setPen(QPen(db >= -12 ? kGridMajor() : kGridMinor(), 1.0));
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

        painter.setPen(kMutedLabel());
        painter.drawText(QRectF(plotRect.right() + 4.0, yTop - 7.0,
                                width() - plotRect.right() - 5.0, 14.0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(db));
    }

    painter.setPen(QPen(kCenterLine(), 1.0));
    painter.drawLine(QPointF(plotRect.left(), centerY),
                     QPointF(plotRect.right(), centerY));

    painter.setPen(kMutedLabel());
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
    painter.setPen(kMutedLabel());
    const QString message = source == QStringLiteral("RX")
        ? QStringLiteral("Enable PC Audio")
        : QStringLiteral("no %1 audio").arg(source);
    painter.drawText(plotRect, Qt::AlignCenter, message);
    painter.restore();
}

void WaveformWidget::drawPausedBadge(QPainter& painter, const QRectF& footerRect) const
{
    painter.save();
    QFont f = font();
    f.setBold(true);
    f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1.0));
    painter.setFont(f);
    painter.setPen(AetherSDR::ThemeManager::instance().color("color.accent.warning"));
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
#ifdef AETHER_GPU_SPECTRUM
        // A QRhiWidget renders whole frames — partial update regions are
        // meaningless; the overlay's own dirty keys throttle text work.
        update();
#else
        // Data repaints invalidate only the plot area — the header/footer
        // text doesn't need re-shaping per frame. Roughly every fifth of a
        // second a full update refreshes the RMS/PK readout (which also
        // reads steadier than a per-frame flicker of digits). Any state
        // change (mode, zoom, pause, TX pin) still calls update() directly.
        const int headerEvery = std::max(1, m_refreshRateHz / 5);
        if (++m_headerTick >= headerEvery) {
            m_headerTick = 0;
            update();
        } else {
            update(plotArea().toAlignedRect());
        }
#endif
        m_repaintThrottle.restart();
    }
}

int WaveformWidget::sanitizeSampleRate(int sampleRate)
{
    if (sampleRate <= 0)
        sampleRate = kDefaultSampleRate;
    return std::clamp(sampleRate, kMinSampleRate, kMaxSampleRate);
}

int WaveformWidget::sanitizeWindowMs(int windowMs) const
{
    return std::clamp(windowMs, kMinWindowMs, m_maxWindowMs);
}

int WaveformWidget::sanitizeRefreshRateHz(int hz) const
{
    return std::clamp(hz, kMinRefreshRateHz, kMaxRefreshRateHz);
}

float WaveformWidget::sanitizeAmplitudeZoom(float zoom)
{
    if (!std::isfinite(zoom))
        zoom = 1.7f;
    return std::clamp(zoom, 1.0f, 6.0f);
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

#ifdef AETHER_GPU_SPECTRUM

// ── QRhi render path ──────────────────────────────────────────────────────
// Layering matches the QPainter path exactly: grid/background image (opaque,
// includes the dB labels) → waveform fragment shader (transparent except the
// trace) → text overlay (readout/footer/badges, refreshed only when its
// content changes). Per-frame uploads are a ≤4 KB column texture and a
// ~200-byte UBO; the text/grid images upload only when dirty.

namespace {

// Fullscreen quad: position (x,y) + texcoord (u,v) — same layout as
// SpectrumWidget's kQuadData so overlay.vert applies unchanged.
const float kWaveQuadData[] = {
    -1, -1,  0, 1,
     1, -1,  1, 1,
    -1,  1,  0, 0,
     1,  1,  1, 0,
};

QShader loadWaveShader(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "WaveformWidget: failed to load shader" << path;
        return {};
    }
    QShader s = QShader::fromSerialized(f.readAll());
    if (!s.isValid())
        qWarning() << "WaveformWidget: invalid shader" << path;
    return s;
}

void fillColor(float* dst, const QColor& c)
{
    dst[0] = static_cast<float>(c.redF());
    dst[1] = static_cast<float>(c.greenF());
    dst[2] = static_cast<float>(c.blueF());
    dst[3] = static_cast<float>(c.alphaF());
}

} // namespace

// std140 mirror of the U block in wavescope.frag.
struct WaveformWidget::WaveUniforms {
    float plot[4];
    float widget[4];
    float params[4];
    float colWave[4];
    float colPeak[4];
    float colRms[4];
    float colClip[4];
    float colBarEmpty[4];
    float colWarn[4];
    float colRmsLight[4];
    float colCap[4];
    float colCenter[4];
};

void WaveformWidget::initWavePipeline()
{
    QRhi* r = rhi();

    m_waveVbo = r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                             sizeof(kWaveQuadData));
    m_waveVbo->create();

    m_waveUbo = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                             sizeof(WaveUniforms));
    m_waveUbo->create();

    m_colTexW = 16;
    m_colTex = r->newTexture(QRhiTexture::RGBA32F, QSize(m_colTexW, 1));
    m_colTex->create();
    m_clipTex = r->newTexture(QRhiTexture::R8, QSize(m_colTexW, 1));
    m_clipTex->create();

    // Linear on the columns so Scope/Envelope curves interpolate between
    // column points the way the polyline stroke did; nearest on the clip
    // flags so a tick never bleeds into its neighbour.
    m_colSampler = r->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                 QRhiSampler::None,
                                 QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_colSampler->create();
    m_clipSampler = r->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                  QRhiSampler::None,
                                  QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_clipSampler->create();

    m_waveSrb = r->newShaderResourceBindings();
    m_waveSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::FragmentStage, m_waveUbo),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, m_colTex, m_colSampler),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage, m_clipTex, m_clipSampler),
    });
    m_waveSrb->create();

    QShader vs = loadWaveShader(QStringLiteral(":/shaders/resources/shaders/overlay.vert.qsb"));
    QShader fs = loadWaveShader(QStringLiteral(":/shaders/resources/shaders/wavescope.frag.qsb"));
    if (!vs.isValid() || !fs.isValid()) {
        qWarning() << "WaveformWidget: wave shader load failed";
        return;
    }

    m_wavePipeline = r->newGraphicsPipeline();
    m_wavePipeline->setShaderStages({
        {QRhiShaderStage::Vertex, vs},
        {QRhiShaderStage::Fragment, fs},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });
    m_wavePipeline->setVertexInputLayout(layout);
    m_wavePipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_wavePipeline->setShaderResourceBindings(m_waveSrb);
    m_wavePipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    // The shader emits premultiplied color over the grid layer.
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    m_wavePipeline->setTargetBlends({blend});
    m_wavePipeline->create();
}

void WaveformWidget::initOverlayPipeline()
{
    QRhi* r = rhi();
    const qreal dpr = devicePixelRatioF();
    const QSize texSize(std::max(1, static_cast<int>(width() * dpr)),
                        std::max(1, static_cast<int>(height() * dpr)));

    m_gridTex = r->newTexture(QRhiTexture::RGBA8, texSize);
    m_gridTex->create();
    m_ovTex = r->newTexture(QRhiTexture::RGBA8, texSize);
    m_ovTex->create();
    m_ovSampler = r->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                QRhiSampler::None,
                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_ovSampler->create();

    m_gridSrb = r->newShaderResourceBindings();
    m_gridSrb->setBindings({
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, m_gridTex, m_ovSampler),
    });
    m_gridSrb->create();

    m_ovSrb = r->newShaderResourceBindings();
    m_ovSrb->setBindings({
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, m_ovTex, m_ovSampler),
    });
    m_ovSrb->create();

    QShader vs = loadWaveShader(QStringLiteral(":/shaders/resources/shaders/overlay.vert.qsb"));
    QShader fs = loadWaveShader(QStringLiteral(":/shaders/resources/shaders/overlay.frag.qsb"));
    if (!vs.isValid() || !fs.isValid()) {
        qWarning() << "WaveformWidget: overlay shader load failed";
        return;
    }

    m_ovPipeline = r->newGraphicsPipeline();
    m_ovPipeline->setShaderStages({
        {QRhiShaderStage::Vertex, vs},
        {QRhiShaderStage::Fragment, fs},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });
    m_ovPipeline->setVertexInputLayout(layout);
    m_ovPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_ovPipeline->setShaderResourceBindings(m_ovSrb);
    m_ovPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    // Both images are QImage::Format_ARGB32_Premultiplied — composite with
    // One / OneMinusSrcAlpha (see the #3294 note in SpectrumWidget).
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    m_ovPipeline->setTargetBlends({blend});
    m_ovPipeline->create();
}

void WaveformWidget::initialize(QRhiCommandBuffer* cb)
{
    if (m_rhiInitialized)
        return;
    QRhi* r = rhi();
    if (!r) {
        qWarning() << "WaveformWidget: QRhi initialization failed — no GPU backend";
        return;
    }

    initWavePipeline();
    initOverlayPipeline();
    if (!m_wavePipeline || !m_ovPipeline)
        return;

    auto* batch = r->nextResourceUpdateBatch();
    batch->uploadStaticBuffer(m_waveVbo, kWaveQuadData);
    cb->resourceUpdate(batch);

    // Force the cached images to re-render and re-upload on the first frame.
    m_gridCacheSize = QSize();
    m_overlayKey.clear();
    m_rhiInitialized = true;
}

void WaveformWidget::render(QRhiCommandBuffer* cb)
{
    QElapsedTimer frameTimer;
    frameTimer.start();

    if (m_shutdownPrepared)
        return;
    if (!m_rhiInitialized) {
        initialize(cb);
        if (!m_rhiInitialized)
            return;
    }

    renderGpuFrame(cb);

    ++m_perfPaintCount;
    const quint64 us = static_cast<quint64>(frameTimer.nsecsElapsed() / 1000);
    m_perfPaintUsTotal += us;
    m_perfPaintUsMax = std::max(m_perfPaintUsMax, us);
}

void WaveformWidget::renderGpuFrame(QRhiCommandBuffer* cb)
{
    QRhi* r = rhi();
    const QRectF plotRect = plotArea();
    const WaveformScopeModel& model = displayModel();
    const QString source = (m_paused ? m_pausedTransmitting : m_transmitting)
        ? QStringLiteral("TX")
        : QStringLiteral("RX");
    const int sampleRate = model.sampleRate();

    // ── Reduce: columns / band levels + whole-window stats ───────────────
    WaveformScopeModel::WindowStats stats;
    int columnCount = 0;
    int barCount = 0;
    if (m_viewMode == ViewMode::VerticalBars) {
        stats = model.windowStats();
        barCount = std::clamp(static_cast<int>(plotRect.width() / 12.0), 10, 18);
        QVector<float> bands;
        computeBandLevels(model, barCount, bands);
        columnCount = barCount;
        m_colUpload.resize(columnCount * 4);
        m_clipUpload.resize(columnCount);
        for (int i = 0; i < columnCount; ++i) {
            const bool have = bands.size() >= (i + 1) * 2;
            m_colUpload[i * 4 + 0] = have ? bands[i * 2] : 0.0f;      // level
            m_colUpload[i * 4 + 1] = have ? bands[i * 2 + 1] : 0.0f;  // amplitude
            m_colUpload[i * 4 + 2] = 0.0f;
            m_colUpload[i * 4 + 3] = 0.0f;
            m_clipUpload[i] = 0;
        }
    } else {
        if (m_viewMode == ViewMode::Bars)
            columnCount = std::clamp(static_cast<int>(plotRect.width() / 5.0), 12, 64);
        else
            columnCount = std::max(1, static_cast<int>(std::floor(plotRect.width())));
        stats = model.mergeColumns(columnCount, m_columns);
        columnCount = std::max(columnCount, 1);
        m_colUpload.resize(columnCount * 4);
        m_clipUpload.resize(columnCount);
        for (int i = 0; i < columnCount; ++i) {
            if (i < m_columns.size()) {
                const WaveformScopeModel::ColumnStats& c = m_columns[i];
                m_colUpload[i * 4 + 0] = c.min;
                m_colUpload[i * 4 + 1] = c.max;
                m_colUpload[i * 4 + 2] = c.rms;
                m_colUpload[i * 4 + 3] = c.peak;
                m_clipUpload[i] = c.clipped > 0 ? 255 : 0;
            } else {
                m_colUpload[i * 4 + 0] = 0.0f;
                m_colUpload[i * 4 + 1] = 0.0f;
                m_colUpload[i * 4 + 2] = 0.0f;
                m_colUpload[i * 4 + 3] = 0.0f;
                m_clipUpload[i] = 0;
            }
        }
    }

    const qint64 sinceAppend = activeModel().msSinceLastAppend();
    const bool stale = !m_paused
        && (sinceAppend < 0 || sinceAppend > kNoAudioTimeoutMs);

    auto* batch = r->nextResourceUpdateBatch();

    // ── Column + clip textures (recreate on width change) ─────────────────
    if (columnCount != m_colTexW) {
        m_colTexW = columnCount;
        m_colTex->setPixelSize(QSize(m_colTexW, 1));
        m_colTex->create();
        m_clipTex->setPixelSize(QSize(m_colTexW, 1));
        m_clipTex->create();
        m_waveSrb->create();   // rebind the recreated textures
        m_lastColUploadGen = ~0ull;   // recreated textures must be re-uploaded
    }
    // Skip the column/clip re-upload when the reduced data is unchanged —
    // model.generation() is the dirty counter (reset above on texture
    // recreate so a new/resized texture is always populated).
    if (model.generation() != m_lastColUploadGen) {
        m_lastColUploadGen = model.generation();
        QRhiTextureSubresourceUploadDescription colDesc(
            m_colUpload.constData(),
            m_colUpload.size() * static_cast<int>(sizeof(float)));
        batch->uploadTexture(m_colTex, QRhiTextureUploadEntry(0, 0, colDesc));
        QRhiTextureSubresourceUploadDescription clipDesc(
            m_clipUpload.constData(), m_clipUpload.size());
        batch->uploadTexture(m_clipTex, QRhiTextureUploadEntry(0, 0, clipDesc));
    }

    // ── Grid/background layer ─────────────────────────────────────────────
    const int gridKind =
        (m_viewMode == ViewMode::Bars || m_viewMode == ViewMode::VerticalBars)
            ? 1 : 0;
    ensureGridCache(gridKind, plotRect);
    const QSize texSize = m_gridCache.size();  // device px
    if (m_gridTex->pixelSize() != texSize) {
        m_gridTex->setPixelSize(texSize);
        m_gridTex->create();
        m_gridSrb->create();
        m_ovTex->setPixelSize(texSize);
        m_ovTex->create();
        m_ovSrb->create();
        m_overlayKey.clear();   // overlay image must re-render at the new size
    }
    if (m_gridCacheDirty) {
        const QImage rgba =
            m_gridCache.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        batch->uploadTexture(m_gridTex,
                             QRhiTextureUploadEntry(
                                 0, 0, QRhiTextureSubresourceUploadDescription(rgba)));
        m_gridCacheDirty = false;
    }

    // ── Text overlay layer ────────────────────────────────────────────────
    updateOverlayImage(stats, source, sampleRate, stale);
    if (m_overlayNeedsUpload) {
        const QImage rgba =
            m_overlayImage.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        batch->uploadTexture(m_ovTex,
                             QRhiTextureUploadEntry(
                                 0, 0, QRhiTextureSubresourceUploadDescription(rgba)));
        m_overlayNeedsUpload = false;
    }

    // ── Uniforms ──────────────────────────────────────────────────────────
    WaveUniforms u{};
    u.plot[0] = static_cast<float>(plotRect.x());
    u.plot[1] = static_cast<float>(plotRect.y());
    u.plot[2] = static_cast<float>(plotRect.width());
    u.plot[3] = static_cast<float>(plotRect.height());
    u.widget[0] = static_cast<float>(width());
    u.widget[1] = static_cast<float>(height());
    switch (m_viewMode) {
    case ViewMode::Graph:        u.widget[2] = 0.0f; break;
    case ViewMode::Envelope:     u.widget[2] = 1.0f; break;
    case ViewMode::Bars:         u.widget[2] = 2.0f; break;
    case ViewMode::VerticalBars: u.widget[2] = 3.0f; break;
    }
    u.widget[3] = m_amplitudeZoom;
    u.params[0] = static_cast<float>(columnCount);
    u.params[1] = waveformLineWidth();
    u.params[2] = static_cast<float>(barCount > 0 ? barCount : columnCount);
    u.params[3] = stats.empty ? 0.0f : 1.0f;
    fillColor(u.colWave, waveformColor());
    fillColor(u.colPeak, kPeakColor());
    fillColor(u.colRms, kRmsColor());
    fillColor(u.colClip, kClipColor());
    fillColor(u.colBarEmpty, kBarEmpty());
    fillColor(u.colWarn, AetherSDR::ThemeManager::instance().color("color.accent.warning"));
    fillColor(u.colRmsLight, kRmsColor().lighter(115));
    fillColor(u.colCap, kBarPeakHold());
    fillColor(u.colCenter, AetherSDR::theme::withAlpha("color.accent.success", 55));
    batch->updateDynamicBuffer(m_waveUbo, 0, sizeof(WaveUniforms), &u);

    // ── Draw: grid → waveform → text ──────────────────────────────────────
    const QSize outPx = renderTarget()->pixelSize();
    cb->beginPass(renderTarget(), QColor(0, 0, 0, 255), {1.0f, 0}, batch);
    cb->setViewport(QRhiViewport(0, 0, outPx.width(), outPx.height()));

    const QRhiCommandBuffer::VertexInput vin(m_waveVbo, 0);

    cb->setGraphicsPipeline(m_ovPipeline);
    cb->setShaderResources(m_gridSrb);
    cb->setVertexInput(0, 1, &vin);
    cb->draw(4);

    cb->setGraphicsPipeline(m_wavePipeline);
    cb->setShaderResources(m_waveSrb);
    cb->setVertexInput(0, 1, &vin);
    cb->draw(4);

    cb->setGraphicsPipeline(m_ovPipeline);
    cb->setShaderResources(m_ovSrb);
    cb->setVertexInput(0, 1, &vin);
    cb->draw(4);

    cb->endPass();
}

void WaveformWidget::updateOverlayImage(const WaveformScopeModel::WindowStats& stats,
                                        const QString& source, int sampleRate,
                                        bool stale)
{
    const QRectF plotRect = plotArea();
    const float peakDb = linearToDb(stats.peak);
    const float rmsDb = linearToDb(stats.rms);

    const QString readout = QStringLiteral("%1  RMS %2 dBFS  PK %3 dBFS")
        .arg(source)
        .arg(rmsDb, 0, 'f', 1)
        .arg(peakDb, 0, 'f', 1);
    const QString timeText = m_viewMode == ViewMode::VerticalBars
        ? QString::fromUtf8("%1 \xc2\xb7 %2 ms \xc2\xb7 frequency bands")
            .arg(formatSampleRate(sampleRate))
            .arg(m_windowMs)
        : QString::fromUtf8("%1 \xc2\xb7 %2 ms \xc2\xb7 %3 ms/div")
            .arg(formatSampleRate(sampleRate))
            .arg(m_windowMs)
            .arg(std::max(1, m_windowMs / 10));

    // Structural key (badges, footer, size, dpr) forces a rebuild; the numeric
    // readout re-renders at most ~5 Hz so text shaping stays off the
    // per-frame path. dpr is part of the key so moving the window between
    // monitors with different scale factors (same logical size + text) still
    // rebuilds the overlay at the new resolution instead of returning early
    // and leaving a blurry image.
    const qreal dpr = devicePixelRatioF();
    const QString structural = QStringLiteral("%1|%2|%3|%4|%5|%6x%7@%8")
        .arg(timeText)
        .arg(m_paused ? 1 : 0)
        .arg(stale ? 1 : 0)
        .arg(stats.clipCount > 0 ? 1 : 0)
        .arg(stats.empty ? 1 : 0)
        .arg(width())
        .arg(height())
        .arg(dpr, 0, 'f', 3);
    const QString key = structural + QLatin1Char('|') + readout
        + QLatin1Char('|') + QString::number(stats.clipCount);
    if (key == m_overlayKey)
        return;
    const bool structuralChange =
        !m_overlayKey.startsWith(structural + QLatin1Char('|'));
    if (!structuralChange && m_overlayTextAge.isValid()
        && m_overlayTextAge.elapsed() < 200)
        return;

    if (m_overlayImage.size() != size() * dpr) {
        m_overlayImage = QImage(size() * dpr, QImage::Format_ARGB32_Premultiplied);
        m_overlayImage.setDevicePixelRatio(dpr);
    }
    m_overlayImage.fill(Qt::transparent);

    {
        QPainter painter(&m_overlayImage);
        QFont labelFont = font();
        labelFont.setPointSizeF(std::max(7.0, labelFont.pointSizeF() - 1.0));
        painter.setFont(labelFont);
        painter.setPen(kLabelColor());
        painter.drawText(QRectF(7.0, 3.0, width() - 14.0, 16.0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         readout);

        if (stats.clipCount > 0) {
            QFont clipFont = labelFont;
            clipFont.setBold(true);
            painter.setFont(clipFont);
            painter.setPen(kClipColor());
            painter.drawText(QRectF(7.0, 3.0, width() - 14.0, 16.0),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("CLIP %1").arg(stats.clipCount));
            painter.setFont(labelFont);
        }

        painter.setPen(kMutedLabel());
        const QRectF footerRect(plotRect.left(), plotRect.bottom() + 2.0,
                                plotRect.width(), 15.0);
        painter.drawText(m_paused ? footerRect.adjusted(0.0, 0.0, -52.0, 0.0)
                                  : footerRect,
                         Qt::AlignLeft | Qt::AlignVCenter,
                         timeText);

        if (stats.empty || stale)
            drawNoAudio(painter, plotRect, source);
        if (m_paused)
            drawPausedBadge(painter, footerRect);
    }

    m_overlayKey = key;
    m_overlayNeedsUpload = true;
    m_overlayTextAge.restart();
}

void WaveformWidget::releaseResources()
{
    delete m_wavePipeline;   m_wavePipeline = nullptr;
    delete m_waveSrb;        m_waveSrb = nullptr;
    delete m_waveVbo;        m_waveVbo = nullptr;
    delete m_waveUbo;        m_waveUbo = nullptr;
    delete m_colTex;         m_colTex = nullptr;
    delete m_clipTex;        m_clipTex = nullptr;
    delete m_colSampler;     m_colSampler = nullptr;
    delete m_clipSampler;    m_clipSampler = nullptr;

    delete m_ovPipeline;     m_ovPipeline = nullptr;
    delete m_gridSrb;        m_gridSrb = nullptr;
    delete m_gridTex;        m_gridTex = nullptr;
    delete m_ovSrb;          m_ovSrb = nullptr;
    delete m_ovTex;          m_ovTex = nullptr;
    delete m_ovSampler;      m_ovSampler = nullptr;

    m_rhiInitialized = false;
}

#endif // AETHER_GPU_SPECTRUM

} // namespace AetherSDR
