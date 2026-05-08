#include "SpectrumWidget.h"
#include "SpectrumOverlayMenu.h"
#include "VfoWidget.h"
#include "SliceColors.h"
#include "SliceColorManager.h"
#include <QVariantAnimation>

#ifdef AETHER_GPU_SPECTRUM
#include <rhi/qrhi.h>
#include <QFile>
#endif

#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QNativeGestureEvent>
#include <QMenu>
#include <QToolTip>
#include <QDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QApplication>
#include <QGuiApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QStringList>
#include <QUrl>
#include "core/AppSettings.h"
#include "models/BandPlanManager.h"
#include "models/BandDefs.h"
#include <QDateTime>
#include <QTimeZone>
#include <QElapsedTimer>
#include "core/LogManager.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace AetherSDR {

static const QColor kAetherBrandBlue(0x00, 0xb4, 0xd8);
static const QColor kAetherBrandGreen(0x20, 0xc0, 0x60);
static const QColor kConnectionTextColor(0xd8, 0xe6, 0xf0);

static QString formatFlagFrequency(double freqMhz)
{
    const long long hz = static_cast<long long>(std::llround(freqMhz * 1.0e6));
    const int mhzPart = static_cast<int>(hz / 1000000);
    const int khzPart = static_cast<int>((hz / 1000) % 1000);
    const int hzPart = static_cast<int>(hz % 1000);
    return QString("%1.%2.%3")
        .arg(mhzPart)
        .arg(khzPart, 3, 10, QChar('0'))
        .arg(hzPart, 3, 10, QChar('0'));
}

// ─── Waterfall color scheme gradient presets ─────────────────────────────────

static constexpr WfGradientStop kDefaultStops[] = {
    {0.00f,   0,   0,   0},   // black
    {0.15f,   0,   0, 128},   // dark blue
    {0.30f,   0,  64, 255},   // blue
    {0.45f,   0, 200, 255},   // cyan
    {0.60f,   0, 220,   0},   // green
    {0.80f, 255, 255,   0},   // yellow
    {1.00f, 255,   0,   0},   // red
};
static constexpr WfGradientStop kGrayscaleStops[] = {
    {0.00f,   0,   0,   0},
    {1.00f, 255, 255, 255},
};
static constexpr WfGradientStop kBlueGreenStops[] = {
    {0.00f,   0,   0,   0},
    {0.25f,   0,  30, 120},
    {0.50f,   0, 100, 180},
    {0.75f,   0, 200, 130},
    {1.00f, 220, 255, 220},
};
static constexpr WfGradientStop kFireStops[] = {
    {0.00f,   0,   0,   0},
    {0.25f, 128,   0,   0},
    {0.50f, 220,  80,   0},
    {0.75f, 255, 200,   0},
    {1.00f, 255, 255, 220},
};
static constexpr WfGradientStop kPlasmaStops[] = {
    {0.00f,   0,   0,   0},
    {0.25f,  80,   0, 140},
    {0.50f, 200,   0, 120},
    {0.75f, 240, 120,   0},
    {1.00f, 255, 255,  80},
};

const WfGradientStop* wfSchemeStops(WfColorScheme scheme, int& count)
{
    switch (scheme) {
    case WfColorScheme::Grayscale: count = 2; return kGrayscaleStops;
    case WfColorScheme::BlueGreen: count = 5; return kBlueGreenStops;
    case WfColorScheme::Fire:      count = 5; return kFireStops;
    case WfColorScheme::Plasma:    count = 5; return kPlasmaStops;
    default:                       count = 7; return kDefaultStops;
    }
}

const char* wfSchemeName(WfColorScheme scheme)
{
    switch (scheme) {
    case WfColorScheme::Grayscale: return "Grayscale";
    case WfColorScheme::BlueGreen: return "Blue-Green";
    case WfColorScheme::Fire:      return "Fire";
    case WfColorScheme::Plasma:    return "Plasma";
    default:                       return "Default";
    }
}

// Interpolate a normalized value t (0–1) through the given gradient stops.
static QRgb interpolateGradient(float t, const WfGradientStop* stops, int n)
{
    int i = 0;
    while (i < n - 2 && stops[i + 1].pos < t) ++i;
    const float seg = (t - stops[i].pos) / (stops[i + 1].pos - stops[i].pos);
    const int r = static_cast<int>(stops[i].r + seg * (stops[i + 1].r - stops[i].r));
    const int g = static_cast<int>(stops[i].g + seg * (stops[i + 1].g - stops[i].g));
    const int b = static_cast<int>(stops[i].b + seg * (stops[i + 1].b - stops[i].b));
    return qRgb(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
}

SpectrumWidget::SpectrumWidget(QWidget* parent)
    : SPECTRUM_BASE_CLASS(parent)
{
    setMinimumHeight(100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
#ifdef AETHER_GPU_SPECTRUM
    // Explicitly request Metal on macOS.
#  ifdef Q_OS_MAC
    setApi(QRhiWidget::Api::Metal);
    // WA_NativeWindow forces Qt to create a dedicated native NSView for this widget.
    // Without it, QRhiWidget embedded in a QWidget hierarchy (especially one whose
    // backing store was created before this widget was added) fails to obtain a QRhi
    // context because the parent window's surface type is RasterSurface, not MetalSurface.
    // A native window gives QRhiWidget its own Metal-capable surface to render into.
    setAttribute(Qt::WA_NativeWindow);
#  else
    // Warn if running under XWayland — GLX context switching between the main
    // window and child dialogs (e.g. Radio Setup) can trigger BadAccess (#1233).
    // main.cpp normally forces native Wayland, but log it if we ended up here.
    if (QGuiApplication::platformName() == QLatin1String("xcb")
        && qEnvironmentVariable("XDG_SESSION_TYPE") == QLatin1String("wayland")) {
        qWarning() << "SpectrumWidget: running under XWayland with OpenGL — "
                      "GLX context issues may occur. Set QT_QPA_PLATFORM=wayland "
                      "or AETHER_NO_GPU=1 to work around (#1233)";
    }
#  endif
#else
    setAttribute(Qt::WA_OpaquePaintEvent);
#endif
    setSpectrumCursor(Qt::CrossCursor);
    setMouseTracking(true);

    // Floating overlay menu (child widget, stays on top)
    m_overlayMenu = new SpectrumOverlayMenu(this);
    m_overlayMenu->raise();

    m_tnfHoverPopup = new QLabel(this);
    m_tnfHoverPopup->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_tnfHoverPopup->setMargin(6);
    m_tnfHoverPopup->hide();
    m_tnfHoverPopup->raise();

    // Tune guide auto-hide timer (2-second inactivity timeout)
    m_tuneGuideTimer = new QTimer(this);
    m_tuneGuideTimer->setSingleShot(true);
    m_tuneGuideTimer->setInterval(4000);
    connect(m_tuneGuideTimer, &QTimer::timeout, this, [this]() {
        m_tuneGuideVisible = false;
        markOverlayDirty();
    });

    m_connectionAnimationTimer = new QTimer(this);
    m_connectionAnimationTimer->setInterval(40);
    connect(m_connectionAnimationTimer, &QTimer::timeout, this, [this]() {
        if (m_connectionAnimationVisible)
            markOverlayDirty();
    });

    // Load display settings (panIndex 0 by default — loadSettings() can be
    // called again after setPanIndex() for multi-pan)
    loadSettings();

    // VFO widgets are created per-slice via addVfoWidget().
    // m_vfoWidget is set by setActiveVfoWidget() as an alias to the active one.

    // Bottom-left waterfall zoom buttons
    static const QString kZoomBtnStyle =
        "QPushButton { background: rgba(15,15,26,180); border: 1px solid #304050;"
        " border-radius: 2px; color: #90a0b0; font-size: 11px; font-weight: bold;"
        " padding: 0; margin: 0; min-width: 0; }"
        "QPushButton:hover { background: rgba(30,50,70,200); color: #c8d8e8; }"
        "QPushButton:pressed { background: #00b4d8; color: #000; }";

    auto makeBtn = [&](const QString& text) {
        auto* btn = new QPushButton(text, this);
        btn->setFixedSize(22, 22);
        btn->setStyleSheet(kZoomBtnStyle);
        btn->setCursor(Qt::PointingHandCursor);
        return btn;
    };
    m_zoomSegBtn  = makeBtn("S");
    m_zoomBandBtn = makeBtn("B");
    m_zoomOutBtn  = makeBtn("\u2212");  // minus sign U+2212
    m_zoomInBtn   = makeBtn("+");

    // SmartSDR pcap: B sends "band_zoom=1", S sends "segment_zoom=1"
    connect(m_zoomBandBtn, &QPushButton::clicked, this, [this]() {
        emit bandZoomRequested();
    });
    connect(m_zoomSegBtn, &QPushButton::clicked, this, [this]() {
        emit segmentZoomRequested();
    });

    // Bandwidth zoom: − zooms out (wider BW), + zooms in (narrower BW)
    // Send both bandwidth AND current center to prevent the radio from
    // auto-centering the panadapter (which causes band jumps).
    auto emitZoom = [this](double factor) {
        // Clamp to limits so the final click always reaches the exact min/max,
        // matching mouse-drag which uses std::clamp (#1458).
        const double newBw = std::clamp(m_bandwidthMhz * factor, m_minBwMhz, m_maxBwMhz);
        if (newBw == m_bandwidthMhz) return;  // already at the hard limit

        // When zooming in, center on the active VFO so repeated clicks do not
        // push it toward the panadapter edge (#1932).
        double newCenter = m_centerMhz;
        if (factor < 1.0) {  // zooming in
            const auto* ao = activeOverlay();
            if (ao)
                newCenter = ao->freqMhz;
        }

        reprojectWaterfall(m_centerMhz, m_bandwidthMhz, newCenter, newBw);
        if (!reprojectSpectrum(m_centerMhz, m_bandwidthMhz, newCenter, newBw)) {
            m_bins.clear();
            m_smoothed.clear();
        }
        m_centerMhz = newCenter;
        m_bandwidthMhz = newBw;
        markOverlayDirty();
        emit frequencyRangeChangeRequested(newCenter, newBw);
    };
    connect(m_zoomOutBtn, &QPushButton::clicked, this, [emitZoom]() { emitZoom(1.5); });
    connect(m_zoomInBtn,  &QPushButton::clicked, this, [emitZoom]() { emitZoom(1.0 / 1.5); });

    connect(&SliceColorManager::instance(), &SliceColorManager::colorsChanged,
            this, [this]() { markOverlayDirty(); });
}

// ── Multi-VfoWidget management ────────────────────────────────────────────────

VfoWidget* SpectrumWidget::vfoWidget(int sliceId) const
{
    return m_vfoWidgets.value(sliceId, nullptr);
}

QString SpectrumWidget::settingsKey(const QString& base) const
{
    if (m_panIndex == 0)
        return base;  // backward compat — no suffix for pan 0
    return QString("%1_%2").arg(base).arg(m_panIndex);
}

void SpectrumWidget::loadSettings()
{
    auto& s = AppSettings::instance();
    m_spectrumFrac   = std::clamp(s.value(settingsKey("SpectrumSplitRatio"), "0.40").toFloat(), 0.10f, 0.90f);
    m_fftAverage     = s.value(settingsKey("DisplayFftAverage"), "0").toInt();
    m_fftFps         = s.value(settingsKey("DisplayFftFps"), "25").toInt();
    m_fftFillAlpha   = s.value(settingsKey("DisplayFftFillAlpha"), "0.70").toFloat();
    m_fftWeightedAvg = s.value(settingsKey("DisplayFftWeightedAvg"), "False").toString() == "True";
    const QString fillColorStr = s.value(settingsKey("DisplayFftFillColor"), "#00e5ff").toString();
    QColor parsed(fillColorStr);
    if (parsed.isValid())
        m_fftFillColor = parsed;
    m_wfColorGain    = s.value(settingsKey("DisplayWfColorGain"), "50").toInt();
    m_wfBlackLevel   = s.value(settingsKey("DisplayWfBlackLevel"), "15").toInt();
    m_wfAutoBlack    = s.value(settingsKey("DisplayWfAutoBlack"), "True").toString() == "True";
    m_wfAutoBlackOffset = s.value(settingsKey("DisplayWfAutoBlackOffset"), "50").toInt();
    m_wfLineDuration = s.value(settingsKey("DisplayWfLineDuration"), "100").toInt();
    // NB Waterfall Blanker (#277)
    m_wfBlankerEnabled   = s.value(settingsKey("WaterfallBlankingEnabled"), "False").toString() == "True";
    m_wfBlankerMode      = s.value(settingsKey("WaterfallBlankingMode"), "0").toInt();
    m_wfBlankerThreshold = std::clamp(
        s.value(settingsKey("WaterfallBlankingThreshold"), "1.15").toFloat(), 1.05f, 2.0f);
    // Migrate old ShowBandPlan bool → BandPlanFontSize int
    if (s.value("BandPlanFontSize").toString().isEmpty()) {
        m_bandPlanFontSize = s.value("ShowBandPlan", "True").toString() == "True" ? 6 : 0;
    } else {
        m_bandPlanFontSize = s.value("BandPlanFontSize", "6").toInt();
    }
    m_fftHeatMap     = s.value(settingsKey("DisplayFftHeatMap"), "True").toString() == "True";
    m_showGrid       = s.value(settingsKey("DisplayShowGrid"), "True").toString() == "True";
    m_freqGridSpacingKhz = s.value(settingsKey("DisplayFreqGridSpacing"), "0").toInt();
    m_fftLineWidth   = s.value(settingsKey("DisplayFftLineWidth"), "2.0").toFloat();
    m_wfColorScheme  = static_cast<WfColorScheme>(
        std::clamp(s.value(settingsKey("DisplayWfColorScheme"), "0").toInt(),
                   0, static_cast<int>(WfColorScheme::Count) - 1));
    m_singleClickTune = s.value("SingleClickTune", "False").toString() == "True";
    m_showTuneGuides  = s.value("ShowTuneGuides", "False").toString() == "True";
    m_extendedFrequencyLine = s.value("ExtendedFrequencyLine", "False").toString() == "True";

    // Background image — default to bundled logo, "none" = explicitly cleared
    QString bgPath = s.value(settingsKey("BackgroundImage"), ":/bg-default.jpg").toString();
    if (bgPath != "none" && !bgPath.isEmpty())
        setBackgroundImage(bgPath);
    m_bgOpacity = s.value(settingsKey("BackgroundOpacity"), "80").toInt();

    // Sync overlay menu sliders with restored settings
    if (m_overlayMenu) {
        m_overlayMenu->syncDisplaySettings(m_fftAverage, m_fftFps,
            static_cast<int>(m_fftFillAlpha * 100), m_fftWeightedAvg, m_fftFillColor,
            m_wfColorGain, m_wfBlackLevel, m_wfAutoBlack, m_wfAutoBlackOffset,
            m_wfLineDuration,
            75, false, m_fftHeatMap, static_cast<int>(m_wfColorScheme), m_showGrid,
            m_fftLineWidth);
        m_overlayMenu->syncExtraDisplaySettings(m_wfBlankerEnabled,
            m_wfBlankerThreshold, m_bgOpacity, m_freqGridSpacingKhz);
    }
}

VfoWidget* SpectrumWidget::addVfoWidget(int sliceId)
{
    if (m_vfoWidgets.contains(sliceId))
        return m_vfoWidgets[sliceId];

    auto* w = new VfoWidget(this);
    m_vfoWidgets[sliceId] = w;
    w->show();
    w->raise();
    m_overlayMenu->raiseAll();  // keep overlay + panels on top of all VFO widgets
    return w;
}

void SpectrumWidget::removeVfoWidget(int sliceId)
{
    if (auto* w = m_vfoWidgets.take(sliceId)) {
        if (m_vfoWidget == w)
            m_vfoWidget = nullptr;
        delete w;
    }
}

void SpectrumWidget::setActiveVfoWidget(int sliceId)
{
    m_vfoWidget = m_vfoWidgets.value(sliceId, nullptr);
    if (m_vfoWidget) {
        m_vfoWidget->raise();
        m_overlayMenu->raiseAll();  // keep overlay above VFO
    }
}

// ── Display control setters (save to AppSettings on each change) ──────────────

void SpectrumWidget::setBandPlanManager(BandPlanManager* mgr) {
    m_bandPlanMgr = mgr;
    if (mgr)
        connect(mgr, &BandPlanManager::planChanged, this, QOverload<>::of(&QWidget::update));
}

void SpectrumWidget::setFftAverage(int frames) {
    m_fftAverage = frames;
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayFftAverage"), QString::number(frames));
    s.save();
}
void SpectrumWidget::setFftWeightedAvg(bool on) {
    m_fftWeightedAvg = on;
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayFftWeightedAvg"), on ? "True" : "False");
    s.save();
}
void SpectrumWidget::setFftFps(int fps) {
    m_fftFps = fps;
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayFftFps"), QString::number(fps));
    s.save();
}
void SpectrumWidget::setFftHeatMap(bool on) {
    m_fftHeatMap = on;
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayFftHeatMap"), on ? "True" : "False");
    s.save();
}
void SpectrumWidget::setShowGrid(bool on) {
    m_showGrid = on;
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayShowGrid"), on ? "True" : "False");
    s.save();
    markOverlayDirty();
}
void SpectrumWidget::setFreqGridSpacing(int khz) {
    m_freqGridSpacingKhz = khz;
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayFreqGridSpacing"), QString::number(khz));
    s.save();
    markOverlayDirty();
}
void SpectrumWidget::setShowTuneGuides(bool on) {
    m_showTuneGuides = on;
    if (!on) {
        m_tuneGuideVisible = false;
        m_tuneGuideTimer->stop();
    }
    auto& s = AppSettings::instance();
    s.setValue("ShowTuneGuides", on ? "True" : "False");
    s.save();
    markOverlayDirty();

    // Propagate to all sibling SpectrumWidgets so the toggle is global
    if (QWidget* top = window()) {
        const auto siblings = top->findChildren<SpectrumWidget*>();
        for (SpectrumWidget* sw : siblings) {
            if (sw != this && sw->m_showTuneGuides != on) {
                sw->m_showTuneGuides = on;
                if (!on) {
                    sw->m_tuneGuideVisible = false;
                    sw->m_tuneGuideTimer->stop();
                }
                sw->markOverlayDirty();
            }
        }
    }
}
void SpectrumWidget::setExtendedFrequencyLine(bool on) {
    m_extendedFrequencyLine = on;
    auto& s = AppSettings::instance();
    s.setValue("ExtendedFrequencyLine", on ? "True" : "False");
    s.save();
    markOverlayDirty();

    // Propagate to all sibling SpectrumWidgets so the toggle is global.
    if (QWidget* top = window()) {
        const auto siblings = top->findChildren<SpectrumWidget*>();
        for (SpectrumWidget* sw : siblings) {
            if (sw != this && sw->m_extendedFrequencyLine != on) {
                sw->m_extendedFrequencyLine = on;
                sw->markOverlayDirty();
            }
        }
    }
}
void SpectrumWidget::setFftLineWidth(float w) {
    m_fftLineWidth = std::clamp(w, 0.0f, 5.0f);
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayFftLineWidth"), QString::number(m_fftLineWidth, 'f', 1));
    s.save();
}
void SpectrumWidget::setFftFillAlpha(float a) {
    m_fftFillAlpha = std::clamp(a, 0.0f, 1.0f);
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayFftFillAlpha"), QString::number(m_fftFillAlpha, 'f', 2));
    s.save();
    markOverlayDirty();
}
void SpectrumWidget::setFftFillColor(const QColor& c) {
    m_fftFillColor = c;
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayFftFillColor"), c.name());
    s.save();
    markOverlayDirty();
}
void SpectrumWidget::setWfColorGain(int gain) {
    int clamped = std::clamp(gain, 0, 100);
    if (clamped != m_wfColorGain) {
        m_wfColorGain = clamped;
        auto& s = AppSettings::instance();
        s.setValue(settingsKey("DisplayWfColorGain"), QString::number(m_wfColorGain));
        s.save();
    }
    update();
}
void SpectrumWidget::setWfBlackLevel(int level) {
    int clamped = std::clamp(level, 0, 100);
    if (clamped != m_wfBlackLevel) {
        m_wfBlackLevel = clamped;
        auto& s = AppSettings::instance();
        s.setValue(settingsKey("DisplayWfBlackLevel"), QString::number(m_wfBlackLevel));
        s.save();
    }
    update();
}
void SpectrumWidget::setWfAutoBlack(bool on) {
    m_wfAutoBlack = on;
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayWfAutoBlack"), on ? "True" : "False");
    s.save();
    update();
}
void SpectrumWidget::setWfAutoBlackOffset(int level) {
    int clamped = std::clamp(level, 0, 100);
    if (clamped != m_wfAutoBlackOffset) {
        m_wfAutoBlackOffset = clamped;
        auto& s = AppSettings::instance();
        s.setValue(settingsKey("DisplayWfAutoBlackOffset"), QString::number(m_wfAutoBlackOffset));
        s.save();
    }
    update();
}
void SpectrumWidget::setWfLineDuration(int ms) {
    m_wfLineDuration = std::clamp(ms, 50, 500);
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayWfLineDuration"), QString::number(m_wfLineDuration));
    s.save();
    // Re-calibrate the time scale for the new rate
    resetWfTimeScale();
}

void SpectrumWidget::setWfColorScheme(int scheme) {
    auto clamped = static_cast<WfColorScheme>(
        std::clamp(scheme, 0, static_cast<int>(WfColorScheme::Count) - 1));
    if (clamped != m_wfColorScheme) {
        m_wfColorScheme = clamped;
        auto& s = AppSettings::instance();
        s.setValue(settingsKey("DisplayWfColorScheme"), QString::number(static_cast<int>(m_wfColorScheme)));
        s.save();
    }
    update();
}

// ── NB Waterfall Blanker setters (#277) ──────────────────────────────────────

void SpectrumWidget::setWfBlankerEnabled(bool on)
{
    m_wfBlankerEnabled = on;
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("WaterfallBlankingEnabled"), on ? "True" : "False");
    s.save();
    if (!on) {
        m_wfBlankerRingCount = 0;
        m_wfBlankerRingIdx = 0;
    }
}

void SpectrumWidget::setWfBlankerThreshold(float t)
{
    m_wfBlankerThreshold = std::clamp(t, 1.05f, 2.0f);
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("WaterfallBlankingThreshold"),
              QString::number(m_wfBlankerThreshold, 'f', 2));
    s.save();
}

void SpectrumWidget::setWfBlankerMode(int mode)
{
    m_wfBlankerMode = qBound(0, mode, 1);
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("WaterfallBlankingMode"), QString::number(m_wfBlankerMode));
    s.save();
}

void SpectrumWidget::resetWfTimeScale() {
    m_wfMsPerRow = static_cast<float>(m_wfLineDuration);  // start from radio's value
    m_wfPrevTimecode = 0;
    m_wfPrevTimecodeMs = 0;
    m_wfCalibrationCount = 0;
    m_wfTimeScaleLocked = false;
    ensureWaterfallHistory();
}

int SpectrumWidget::waterfallHistoryCapacityRows() const
{
    const int msPerRow = std::max(1, m_wfLineDuration);
    return static_cast<int>((kWaterfallHistoryMs + msPerRow - 1) / msPerRow);
}

int SpectrumWidget::maxWaterfallHistoryOffsetRows() const
{
    return std::max(0, m_wfHistoryRowCount - m_waterfall.height());
}

int SpectrumWidget::historyRowIndexForAge(int ageRows) const
{
    if (m_waterfallHistory.isNull() || ageRows < 0 || ageRows >= m_wfHistoryRowCount) {
        return -1;
    }
    return (m_wfHistoryWriteRow + ageRows) % m_waterfallHistory.height();
}

QString SpectrumWidget::pausedTimeLabelForAge(int ageRows) const
{
    const int rowIndex = historyRowIndexForAge(ageRows);
    if (rowIndex < 0 || rowIndex >= m_wfHistoryTimestamps.size()) {
        return QString();
    }

    const qint64 timestampMs = m_wfHistoryTimestamps[rowIndex];
    if (timestampMs <= 0) {
        return QString();
    }

    const QDateTime utc = QDateTime::fromMSecsSinceEpoch(timestampMs, QTimeZone::utc());
    return "-" + utc.toString("HH:mm:ssZ");
}

void SpectrumWidget::ensureWaterfallHistory()
{
    if (m_waterfall.isNull()) {
        return;
    }

    const QSize desiredSize(m_waterfall.width(), waterfallHistoryCapacityRows());
    if (desiredSize.width() <= 0 || desiredSize.height() <= 0) {
        return;
    }

    if (m_waterfallHistory.size() == desiredSize) {
        return;
    }

    // Preserve rows across width changes (e.g. band stack toggle, manual
    // window resize) by horizontally scaling the existing history image.
    // Height capacity is fixed via waterfallHistoryCapacityRows() so row
    // indices and timestamps remain valid.
    QImage newHistory;
    if (!m_waterfallHistory.isNull() && m_wfHistoryRowCount > 0
        && m_waterfallHistory.height() == desiredSize.height()) {
        newHistory = m_waterfallHistory.scaled(
            desiredSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }
    if (newHistory.isNull() || newHistory.size() != desiredSize) {
        newHistory = QImage(desiredSize, QImage::Format_RGB32);
        newHistory.fill(Qt::black);
        m_wfHistoryTimestamps = QVector<qint64>(desiredSize.height(), 0);
        m_wfHistoryWriteRow = 0;
        m_wfHistoryRowCount = 0;
        m_wfHistoryOffsetRows = 0;
        m_wfLive = true;
    }
    m_waterfallHistory = newHistory;
}

void SpectrumWidget::appendVisibleRow(const QRgb* rowData)
{
    if (m_waterfall.isNull() || rowData == nullptr) {
        return;
    }

    const int h = m_waterfall.height();
    if (h <= 0) {
        return;
    }

    m_wfWriteRow = (m_wfWriteRow - 1 + h) % h;
    auto* row = reinterpret_cast<QRgb*>(m_waterfall.bits() + m_wfWriteRow * m_waterfall.bytesPerLine());
    std::memcpy(row, rowData, m_waterfall.width() * sizeof(QRgb));
}

void SpectrumWidget::appendHistoryRow(const QRgb* rowData, qint64 timestampMs)
{
    ensureWaterfallHistory();
    if (m_waterfallHistory.isNull() || rowData == nullptr) {
        return;
    }

    const int h = m_waterfallHistory.height();
    m_wfHistoryWriteRow = (m_wfHistoryWriteRow - 1 + h) % h;
    auto* row = reinterpret_cast<QRgb*>(m_waterfallHistory.bits()
                                        + m_wfHistoryWriteRow * m_waterfallHistory.bytesPerLine());
    std::memcpy(row, rowData, m_waterfallHistory.width() * sizeof(QRgb));
    if (m_wfHistoryWriteRow >= 0 && m_wfHistoryWriteRow < m_wfHistoryTimestamps.size()) {
        m_wfHistoryTimestamps[m_wfHistoryWriteRow] = timestampMs;
    }
    if (m_wfHistoryRowCount < h) {
        ++m_wfHistoryRowCount;
    }
    if (!m_wfLive) {
        m_wfHistoryOffsetRows = std::min(m_wfHistoryOffsetRows + 1, maxWaterfallHistoryOffsetRows());
    }
}

void SpectrumWidget::rebuildWaterfallViewport()
{
    if (m_waterfall.isNull()) {
        return;
    }

    m_wfHistoryOffsetRows = std::clamp(m_wfHistoryOffsetRows, 0, maxWaterfallHistoryOffsetRows());
    m_waterfall.fill(Qt::black);
    m_wfWriteRow = 0;

    if (m_waterfallHistory.isNull()) {
        update();
        return;
    }

    const int rowWidthBytes = m_waterfall.width() * static_cast<int>(sizeof(QRgb));
    for (int y = 0; y < m_waterfall.height(); ++y) {
        const int rowIndex = historyRowIndexForAge(m_wfHistoryOffsetRows + y);
        if (rowIndex < 0) {
            break;
        }
        const QRgb* src = reinterpret_cast<const QRgb*>(
            m_waterfallHistory.constScanLine(rowIndex));
        auto* dst = reinterpret_cast<QRgb*>(m_waterfall.scanLine(y));
        std::memcpy(dst, src, rowWidthBytes);
    }

#ifdef AETHER_GPU_SPECTRUM
    m_wfTexFullUpload = true;
#endif
    update();
}

void SpectrumWidget::setWaterfallLive(bool live)
{
    if (m_wfLive == live) {
        return;
    }
    if (live) {
        m_wfHistoryOffsetRows = 0;
    }
    m_wfLive = live;
    rebuildWaterfallViewport();
    markOverlayDirty();
}

int SpectrumWidget::waterfallStripWidth() const
{
    return m_wfLive ? DBM_STRIP_W : 72;
}

QRect SpectrumWidget::waterfallTimeScaleRect(const QRect& wfRect) const
{
    const int stripWidth = waterfallStripWidth();
    const int stripX = wfRect.right() - stripWidth + 1;
    return QRect(stripX, wfRect.top(), stripWidth, wfRect.height());
}

QRect SpectrumWidget::waterfallLiveButtonRect(const QRect& wfRect) const
{
    const QRect strip = waterfallTimeScaleRect(wfRect);
    const int buttonW = 32;
    const int buttonH = 16;
    const int buttonX = strip.right() - buttonW - 2;
    const int buttonY = wfRect.top() - FREQ_SCALE_H + 2;
    return QRect(buttonX, buttonY, buttonW, buttonH);
}

// ─────────────────────────────────────────────────────────────────────────────

void SpectrumWidget::clearDisplay()
{
    m_bins.clear();
    m_smoothed.clear();
    if (!m_waterfall.isNull()) {
        m_waterfall.fill(Qt::black);
    }
    if (!m_waterfallHistory.isNull()) {
        m_waterfallHistory.fill(Qt::black);
    }
    std::fill(m_wfHistoryTimestamps.begin(), m_wfHistoryTimestamps.end(), 0);
    m_wfWriteRow = 0;
    m_wfHistoryWriteRow = 0;
    m_wfHistoryRowCount = 0;
    m_wfHistoryOffsetRows = 0;
    m_wfLive = true;
    markOverlayDirty();
}

void SpectrumWidget::setConnectionAnimationVisible(bool on, const QString& label)
{
    const QString nextLabel = label.trimmed().isEmpty()
        ? QStringLiteral("Connecting to radio…")
        : label.trimmed();
    const bool changed = (m_connectionAnimationVisible != on)
        || (on && m_connectionAnimationLabel != nextLabel);

    if (!changed) {
        return;
    }

    m_connectionAnimationVisible = on;
    if (on) {
        m_connectionAnimationLabel = nextLabel;
        m_connectionAnimationClock.restart();
        m_connectionAnimationTimer->start();
    } else {
        m_connectionAnimationLabel.clear();
        m_connectionAnimationTimer->stop();
    }
    markOverlayDirty();
}

void SpectrumWidget::drawConnectionAnimation(QPainter& p, const QRect& contentRect)
{
    if (!m_connectionAnimationVisible || !m_connectionAnimationClock.isValid()) {
        return;
    }

    const qreal insetX = qMin(40.0, contentRect.width() * 0.10);
    const qreal insetTop = qMin(18.0, contentRect.height() * 0.08);
    const qreal insetBottom = qMin(22.0, contentRect.height() * 0.10);
    const QRectF available = QRectF(contentRect).adjusted(insetX, insetTop, -insetX, -insetBottom);
    if (available.width() < 140.0 || available.height() < 96.0) {
        return;
    }

    const qreal seconds = m_connectionAnimationClock.elapsed() / 1000.0;
    const qreal towerHeight = qMin(available.height() * 0.27, 90.0);
    const qreal towerWidth = towerHeight * 0.34;
    const qreal anchorX = static_cast<qreal>(mhzToX(m_centerMhz));
    const qreal centerX = qBound(available.left() + towerWidth * 1.5,
                                 anchorX,
                                 available.right() - towerWidth * 1.5);
    const qreal baseY = available.top() + available.height() * 0.66;
    const qreal topY = baseY - towerHeight;
    const qreal phase = std::fmod(seconds * 0.7, 1.0);

    auto withAlpha = [](QColor color, int alpha) {
        color.setAlpha(alpha);
        return color;
    };

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);

    QRadialGradient glow(QPointF(centerX, topY + towerHeight * 0.42),
                         towerHeight * 1.05);
    glow.setColorAt(0.0, withAlpha(kAetherBrandBlue, 48));
    glow.setColorAt(0.55, withAlpha(kAetherBrandGreen, 22));
    glow.setColorAt(1.0, QColor(0, 0, 0, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(QPointF(centerX, topY + towerHeight * 0.44),
                  towerHeight * 0.98, towerHeight * 0.72);

    static constexpr qreal kTau = 6.28318530717958647692;
    const qreal pulse = 0.55 + 0.45 * std::sin(seconds * kTau);
    const qreal waveCenterY = topY + towerHeight * 0.38;
    for (int ring = 0; ring < 3; ++ring) {
        const qreal ringProgress = std::fmod(phase + ring * 0.24, 1.0);
        const qreal radiusX = towerWidth * 0.50 + ringProgress * towerHeight * 0.68;
        const qreal radiusY = radiusX * 0.86;
        QColor waveColor = (ring % 2 == 0) ? kAetherBrandBlue : kAetherBrandGreen;
        const qreal fade = 1.0 - ringProgress;
        waveColor.setAlphaF(qBound(0.10, 0.16 + fade * 0.48 * pulse, 0.70));
        QPen wavePen(waveColor,
                     qMax(1.8, towerWidth * (0.075 + fade * 0.05)),
                     Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(wavePen);
        p.setBrush(Qt::NoBrush);
        const QRectF leftArcRect(centerX - towerWidth * 0.16 - radiusX * 2.0,
                                 waveCenterY - radiusY,
                                 radiusX * 2.0, radiusY * 2.0);
        const QRectF rightArcRect(centerX + towerWidth * 0.16,
                                  waveCenterY - radiusY,
                                  radiusX * 2.0, radiusY * 2.0);
        p.drawArc(leftArcRect, 100 * 16, 160 * 16);
        p.drawArc(rightArcRect, -80 * 16, 160 * 16);
    }

    QLinearGradient towerGradient(QPointF(centerX, topY), QPointF(centerX, baseY));
    towerGradient.setColorAt(0.0, kAetherBrandBlue.lighter(115));
    towerGradient.setColorAt(0.55, kAetherBrandBlue);
    towerGradient.setColorAt(1.0, kAetherBrandGreen);

    QPainterPath towerFill;
    towerFill.moveTo(centerX - towerWidth * 0.48, baseY);
    towerFill.lineTo(centerX, topY + towerHeight * 0.08);
    towerFill.lineTo(centerX + towerWidth * 0.48, baseY);
    towerFill.closeSubpath();
    QLinearGradient fillGradient(QPointF(centerX, topY), QPointF(centerX, baseY));
    fillGradient.setColorAt(0.0, withAlpha(kAetherBrandBlue, 28));
    fillGradient.setColorAt(1.0, withAlpha(kAetherBrandGreen, 12));
    p.fillPath(towerFill, fillGradient);

    QPen towerPen(QBrush(towerGradient), qMax(2.2, towerWidth * 0.11),
                  Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(towerPen);
    p.drawLine(QPointF(centerX - towerWidth * 0.48, baseY),
               QPointF(centerX, topY));
    p.drawLine(QPointF(centerX + towerWidth * 0.48, baseY),
               QPointF(centerX, topY));

    const qreal mastBottomY = topY + towerHeight * 0.28;
    p.drawLine(QPointF(centerX, topY - towerHeight * 0.10),
               QPointF(centerX, mastBottomY));

    for (qreal t : {0.25, 0.45, 0.66, 0.84}) {
        const qreal y = topY + towerHeight * t;
        const qreal halfWidth = towerWidth * 0.48 * t;
        p.drawLine(QPointF(centerX - halfWidth, y), QPointF(centerX + halfWidth, y));
        if (t < 0.8) {
            const qreal nextT = qMin(t + 0.16, 0.98);
            const qreal nextY = topY + towerHeight * nextT;
            const qreal nextHalfWidth = towerWidth * 0.48 * nextT;
            p.drawLine(QPointF(centerX - halfWidth, y),
                       QPointF(centerX + nextHalfWidth, nextY));
            p.drawLine(QPointF(centerX + halfWidth, y),
                       QPointF(centerX - nextHalfWidth, nextY));
        }
    }

    p.drawLine(QPointF(centerX - towerWidth * 0.72, baseY),
               QPointF(centerX + towerWidth * 0.72, baseY));
    p.setBrush(kAetherBrandBlue);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(centerX, topY - towerHeight * 0.10),
                  towerWidth * 0.10, towerWidth * 0.10);

    QFont titleFont = p.font();
    titleFont.setPointSizeF(qBound(10.5, available.height() * 0.078, 18.0));
    titleFont.setBold(true);
    p.setFont(titleFont);
    QFontMetricsF titleFm(titleFont);

    QString subtitle = QStringLiteral("Getting everything ready");
    if (m_connectionAnimationLabel.contains(QStringLiteral("remote"), Qt::CaseInsensitive)
        || m_connectionAnimationLabel.contains(QStringLiteral("smartlink"), Qt::CaseInsensitive)) {
        subtitle = QStringLiteral("Establishing a secure radio link");
    } else if (m_connectionAnimationLabel.contains(QStringLiteral("reconnect"), Qt::CaseInsensitive)) {
        subtitle = QStringLiteral("Restoring your session");
    }

    QFont subtitleFont = titleFont;
    subtitleFont.setPointSizeF(qMax(9.0, titleFont.pointSizeF() - 2.5));
    subtitleFont.setBold(false);
    QFontMetricsF subtitleFm(subtitleFont);

    const qreal titleY = baseY + towerHeight * 0.18;
    const QRectF titleRect(available.left(), titleY, available.width(), titleFm.height() + 6.0);
    const QRectF subtitleRect(available.left(), titleRect.bottom() + 4.0,
                              available.width(), subtitleFm.height() + 4.0);

    QLinearGradient titleGradient(titleRect.topLeft(), titleRect.topRight());
    titleGradient.setColorAt(0.0, kAetherBrandBlue.lighter(108));
    titleGradient.setColorAt(1.0, kAetherBrandGreen.lighter(105));
    p.setFont(titleFont);
    p.setPen(QPen(QBrush(titleGradient), 1.0));
    p.drawText(titleRect, Qt::AlignHCenter | Qt::AlignTop, m_connectionAnimationLabel);

    p.setFont(subtitleFont);
    p.setPen(withAlpha(kConnectionTextColor, 180));
    p.drawText(subtitleRect, Qt::AlignHCenter | Qt::AlignTop, subtitle);

    p.restore();
}

void SpectrumWidget::resetGpuResources()
{
#ifdef AETHER_GPU_SPECTRUM
    // On macOS/Windows, the GPU surface doesn't survive reparenting — tear
    // down old pipelines so initialize() rebuilds them for the new window.
    // On Linux (OpenGL), a simple update() is sufficient (#1240).
#ifndef Q_OS_LINUX
    releaseResources();
#endif
#endif
    update();
}

void SpectrumWidget::reprojectWaterfall(double oldCenterMhz, double oldBandwidthMhz,
                                        double newCenterMhz, double newBandwidthMhz)
{
    if (oldBandwidthMhz <= 0.0 || newBandwidthMhz <= 0.0) {
        return;
    }

    const double oldStartMhz = oldCenterMhz - oldBandwidthMhz / 2.0;
    const double oldEndMhz = oldCenterMhz + oldBandwidthMhz / 2.0;
    const double newStartMhz = newCenterMhz - newBandwidthMhz / 2.0;
    const double newEndMhz = newCenterMhz + newBandwidthMhz / 2.0;
    const double overlapStartMhz = std::max(oldStartMhz, newStartMhz);
    const double overlapEndMhz = std::min(oldEndMhz, newEndMhz);

    auto reprojectImage = [&](QImage& image) {
        if (image.isNull()) {
            return;
        }

        const int imageWidth = image.width();
        const int imageHeight = image.height();
        if (imageWidth <= 0 || imageHeight <= 0) {
            return;
        }

        QImage reprojected(imageWidth, imageHeight, QImage::Format_RGB32);
        reprojected.fill(Qt::black);

        if (overlapEndMhz > overlapStartMhz) {
            const double srcLeft = (overlapStartMhz - oldStartMhz) / oldBandwidthMhz * imageWidth;
            const double srcRight = (overlapEndMhz - oldStartMhz) / oldBandwidthMhz * imageWidth;
            const double dstLeft = (overlapStartMhz - newStartMhz) / newBandwidthMhz * imageWidth;
            const double dstRight = (overlapEndMhz - newStartMhz) / newBandwidthMhz * imageWidth;

            if (srcRight > srcLeft && dstRight > dstLeft) {
                QPainter painter(&reprojected);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
                painter.drawImage(QRectF(dstLeft, 0.0, dstRight - dstLeft, imageHeight),
                                  image,
                                  QRectF(srcLeft, 0.0, srcRight - srcLeft, imageHeight));
            }
        }

        image = std::move(reprojected);
    };

    reprojectImage(m_waterfall);
    reprojectImage(m_waterfallHistory);
    m_prevTileScanline.clear();
#ifdef AETHER_GPU_SPECTRUM
    m_wfTexFullUpload = true;
#endif
}

bool SpectrumWidget::reprojectSpectrum(double oldCenterMhz, double oldBandwidthMhz,
                                       double newCenterMhz, double newBandwidthMhz)
{
    if (oldBandwidthMhz <= 0.0 || newBandwidthMhz <= 0.0) {
        return false;
    }

    const double oldStartMhz = oldCenterMhz - oldBandwidthMhz / 2.0;
    const double oldEndMhz = oldCenterMhz + oldBandwidthMhz / 2.0;
    const double newStartMhz = newCenterMhz - newBandwidthMhz / 2.0;
    const double newEndMhz = newCenterMhz + newBandwidthMhz / 2.0;
    const double overlapStartMhz = std::max(oldStartMhz, newStartMhz);
    const double overlapEndMhz = std::min(oldEndMhz, newEndMhz);
    if (overlapEndMhz <= overlapStartMhz) {
        return false;
    }

    auto reprojectBins = [&](QVector<float>& bins) {
        const int binCount = bins.size();
        if (binCount <= 0) {
            return;
        }

        const QVector<float> oldBins = std::move(bins);
        QVector<float> reprojected(binCount, m_refLevel - m_dynamicRange);

        for (int dst = 0; dst < binCount; ++dst) {
            const double dstFrac = (static_cast<double>(dst) + 0.5) / binCount;
            const double freqMhz = newStartMhz + dstFrac * newBandwidthMhz;
            if (freqMhz < overlapStartMhz || freqMhz > overlapEndMhz) {
                continue;
            }

            const double srcPos = ((freqMhz - oldStartMhz) / oldBandwidthMhz) * binCount - 0.5;
            const int srcLeft = static_cast<int>(std::floor(srcPos));
            const int srcRight = srcLeft + 1;
            if (srcLeft < 0 || srcRight >= binCount) {
                const int src = std::clamp(static_cast<int>(std::round(srcPos)), 0, binCount - 1);
                reprojected[dst] = oldBins[src];
                continue;
            }

            const float t = static_cast<float>(srcPos - srcLeft);
            reprojected[dst] = oldBins[srcLeft] * (1.0f - t) + oldBins[srcRight] * t;
        }

        bins = std::move(reprojected);
    };

    reprojectBins(m_bins);
    reprojectBins(m_smoothed);
    return !m_bins.isEmpty() || !m_smoothed.isEmpty();
}

void SpectrumWidget::setFrequencyRange(double centerMhz, double bandwidthMhz)
{
    if (centerMhz == m_centerMhz && bandwidthMhz == m_bandwidthMhz)
        return;

    const double oldCenterMhz = m_centerMhz;
    const double oldBandwidthMhz = m_bandwidthMhz;

    // Stale-echo guard: if animation is running and the incoming center equals
    // the value m_centerMhz had when the animation started, this is a status
    // echo from a radio command sent *before* the pan-follow (e.g. a floor-level
    // change whose echo-back includes the pre-animation center).  Accepting it
    // would either reverse the in-flight animation or trigger a false large-shift
    // that blanks the spectrum, so skip it — but only when the bandwidth is also
    // unchanged, so that bandwidth corrections (e.g. after xpixels resize) are
    // not silently dropped (#1729).
    if (m_panCenterAnim &&
        m_panCenterAnim->state() != QAbstractAnimation::Stopped &&
        std::abs(centerMhz - m_panCenterStart) < 1e-9 &&
        bandwidthMhz == m_bandwidthMhz) {
        return;
    }

    // Distinguish pan-follow nudges (#989) from large jumps (band change, click-to-tune).
    // Nudges shift center by ~10% of halfBw; 25% threshold comfortably separates the two.
    const double halfBw = bandwidthMhz / 2.0;
    const bool bwChanged = (bandwidthMhz != m_bandwidthMhz);
    // Compare incoming center against the animation's *destination* (m_panCenterTarget),
    // not the mid-animation position (m_centerMhz). During a short retargetable
    // nudge, the animated center is far from its start, so a subsequent nudge
    // of similar magnitude would
    // falsely exceed the 25 % threshold and trigger a large-shift clear+blank.
    const double refForShiftCheck = (m_panCenterAnim &&
        m_panCenterAnim->state() != QAbstractAnimation::Stopped)
        ? m_panCenterTarget : m_centerMhz;
    const bool largeShift = bwChanged ||
        (halfBw > 0.0 && std::abs(centerMhz - refForShiftCheck) > halfBw * 0.25);

    if (bwChanged) {
        m_bandwidthMhz = bandwidthMhz;
    }

    if (largeShift) {
        // Large jump: cancel any running animation and snap immediately.
        if (m_panCenterAnim && m_panCenterAnim->state() != QAbstractAnimation::Stopped) {
            m_panCenterAnim->stop();
        }
        if (oldBandwidthMhz > 0.0 && bandwidthMhz > 0.0) {
            reprojectWaterfall(oldCenterMhz, oldBandwidthMhz, centerMhz, bandwidthMhz);
        }
        const bool keptSpectrum = reprojectSpectrum(oldCenterMhz, oldBandwidthMhz,
                                                    centerMhz, bandwidthMhz);
        if (!keptSpectrum) {
            m_bins.clear();
            m_smoothed.clear();
            m_wfWriteRow = 0;
        }
        m_centerMhz       = centerMhz;
        m_panCenterTarget = centerMhz;
        markOverlayDirty();
        return;
    }

    // Small nudge: animate m_centerMhz smoothly so the VFO widget glides rather
    // than snapping. The radio command has already been sent with the final center
    // by panFollowVfo; the echo-back will be a no-op once the animation lands.

    // Guard: if already animating toward this exact target (e.g. radio echo-back
    // arriving mid-animation), don't restart — just let the current animation finish.
    if (m_panCenterAnim &&
        m_panCenterAnim->state() != QAbstractAnimation::Stopped &&
        std::abs(m_panCenterTarget - centerMhz) < 1e-9) {
        return;
    }

    // Only reproject the waterfall when starting a fresh animation.  If we are
    // already animating toward a different target (rapid edge scroll: multiple
    // echo-backs arrive before the first animation finishes), skip the reproject.
    // The waterfall was already shifted at the start of this animation session;
    // re-shifting on every retarget causes repeated horizontal jumps.
    const bool animAlreadyRunning = m_panCenterAnim &&
        m_panCenterAnim->state() != QAbstractAnimation::Stopped;

    if (!animAlreadyRunning) {
        // Record the start position so the stale-echo guard above can
        // recognise echo-backs that refer to the pre-animation center.
        m_panCenterStart = m_centerMhz;

        // Scroll waterfall history to align with the new center before the
        // animation begins.  Without this, old rows (at old center) and new
        // rows (at new center) are at different pixel positions, so signals
        // appear to jump vertically.  We do NOT reset m_wfWriteRow or clear
        // bins — the shift is small and new rows fill in naturally.
        reprojectWaterfall(m_centerMhz, m_bandwidthMhz, centerMhz, m_bandwidthMhz);
    }

    m_panCenterTarget = centerMhz;

    if (!m_panCenterAnim) {
        m_panCenterAnim = new QVariantAnimation(this);
        // InOutQuad: slow start → fast middle → slow end.
        // First frame moves ~1% of total distance so the widget eases in rather
        // than snapping, while still completing in a perceptually short time.
        m_panCenterAnim->setEasingCurve(QEasingCurve::InOutQuad);
        connect(m_panCenterAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) {
                m_centerMhz = v.toDouble();
                markOverlayDirty();
            });
        connect(m_panCenterAnim, &QVariantAnimation::finished, this,
            [this]() {
                m_centerMhz = m_panCenterTarget;  // land exactly on target
                markOverlayDirty();
            });
    }

    // Stop any in-progress animation toward a *different* target and restart
    // from the current visual position toward the new one.
    if (m_panCenterAnim->state() != QAbstractAnimation::Stopped) {
        m_panCenterAnim->stop();
    }
    m_panCenterAnim->setStartValue(m_centerMhz);
    m_panCenterAnim->setEndValue(centerMhz);
    m_panCenterAnim->setDuration(110);
    m_panCenterAnim->start();
}

void SpectrumWidget::setSpectrumFrac(float f)
{
    m_spectrumFrac = std::clamp(f, 0.10f, 0.90f);
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("SpectrumSplitRatio"), QString::number(m_spectrumFrac, 'f', 3));
    s.save();
    markOverlayDirty();
}

void SpectrumWidget::setDbmRange(float minDbm, float maxDbm)
{
    float ref = maxDbm;
    float dyn = maxDbm - minDbm;
    if (ref == m_refLevel && dyn == m_dynamicRange) return;
    m_refLevel     = ref;
    m_dynamicRange = dyn;
    markOverlayDirty();
}

// ─── Slice color table (shared via SliceColors.h) ────────────────────────────

static QColor sliceColor(int sliceId, bool active) {
    if (active) return SliceColorManager::instance().activeColor(sliceId);
    return SliceColorManager::instance().dimColor(sliceId);
}

// ─── Multi-slice overlay management ──────────────────────────────────────────

int SpectrumWidget::overlayIndex(int sliceId) const
{
    for (int i = 0; i < m_sliceOverlays.size(); ++i)
        if (m_sliceOverlays[i].sliceId == sliceId) return i;
    return -1;
}

const SpectrumWidget::SliceOverlay* SpectrumWidget::activeOverlay() const
{
    for (const auto& o : m_sliceOverlays)
        if (o.isActive) return &o;
    return m_sliceOverlays.isEmpty() ? nullptr : &m_sliceOverlays.first();
}

void SpectrumWidget::setSliceOverlay(int sliceId, double freq, int fLow, int fHigh,
                                     bool tx, bool active, const QString& mode,
                                     int rttyMark, int rttyShift,
                                     bool ritOn, int ritFreq,
                                     bool xitOn, int xitFreq)
{
    int idx = overlayIndex(sliceId);
    if (idx < 0) {
        SliceOverlay o;
        o.sliceId = sliceId; o.freqMhz = freq;
        o.filterLowHz = fLow; o.filterHighHz = fHigh;
        o.isTxSlice = tx; o.isActive = active;
        o.mode = mode; o.rttyMark = rttyMark; o.rttyShift = rttyShift;
        o.ritOn = ritOn; o.ritFreq = ritFreq;
        o.xitOn = xitOn; o.xitFreq = xitFreq;
        m_sliceOverlays.append(o);
        markOverlayDirty();
    } else {
        auto& o = m_sliceOverlays[idx];
        if (o.freqMhz == freq && o.filterLowHz == fLow && o.filterHighHz == fHigh &&
            o.isTxSlice == tx && o.isActive == active && o.mode == mode &&
            o.rttyMark == rttyMark && o.rttyShift == rttyShift &&
            o.ritOn == ritOn && o.ritFreq == ritFreq &&
            o.xitOn == xitOn && o.xitFreq == xitFreq)
            return;
        o.freqMhz = freq; o.filterLowHz = fLow; o.filterHighHz = fHigh;
        o.isTxSlice = tx; o.isActive = active;
        o.mode = mode; o.rttyMark = rttyMark; o.rttyShift = rttyShift;
        o.ritOn = ritOn; o.ritFreq = ritFreq;
        o.xitOn = xitOn; o.xitFreq = xitFreq;
        markOverlayDirty();
    }
}

void SpectrumWidget::setSliceOverlayMarkerStyle(int sliceId, int markerWidth, bool filterEdgesHidden)
{
    int idx = overlayIndex(sliceId);
    if (idx < 0) return;
    auto& o = m_sliceOverlays[idx];
    if (o.markerWidth == markerWidth && o.filterEdgesHidden == filterEdgesHidden) return;
    o.markerWidth = markerWidth;
    o.filterEdgesHidden = filterEdgesHidden;
    markOverlayDirty();
}

void SpectrumWidget::setSliceOverlayFreq(int sliceId, double freqMhz)
{
    for (auto& so : m_sliceOverlays) {
        if (so.sliceId == sliceId) {
            if (so.freqMhz == freqMhz) return;  // unchanged — no repaint needed
            so.freqMhz = freqMhz;
            markOverlayDirty();  // repaint so markers reflect the new frequency (#1272)
            return;
        }
    }
}

void SpectrumWidget::removeSliceOverlay(int sliceId)
{
    int idx = overlayIndex(sliceId);
    if (idx >= 0) m_sliceOverlays.remove(idx);
    markOverlayDirty();
}

void SpectrumWidget::setSplitPair(int rxSliceId, int txSliceId)
{
    // Clear old split markers
    for (auto& so : m_sliceOverlays)
        so.splitPartnerId = -1;

    if (rxSliceId >= 0 && txSliceId >= 0) {
        int rxIdx = overlayIndex(rxSliceId);
        int txIdx = overlayIndex(txSliceId);
        if (rxIdx >= 0) m_sliceOverlays[rxIdx].splitPartnerId = txSliceId;
        if (txIdx >= 0) m_sliceOverlays[txIdx].splitPartnerId = rxSliceId;
    }
    markOverlayDirty();
}

// ─── Legacy single-slice convenience wrappers ────────────────────────────────

void SpectrumWidget::setVfoFrequency(double freqMhz)
{
    auto* o = const_cast<SliceOverlay*>(activeOverlay());
    if (o) { o->freqMhz = freqMhz; markOverlayDirty(); }
}

void SpectrumWidget::setVfoFilter(int lowHz, int highHz)
{
    auto* o = const_cast<SliceOverlay*>(activeOverlay());
    if (o) { o->filterLowHz = lowHz; o->filterHighHz = highHz; markOverlayDirty(); }
}

void SpectrumWidget::setSliceInfo(int sliceId, bool isTxSlice)
{
    int idx = overlayIndex(sliceId);
    if (idx >= 0) { m_sliceOverlays[idx].isTxSlice = isTxSlice; markOverlayDirty(); }
}

void SpectrumWidget::updateSpectrum(const QVector<float>& binsDbm)
{
    // Forward to GPU renderer (#502)


    if (m_smoothed.size() != binsDbm.size())
        m_smoothed = binsDbm;
    else {
        for (int i = 0; i < binsDbm.size(); ++i)
            m_smoothed[i] = SMOOTH_ALPHA * binsDbm[i] + (1.0f - SMOOTH_ALPHA) * m_smoothed[i];
    }
    m_bins = binsDbm;

    // ── Live noise floor measurement (two-pass trimmed mean) ─────────────
    // Same technique as the waterfall auto-black: compute the mean of ALL bins,
    // then average only the bins at-or-below that mean.  Signal peaks inflate
    // the first-pass mean and therefore exclude themselves from the second pass,
    // leaving only the "consistently low, close-in-value" noise bins — exactly
    // the flat green line a human eye reads as the noise floor on the scope.
    // This is robust even on a very crowded band (40-50% bins occupied).
    if (!binsDbm.isEmpty()) {
        // Pass 1 — overall mean (sampled every 4th bin for speed)
        float sum = 0.0f;
        int   cnt = 0;
        for (int j = 0; j < binsDbm.size(); j += 4) { sum += binsDbm[j]; ++cnt; }
        const float mean = sum / cnt;

        // Pass 2 — average only noise bins (≤ mean), which excludes signal peaks
        float noiseSum = 0.0f;
        int   noiseCnt = 0;
        for (int j = 0; j < binsDbm.size(); j += 4) {
            if (binsDbm[j] <= mean) { noiseSum += binsDbm[j]; ++noiseCnt; }
        }
        const float frameFloor = (noiseCnt > 0) ? noiseSum / noiseCnt : mean;

        constexpr float kAlpha = 0.05f;  // ~20-frame window ≈ 0.8 s at 25 fps
        m_measuredNoiseFloorDbm = (m_measuredNoiseFloorDbm <= -500.0f)
            ? frameFloor
            : m_measuredNoiseFloorDbm * (1.0f - kAlpha) + frameFloor * kAlpha;
    }

    // Noise floor auto-adjust: every 10 frames, measure noise floor and
    // adjust min_dbm so it sits at the user's chosen position.
    if (m_noiseFloorEnable && !m_transmitting && !m_smoothed.isEmpty()) {
        if (++m_noiseFloorFrameCount >= 10) {
            m_noiseFloorFrameCount = 0;

            // Compute noise floor as 20th percentile of smoothed bins
            QVector<float> sorted = m_smoothed;
            std::sort(sorted.begin(), sorted.end());
            int idx = sorted.size() / 5;  // 20th percentile
            float noiseFloor = sorted[idx];

            // Position: 0 = noise at top, 100 = noise at bottom
            // noiseFloor should appear at (position/100) of the way down
            float frac = m_noiseFloorPosition / 100.0f;
            // noiseFloor maps to frac in the display:
            //   frac = (refLevel - noiseFloor) / dynamicRange
            //   => dynamicRange = (refLevel - noiseFloor) / frac
            // Keep refLevel (max_dbm) fixed, adjust min_dbm
            if (frac > 0.05f && frac < 0.95f) {
                float newRange = (m_refLevel - noiseFloor) / frac;
                newRange = std::clamp(newRange, 20.0f, 150.0f);
                float newMin = m_refLevel - newRange;
                // Only adjust if change is significant (> 1 dB)
                float currentMin = m_refLevel - m_dynamicRange;
                if (std::abs(newMin - currentMin) > 1.0f) {
                    // Optimistic update: suppress re-firing on subsequent FFT frames
                    // while waiting for the radio to confirm and echo the new range.
                    // Without this, every FFT frame would re-emit until the echo-back
                    // arrives, producing a burst of identical display pan set commands.
                    m_dynamicRange = newRange;
                    emit dbmRangeChangeRequested(newMin, m_refLevel);
                }
            }
        }
    }

    // Use FFT data for waterfall only when native tiles aren't available.
    // If native tiles stop arriving (e.g., disconnect), fall back after 2 seconds.
    // During TX: immediately use FFT-derived rows (radio stops sending tiles).
    // During RX: use native tiles, fall back to FFT after 2s timeout.
    if (m_transmitting) {
        // TX: push FFT-derived rows only if show-tx-in-waterfall is enabled,
        // or if this pan doesn't contain the TX slice (multi-pan: non-TX pan
        // keeps scrolling regardless).
        bool freeze = !m_showTxInWaterfall && m_hasTxSlice;
        if (!freeze && !m_waterfall.isNull())
            pushWaterfallRow(binsDbm, m_waterfall.width());
    } else {
        // Suppress post-TX transient noise rows (#2117).  The receiver AGC
        // needs ~400 ms to settle after TX→RX; discard waterfall data during
        // that window so the user doesn't see the bright transient stripe.
        bool postTxBlanking = false;
        if (m_txEndMs > 0) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_txEndMs < 400)
                postTxBlanking = true;
            else
                m_txEndMs = 0;
        }
        if (!postTxBlanking) {
            if (m_hasNativeWaterfall) {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - m_lastNativeTileMs > 2000) {
                    m_hasNativeWaterfall = false;
                    qDebug() << "SpectrumWidget: native waterfall tiles timed out, falling back to FFT-derived";
                }
            }
            if (!m_hasNativeWaterfall && !m_waterfall.isNull())
                pushWaterfallRow(binsDbm, m_waterfall.width());
        }
    }

    update();
}

void SpectrumWidget::updateWaterfallRow(const QVector<float>& binsIntensity,
                                        double lowFreqMhz, double highFreqMhz,
                                        quint32 timecode)
{
    // Native waterfall tiles carry intensity values (int16/128.0f, ~96-120 on HF).
    if (binsIntensity.isEmpty()) return;

    // Forward to GPU renderer (#502)


    if (m_waterfall.isNull()) return;

    // Freeze waterfall during TX if show-tx-in-waterfall is off and this pan
    // contains the TX slice. Non-TX pans keep scrolling in multi-pan.
    if (m_transmitting && !m_showTxInWaterfall && m_hasTxSlice) return;

    // Suppress post-TX transient noise rows (#2117). The receiver AGC needs
    // ~400 ms to settle after TX→RX; discard waterfall data during that
    // window.
    if (m_txEndMs > 0) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_txEndMs < 400) return;
        m_txEndMs = 0;
    }

    // Derive ms-per-row by measuring wall-clock / timecode delta.
    // Collect data for the first 50 tiles to converge, then lock the value.
    // Only re-measure when the user changes the waterfall rate slider
    // (which calls resetWfTimeScale()).
    if (!m_wfTimeScaleLocked) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (timecode > 0 && m_wfPrevTimecode > 0 && timecode > m_wfPrevTimecode && m_wfPrevTimecodeMs > 0) {
            const float wallDelta = static_cast<float>(now - m_wfPrevTimecodeMs);
            const float tcDelta = static_cast<float>(timecode - m_wfPrevTimecode);
            if (tcDelta > 0 && wallDelta > 0) {
                const float measured = wallDelta / tcDelta;
                m_wfMsPerRow = 0.9f * m_wfMsPerRow + 0.1f * measured;
                if (++m_wfCalibrationCount >= 50) {
                    m_wfTimeScaleLocked = true;  // lock — no more jitter
                }
            }
        }
        if (timecode > 0) {
            m_wfPrevTimecode = timecode;
            m_wfPrevTimecodeMs = now;
        }
    }

    // Client-side auto-black: track the noise floor from tile data and adjust
    // the black threshold to sit just above it. This replaces the radio's
    // auto_black which targets SmartSDR's different rendering engine.
    if (m_wfAutoBlack && !m_transmitting) {
        // Estimate noise floor from incoming tiles using a two-pass trimmed mean.
        // Freeze during TX; threshold is restored to pre-TX value on TX→RX transition.
        // Pass 1: compute overall mean (sampled every 8th bin for speed).
        float sum = 0;
        int count = 0;
        for (int i = 0; i < binsIntensity.size(); i += 8) {
            sum += binsIntensity[i];
            count++;
        }
        if (count > 0) {
            const float mean = sum / count;
            // Pass 2: mean of bins at or below overall mean — filters out
            // strong signals that would pull the noise floor estimate upward.
            float noiseSum = 0;
            int noiseCount = 0;
            for (int i = 0; i < binsIntensity.size(); i += 8) {
                if (binsIntensity[i] <= mean) {
                    noiseSum += binsIntensity[i];
                    noiseCount++;
                }
            }
            const float noiseFloor = (noiseCount > 0) ? (noiseSum / noiseCount) : mean;
            // Use noise floor directly as threshold — noise maps to black,
            // signals above stand out with better contrast.
            const float target = noiseFloor;
            // Smooth to prevent jitter
            m_autoBlackThresh = 0.95f * m_autoBlackThresh + 0.05f * target;
        }
    }

    // One pixel row per tile — scroll speed determined by tile arrival rate.
    int rowsToPush = 1;

    m_hasNativeWaterfall = true;
    m_lastNativeTileMs = QDateTime::currentMSecsSinceEpoch();

    const int destWidth = m_waterfall.width();
    if (destWidth <= 0) return;

    const int h = m_waterfall.height();
    if (h <= 1) return;
    rowsToPush = std::min(rowsToPush, h - 1);

    // Render the tile row into a temporary scanline.
    // Per FlexRadio community guidance: tiles extend BEYOND the panadapter edges.
    // For each display pixel, calculate its frequency, then find the corresponding
    // tile bin via: binIdx = (freq - tileLowFreq) / binBandwidth.
    const int srcSize = binsIntensity.size();
    const double tileBw = (srcSize > 0) ? (highFreqMhz - lowFreqMhz) / srcSize : 0.0;
    const double panStartMhz = m_centerMhz - m_bandwidthMhz / 2.0;

    QVector<QRgb> scanline(destWidth, qRgb(0, 0, 0));
    if (tileBw > 0) {
        for (int x = 0; x < destWidth; ++x) {
            const double freq = panStartMhz + (static_cast<double>(x) / destWidth) * m_bandwidthMhz;
            const double binF = (freq - lowFreqMhz) / tileBw;
            const int binIdx = static_cast<int>(binF);
            if (binIdx >= 0 && binIdx < srcSize) {
                // Linear interpolation between adjacent bins
                const float frac = static_cast<float>(binF - binIdx);
                const float i0 = binsIntensity[binIdx];
                const float i1 = (binIdx + 1 < srcSize) ? binsIntensity[binIdx + 1] : i0;
                scanline[x] = intensityToRgb(i0 + frac * (i1 - i0));
            }
        }
    }

    // NB Waterfall Blanker (#277) — suppress impulse rows
    if (m_wfBlankerEnabled) {
        float rowSum = 0.0f;
        const int binCount = binsIntensity.size();
        for (int i = 0; i < binCount; ++i)
            rowSum += binsIntensity[i];
        const float rowMean = (binCount > 0) ? (rowSum / binCount) : 0.0f;

        // Compute rolling baseline
        float baseline = 0.0f;
        for (int i = 0; i < m_wfBlankerRingCount; ++i)
            baseline += m_wfBlankerRing[i];
        if (m_wfBlankerRingCount > 0)
            baseline /= m_wfBlankerRingCount;

        // Detect impulse (need ≥8 rows of history)
        if (m_wfBlankerRingCount >= 8 && baseline > 0.0f
                && rowMean > baseline * m_wfBlankerThreshold) {
            // Impulse detected — replace with last good row (interpolate)
            if (m_wfLastGoodRow.size() == destWidth) {
                scanline = m_wfLastGoodRow;
            } else {
                // No previous good row yet — fill with noise floor color
                const QRgb floorColor = intensityToRgb(baseline);
                std::fill(scanline.begin(), scanline.end(), floorColor);
            }
            m_wfBlankerRing[m_wfBlankerRingIdx] = std::min(rowMean, baseline * 1.05f);
        } else {
            m_wfLastGoodRow = scanline;
            m_wfBlankerRing[m_wfBlankerRingIdx] = rowMean;
        }
        m_wfBlankerRingIdx = (m_wfBlankerRingIdx + 1) % WF_BLANKER_N;
        if (m_wfBlankerRingCount < WF_BLANKER_N)
            ++m_wfBlankerRingCount;
    }

    // Write rows into history + visible viewport.
    const bool canInterp = (m_prevTileScanline.size() == destWidth && rowsToPush > 1);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (int r = 0; r < rowsToPush; ++r) {
        QVector<QRgb> interpolatedRow(destWidth, qRgb(0, 0, 0));
        if (canInterp) {
            // t=0 at row 0 (current), t=1 at last row (previous)
            const float t = static_cast<float>(r) / rowsToPush;
            for (int x = 0; x < destWidth; ++x) {
                const QRgb c = scanline[x];
                const QRgb p = m_prevTileScanline[x];
                const int cr = qRed(c)   + static_cast<int>(t * (qRed(p)   - qRed(c)));
                const int cg = qGreen(c) + static_cast<int>(t * (qGreen(p) - qGreen(c)));
                const int cb = qBlue(c)  + static_cast<int>(t * (qBlue(p)  - qBlue(c)));
                interpolatedRow[x] = qRgb(cr, cg, cb);
            }
        } else {
            interpolatedRow = scanline;
        }

        appendHistoryRow(interpolatedRow.constData(), nowMs);
        if (m_wfLive) {
            appendVisibleRow(interpolatedRow.constData());
        } else {
            rebuildWaterfallViewport();
        }
    }
    m_prevTileScanline = scanline;

    update();
}

// ─── Layout helpers ────────────────────────────────────────────────────────────

int SpectrumWidget::mhzToX(double mhz) const
{
    if (m_bandwidthMhz <= 0.0) return -1;
    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double px = (mhz - startMhz) / m_bandwidthMhz * width();
    if (std::isnan(px) || std::isinf(px)) return -1;
    // Round to nearest pixel so all vertical markers (VFO, TNF, filter edges) are
    // centred on the true frequency.  Truncation caused ±1 px jitter during
    // continuous tuning because the same frequency mapped to different pixels
    // depending on floating-point remainder (#1272, also fixes cursor snap #1369).
    return static_cast<int>(std::round(std::clamp(px, -1.0e6, 1.0e6)));
}

double SpectrumWidget::xToMhz(int x) const
{
    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    return startMhz + (static_cast<double>(x) / width()) * m_bandwidthMhz;
}

void SpectrumWidget::updateTrackedCursorState(const QPoint& localPos, bool insideWidget)
{
    const QPoint oldCursorPos = m_cursorPos;
    const int oldHoveredTnfId = m_hoveredTnfId;
    const bool oldTuneGuideVisible = m_tuneGuideVisible;

    if (!insideWidget) {
        m_cursorPos = {-1, -1};
        m_hoveredTnfId = -1;
        m_tuneGuideVisible = false;
        m_tuneGuideTimer->stop();
        QToolTip::hideText();
    } else {
        const int chromeH = FREQ_SCALE_H + DIVIDER_H;
        const int contentH = height() - chromeH;
        const int specH = static_cast<int>(contentH * m_spectrumFrac);
        const bool inSpectrum = localPos.y() >= 0
            && localPos.y() < specH
            && localPos.x() >= 0
            && localPos.x() < width();
        const int preferredTnfId = (m_draggingTnfId >= 0) ? m_draggingTnfId : m_hoveredTnfId;

        m_cursorPos = localPos;
        m_hoveredTnfId = inSpectrum ? tnfAtPixel(localPos.x(), preferredTnfId) : -1;

        if (m_showTuneGuides && m_hoveredTnfId < 0) {
            m_tuneGuideVisible = true;
            m_tuneGuideTimer->start();
        } else {
            m_tuneGuideVisible = false;
            m_tuneGuideTimer->stop();
        }

        if (m_hoveredTnfId >= 0) {
            QToolTip::hideText();
        }
    }

    if (m_cursorPos != oldCursorPos
        || m_hoveredTnfId != oldHoveredTnfId
        || m_tuneGuideVisible != oldTuneGuideVisible) {
        markOverlayDirty();
    }
    updateTnfHoverPopup();
}

void SpectrumWidget::setSpectrumCursor(Qt::CursorShape shape)
{
    // Avoid redundant cursor installs on every hover mouse-move. On macOS, even
    // standard Qt cursor changes can pass through QImage::toCGImage() in the
    // Cocoa platform plugin; issue #2458 crashed in that path while dispatching
    // enter/leave events.
    if (cursor().shape() == shape) {
        return;
    }
    setCursor(shape);
}

// ─── Mouse ────────────────────────────────────────────────────────────────────

// Snap a frequency (MHz) to the nearest multiple of m_stepHz.
static double snapToStep(double mhz, int stepHz)
{
    if (stepHz <= 0) return mhz;
    const double stepMhz = stepHz / 1e6;
    return std::round(mhz / stepMhz) * stepMhz;
}

void SpectrumWidget::mousePressEvent(QMouseEvent* ev)
{
    const int chromeH  = FREQ_SCALE_H + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int y = static_cast<int>(ev->position().y());

    // Save press position for single-click-to-tune drag threshold
    if (ev->button() == Qt::LeftButton)
        m_clickPressPos = ev->position().toPoint();

    // Click on prop forecast overlay → open dashboard
    if (ev->button() == Qt::LeftButton && !m_propClickRect.isNull()) {
        const QPoint pos(static_cast<int>(ev->position().x()), y);
        if (m_propClickRect.contains(pos)) {
            emit propForecastClicked();
            m_spotClickConsumed = true;   // suppress release-to-tune (#1647)
            ev->accept();
            return;
        }
    }

    // Click on a spot label → tune to that frequency
    if (m_showSpots && ev->button() == Qt::LeftButton) {
        const QPoint pos(static_cast<int>(ev->position().x()), y);
        for (const auto& hr : m_spotClickRects) {
            if (hr.rect.contains(pos)) {
                if (hr.markerIndex >= 0 && hr.markerIndex < m_spotMarkers.size()) {
                    const auto& marker = m_spotMarkers[hr.markerIndex];
                    if (marker.source == "Memory") {
                        emit spotTriggered(marker.index);
                    } else {
                        emit frequencyClicked(hr.freqMhz);
                        // Notify the radio that a spot was clicked (#341)
                        emit spotTriggered(marker.index);
                    }
                } else {
                    emit frequencyClicked(hr.freqMhz);
                }
                m_spotClickConsumed = true;  // suppress release-to-tune (#530)
                ev->accept();
                return;
            }
        }
        // Click on a cluster badge → show popup with collapsed spots
        for (const auto& cluster : m_spotClusters) {
            if (cluster.rect.contains(pos)) {
                showSpotClusterPopup(cluster, mapToGlobal(pos));
                ev->accept();
                return;
            }
        }
    }

    // Click on the divider bar → start split drag
    if (y >= specH && y < specH + DIVIDER_H) {
        m_draggingDivider = true;
        setSpectrumCursor(Qt::SplitVCursor);
        ev->accept();
        return;
    }

    // Click on the freq scale bar → start bandwidth drag
    const int scaleY = specH + DIVIDER_H;
    if (y >= scaleY && y < scaleY + FREQ_SCALE_H) {
        const QRect wfRect(0, scaleY + FREQ_SCALE_H, width(), height() - (scaleY + FREQ_SCALE_H));
        if (waterfallLiveButtonRect(wfRect).contains(ev->position().toPoint())) {
            setWaterfallLive(true);
            ev->accept();
            return;
        }

        m_draggingBandwidth = true;
        m_bwDragStartX = static_cast<int>(ev->position().x());
        m_bwDragStartBw = m_bandwidthMhz;
        const double mouseXFrac = ev->position().x() / width() - 0.5;
        m_bwDragAnchorMhz = m_centerMhz + mouseXFrac * m_bandwidthMhz;
        setSpectrumCursor(Qt::SizeHorCursor);
        ev->accept();
        return;
    }

    // Left-click in waterfall area → start pan drag (tune on double-click only)
    const int wfY = scaleY + FREQ_SCALE_H;
    if (y >= wfY && ev->button() == Qt::LeftButton) {
        const QRect wfRect(0, wfY, width(), height() - wfY);
        const QRect timeScaleRect = waterfallTimeScaleRect(wfRect);
        const QPoint pos = ev->position().toPoint();

        if (timeScaleRect.contains(pos)) {
            m_draggingTimeScale = true;
            m_timeScaleDragStartY = y;
            m_timeScaleDragStartOffsetRows = m_wfHistoryOffsetRows;
            setSpectrumCursor(Qt::SizeVerCursor);
            ev->accept();
            return;
        }

        m_draggingPan = true;
        m_panDragStartX = static_cast<int>(ev->position().x());
        m_panDragStartCenter = m_centerMhz;
        setSpectrumCursor(Qt::ClosedHandCursor);
        ev->accept();
        return;
    }

    // Left-click on off-screen slice indicator → absorb or switch slice
    if (ev->button() == Qt::LeftButton) {
        for (int oi = 0; oi < m_offScreenRects.size(); ++oi) {
            if (!m_offScreenRects[oi].isNull() &&
                m_offScreenRects[oi].contains(QPoint(static_cast<int>(ev->position().x()), y))) {
                const auto& so = m_sliceOverlays[oi];
                if (!so.isActive) emit sliceClicked(so.sliceId);
                m_spotClickConsumed = true;   // suppress release-to-tune (#1772)
                ev->accept();
                return;
            }
        }
    }

    // Check for click on dBm scale strip (right edge of FFT area)
    if (y < specH) {
        const int mx = static_cast<int>(ev->position().x());
        const int stripX = width() - DBM_STRIP_W;

        if (mx >= stripX) {
            // Arrow row (side by side: left = up, right = down)
            if (y < DBM_ARROW_H) {
                const float bottom = m_refLevel - m_dynamicRange;
                if (mx < stripX + DBM_STRIP_W / 2) {
                    // Up arrow: raise ref level by 10 dB, keep bottom fixed
                    m_refLevel += 10.0f;
                } else {
                    // Down arrow: lower ref level by 10 dB, keep bottom fixed
                    m_refLevel -= 10.0f;
                }
                m_dynamicRange = m_refLevel - bottom;
                if (m_dynamicRange < 10.0f) {
                    m_dynamicRange = 10.0f;
                    m_refLevel = bottom + m_dynamicRange;
                }
                markOverlayDirty();
                emit dbmRangeChangeRequested(bottom, m_refLevel);
                ev->accept();
                return;
            }
            // Below arrows: start dBm drag (pan reference)
            m_draggingDbm = true;
            m_dbmDragStartY = y;
            m_dbmDragStartRef = m_refLevel;
            setSpectrumCursor(Qt::SizeVerCursor);
            ev->accept();
            return;
        }
    }

    // Right-click context menu on spectrum or waterfall (cancel any active TNF drag first)
    if (ev->button() == Qt::RightButton) {
        m_draggingTnfId = -1;
        const int mx = static_cast<int>(ev->position().x());

        // Right-click on off-screen slice indicator → slice context menu
        for (int oi = 0; oi < m_offScreenRects.size(); ++oi) {
            if (!m_offScreenRects[oi].isNull() &&
                m_offScreenRects[oi].contains(QPoint(mx, y))) {
                const auto& so = m_sliceOverlays[oi];
                const QChar letter = QChar('A' + (so.sliceId % kSliceColorCount));
                QMenu menu(this);
                menu.addAction(QString("Close Slice %1").arg(letter), this,
                    [this, id = so.sliceId]{ emit sliceCloseRequested(id); });
                menu.addAction(QString("Move Slice %1 Here").arg(letter), this,
                    [this, id = so.sliceId]{ emit sliceTuneRequested(id, m_centerMhz); });
                menu.addAction(QString("Center Slice %1").arg(letter), this,
                    [this, freq = so.freqMhz]{
                        m_centerMhz = freq;
                        markOverlayDirty();
                        emit centerChangeRequested(m_centerMhz);
                    });
                menu.exec(ev->globalPosition().toPoint());
                ev->accept();
                return;
            }
        }

        const double freqMhz = xToMhz(mx);
        const int hitTnf = tnfAtPixel(mx);

        // Check if right-click is on an existing spot label
        int hitSpotIdx = -1;
        QString hitSpotCall;
        double hitSpotFreq = 0;
        QString hitSpotSource;
        for (const auto& hr : m_spotClickRects) {
            if (hr.rect.contains(mx, static_cast<int>(ev->position().y()))) {
                if (hr.markerIndex >= 0 && hr.markerIndex < m_spotMarkers.size()) {
                    const auto& sm = m_spotMarkers[hr.markerIndex];
                    hitSpotIdx = sm.index;
                    hitSpotCall = sm.callsign;
                    hitSpotFreq = sm.freqMhz;
                    hitSpotSource = sm.source;
                }
                break;
            }
        }

        QMenu menu(this);

        // Spot-on-label context menu
        if (hitSpotIdx >= 0) {
            if (hitSpotSource == "Memory") {
                const QString title = hitSpotCall.isEmpty()
                    ? QStringLiteral("Apply Memory")
                    : QString("Apply %1").arg(hitSpotCall);
                menu.addAction(title, this, [this, hitSpotIdx]{
                    emit spotTriggered(hitSpotIdx);
                });
            } else {
                menu.addAction(QString("Tune to %1").arg(hitSpotCall), this,
                    [this, hitSpotFreq]{ emit frequencyClicked(hitSpotFreq); });
                menu.addAction("Copy Callsign", this, [hitSpotCall]{
                    QApplication::clipboard()->setText(hitSpotCall);
                });
                menu.addAction("Lookup on QRZ", this, [hitSpotCall]{
                    QDesktopServices::openUrl(QUrl("https://www.qrz.com/db/" + hitSpotCall));
                });
                menu.addSeparator();
                menu.addAction("Remove Spot", this,
                    [this, hitSpotIdx]{ emit spotRemoveRequested(hitSpotIdx); });
            }
        }
        // TNF context menu (when clicking on a TNF marker)
        else if (hitTnf >= 0) {
            const TnfMarker* tnf = tnfMarkerById(hitTnf);
            if (tnf) {
                auto* infoAction = new QWidgetAction(&menu);
                auto* infoWidget = new QWidget(&menu);
                auto* infoLayout = new QVBoxLayout(infoWidget);
                infoLayout->setContentsMargins(10, 6, 10, 6);
                infoLayout->setSpacing(2);

                auto* freqLabel = new QLabel(QString("%1 MHz").arg(formatFlagFrequency(tnf->freqMhz)), infoWidget);
                auto* widthLabel = new QLabel(QString("Width: %1 Hz").arg(tnf->widthHz), infoWidget);
                auto* separator = new QFrame(infoWidget);
                separator->setFrameShape(QFrame::HLine);
                separator->setFrameShadow(QFrame::Plain);
                separator->setStyleSheet("color: rgba(90, 110, 130, 180);");

                infoWidget->setStyleSheet(
                    "background: rgba(40, 48, 58, 190);"
                    "color: rgba(200, 216, 232, 170);");
                infoLayout->addWidget(freqLabel);
                infoLayout->addWidget(widthLabel);
                infoLayout->addSpacing(4);
                infoLayout->addWidget(separator);
                // Prevent the info header from closing the menu on click
                infoWidget->setAttribute(Qt::WA_TransparentForMouseEvents);
                infoAction->setEnabled(false);
                infoAction->setDefaultWidget(infoWidget);
                menu.addAction(infoAction);
            }
            menu.addAction("Remove TNF", this, [this, hitTnf]{ emit tnfRemoveRequested(hitTnf); });
            auto* widthMenu = menu.addMenu("Width");
            for (int w : {50, 100, 200, 500}) {
                QAction* action = widthMenu->addAction(QString("%1 Hz").arg(w), this,
                    [this, hitTnf, w]{ emit tnfWidthRequested(hitTnf, w); });
                action->setCheckable(true);
                action->setChecked(tnf && tnf->widthHz == w);
            }
            auto* depthMenu = menu.addMenu("Depth");
            const int currentDepth = tnf ? tnf->depthDb : 1;
            for (const auto& option : std::initializer_list<std::pair<const char*, int>>{
                     {"Normal", 1}, {"Deep", 2}, {"Very Deep", 3}}) {
                QAction* action = depthMenu->addAction(option.first, this,
                    [this, hitTnf, depth = option.second]{ emit tnfDepthRequested(hitTnf, depth); });
                action->setCheckable(true);
                action->setChecked(currentDepth == option.second);
            }
            menu.addSeparator();
            bool isPerm = false;
            for (const auto& t : m_tnfMarkers)
                if (t.id == hitTnf) { isPerm = t.permanent; break; }
            if (isPerm)
                menu.addAction("Make Temporary", this, [this, hitTnf]{ emit tnfPermanentRequested(hitTnf, false); });
            else
                menu.addAction("Make Permanent", this, [this, hitTnf]{ emit tnfPermanentRequested(hitTnf, true); });
        }
        // General area context menu
        else {
            // Snap frequency to step size for spot placement
            double snappedMhz = freqMhz;
            if (m_stepHz > 0) {
                const double stepMhz = m_stepHz / 1e6;
                snappedMhz = std::round(freqMhz / stepMhz) * stepMhz;
            }
            const QString freqStr = QString::number(snappedMhz, 'f', 6);
            menu.addAction(QString("Add Spot at %1 MHz...").arg(freqStr), this,
                [this, snappedMhz]{ showAddSpotDialog(snappedMhz); });
            menu.addAction(QString("Add TNF at %1 MHz").arg(freqStr), this,
                [this, freqMhz]{ emit tnfCreateRequested(freqMhz); });
        }

        if (hitTnf < 0) {
            // Close Slice option (only when multiple slices exist)
            if (m_sliceOverlays.size() > 1) {
                menu.addSeparator();
                int closestSlice = -1;
                int closestDist = INT_MAX;
                for (const auto& so : m_sliceOverlays) {
                    int dist = std::abs(mx - mhzToX(so.freqMhz));
                    if (dist < closestDist) { closestDist = dist; closestSlice = so.sliceId; }
                }
                if (closestSlice >= 0) {
                    const QChar letter = QChar('A' + (closestSlice & 3));
                    menu.addAction(QString("Close Slice %1").arg(letter), this,
                        [this, closestSlice]{ emit sliceCloseRequested(closestSlice); });
                }
            }

            menu.addSeparator();
            QAction* tuneGuideAction = menu.addAction("Show Tune Guides");
            tuneGuideAction->setCheckable(true);
            tuneGuideAction->setChecked(m_showTuneGuides);
            connect(tuneGuideAction, &QAction::toggled, this, &SpectrumWidget::setShowTuneGuides);

            QAction* extendedLineAction = menu.addAction("Extended Frequency Line");
            extendedLineAction->setCheckable(true);
            extendedLineAction->setChecked(m_extendedFrequencyLine);
            connect(extendedLineAction, &QAction::toggled, this, &SpectrumWidget::setExtendedFrequencyLine);

            menu.addSeparator();
            bool floating = m_isFloating;
            QAction* popOutAction = menu.addAction(floating ? "\u21a9 Dock" : "\u2197 Pop out");
            connect(popOutAction, &QAction::triggered, this, [this, floating]() {
                emit popOutRequested(!floating);
            });
        }

        menu.exec(ev->globalPosition().toPoint());
        ev->accept();
        return;
    }

    // Check for click on TNF marker in FFT area → start drag
    if (ev->button() == Qt::LeftButton && y < specH) {
        const int mx = static_cast<int>(ev->position().x());
        const int hitTnf = tnfAtPixel(mx);
        if (hitTnf >= 0) {
            m_draggingTnfId = hitTnf;
            m_tnfDragStartPos = ev->position().toPoint();
            for (const auto& t : m_tnfMarkers) {
                if (t.id == hitTnf) {
                    m_dragTnfOrigWidthHz = t.widthHz;
                    m_dragTnfLastFreq = t.freqMhz;
                    m_dragTnfLastWidthHz = t.widthHz;
                    break;
                }
            }
            setSpectrumCursor(Qt::SizeAllCursor);
            ev->accept();
            return;
        }
    }

    // Check for click on an inactive slice overlay — switch active so the next
    // interaction targets the clicked slice's passband/marker.
    if (y < specH) {
        const int mx = static_cast<int>(ev->position().x());
        for (const auto& so : m_sliceOverlays) {
            if (so.isActive) continue;
            const int sliceX = mhzToX(so.freqMhz);
            const int loX = mhzToX(so.freqMhz + so.filterLowHz / 1.0e6);
            const int hiX = mhzToX(so.freqMhz + so.filterHighHz / 1.0e6);
            const int left = std::min(loX, hiX);
            const int right = std::max(loX, hiX);
            // Slice badge area in top 25px
            if (mx >= sliceX - 8 && mx <= sliceX + 35 && y <= 25) {
                emit sliceClicked(so.sliceId);
                ev->accept();
                return;
            }
            // Center line anywhere vertically
            if (std::abs(mx - sliceX) <= 8) {
                emit sliceClicked(so.sliceId);
                ev->accept();
                return;
            }
            // Filter passband body anywhere in the FFT area: activate and
            // immediately enter VFO drag so the first click-drag retunes.
            if (mx >= left && mx <= right) {
                emit sliceClicked(so.sliceId);
                m_draggingVfo = true;
                m_vfoDragOffsetHz = static_cast<int>(
                    std::round((xToMhz(mx) - so.freqMhz) * 1.0e6));
                setSpectrumCursor(Qt::SizeHorCursor);
                ev->accept();
                return;
            }
        }
    }

    // Check for click on filter edges in FFT area (8px grab zone — bumped
    // from 5px, the 5px target was too easy to miss especially when the
    // edge sits near the VFO line. #2259)
    if (y < specH) {
        const auto* ao = activeOverlay();
        if (!ao) { ev->accept(); return; }
        const int mx = static_cast<int>(ev->position().x());
        const int loX = mhzToX(ao->freqMhz + ao->filterLowHz / 1.0e6);
        const int hiX = mhzToX(ao->freqMhz + ao->filterHighHz / 1.0e6);
        constexpr int GRAB = 8;

        const bool loHit = std::abs(mx - loX) <= GRAB;
        const bool hiHit = std::abs(mx - hiX) <= GRAB;
        if (loHit || hiHit) {
            // When both edges are within grab range, pick the closer one (#764)
            if (loHit && hiHit)
                m_draggingFilter = (std::abs(mx - loX) <= std::abs(mx - hiX))
                    ? FilterEdge::Low : FilterEdge::High;
            else
                m_draggingFilter = loHit ? FilterEdge::Low : FilterEdge::High;

            // Store anchor offset so the edge doesn't snap to cursor (#764)
            const int edgeHz = (m_draggingFilter == FilterEdge::Low) ? ao->filterLowHz : ao->filterHighHz;
            m_filterDragStartX = mx;
            m_filterDragStartHz = edgeHz;

            setSpectrumCursor(Qt::SizeHorCursor);
            ev->accept();
            return;
        }

        // Click inside the filter passband → start VFO drag (#404)
        const int left = std::min(loX, hiX);
        const int right = std::max(loX, hiX);
        if (mx > left + GRAB && mx < right - GRAB) {
            m_draggingVfo = true;
            m_vfoDragOffsetHz = static_cast<int>(std::round((xToMhz(mx) - ao->freqMhz) * 1.0e6));
            setSpectrumCursor(Qt::SizeHorCursor);
            ev->accept();
            return;
        }
    }

    // Click in FFT area → start pan drag (tune on double-click only)
    m_draggingPan = true;
    m_panDragStartX = static_cast<int>(ev->position().x());
    m_panDragStartCenter = m_centerMhz;
    setSpectrumCursor(Qt::ClosedHandCursor);
    ev->accept();
}

void SpectrumWidget::mouseMoveEvent(QMouseEvent* ev)
{
    const int chromeH  = FREQ_SCALE_H + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int y = static_cast<int>(ev->position().y());
    const int mx = static_cast<int>(ev->position().x());

    // TNF drag
    if (m_draggingTnfId >= 0) {
        const double newFreq = xToMhz(mx);
        const int dy = static_cast<int>(ev->position().y()) - m_tnfDragStartPos.y();
        const double widthScale = std::pow(2.0, static_cast<double>(-dy) / 48.0);
        const int newWidthHz = std::clamp(
            static_cast<int>(std::lround(static_cast<double>(m_dragTnfOrigWidthHz) * widthScale)),
            10, 12000);
        for (auto& t : m_tnfMarkers) {
            if (t.id == m_draggingTnfId) {
                t.freqMhz = newFreq;
                t.widthHz = newWidthHz;
                break;
            }
        }
        if (!qFuzzyCompare(newFreq + 1.0, m_dragTnfLastFreq + 1.0)) {
            m_dragTnfLastFreq = newFreq;
            emit tnfMoveRequested(m_draggingTnfId, newFreq);
        }
        if (newWidthHz != m_dragTnfLastWidthHz) {
            m_dragTnfLastWidthHz = newWidthHz;
            emit tnfWidthRequested(m_draggingTnfId, newWidthHz);
        }
        m_hoveredTnfId = m_draggingTnfId;
        m_tuneGuideVisible = false;
        m_tuneGuideTimer->stop();
        m_cursorPos = ev->position().toPoint();
        updateTnfHoverPopup();
        markOverlayDirty();
        ev->accept();
        return;
    }

    if (m_draggingDivider) {
        // Clamp the divider position: 10%–90% of content area
        float frac = static_cast<float>(y) / contentH;
        m_spectrumFrac = std::clamp(frac, 0.10f, 0.90f);
        // Rebuild waterfall image for new size
        const int wfHeight = static_cast<int>(contentH * (1.0f - m_spectrumFrac));
        if (wfHeight > 0 && width() > 0) {
            QImage newWf(width(), wfHeight, QImage::Format_RGB32);
            newWf.fill(Qt::black);
            if (!m_waterfall.isNull()) {
                QImage scaled = m_waterfall.scaled(width(), wfHeight, Qt::IgnoreAspectRatio, Qt::FastTransformation);
                if (!scaled.isNull())
                    newWf = std::move(scaled);
            }
            m_waterfall = std::move(newWf);
            m_wfWriteRow = 0;
            ensureWaterfallHistory();
            if (m_wfHistoryRowCount > 0) {
                rebuildWaterfallViewport();
            }
        }
        markOverlayDirty();
        ev->accept();
        return;
    }

    if (m_draggingDbm) {
        const int dy = y - m_dbmDragStartY;
        // Convert pixel drag to dB: full FFT height = full dynamic range
        const float deltaDb = (static_cast<float>(dy) / specH) * m_dynamicRange;
        m_refLevel = m_dbmDragStartRef + deltaDb;
        markOverlayDirty();
        emit dbmRangeChangeRequested(m_refLevel - m_dynamicRange, m_refLevel);
        ev->accept();
        return;
    }

    if (m_draggingTimeScale) {
        const int wfY = specH + DIVIDER_H + FREQ_SCALE_H;
        const QRect wfRect(0, wfY, width(), height() - wfY);
        const QRect timeScaleRect = waterfallTimeScaleRect(wfRect);
        const int dragHeight = std::max(1, timeScaleRect.height());
        const int maxOffset = maxWaterfallHistoryOffsetRows();
        const int dy = m_timeScaleDragStartY - y;  // pull up = scroll back in time
        const int deltaRows = (maxOffset > 0)
            ? static_cast<int>(std::round((static_cast<double>(dy) / dragHeight) * maxOffset))
            : 0;
        const int newOffset = std::clamp(m_timeScaleDragStartOffsetRows + deltaRows, 0, maxOffset);

        if (newOffset != m_wfHistoryOffsetRows) {
            m_wfHistoryOffsetRows = newOffset;
            if (newOffset > 0) {
                m_wfLive = false;
            }
            rebuildWaterfallViewport();
            markOverlayDirty();
        }

        setSpectrumCursor(Qt::SizeVerCursor);
        ev->accept();
        return;
    }

    if (m_draggingBandwidth) {
        const int dx = static_cast<int>(ev->position().x()) - m_bwDragStartX;
        // 4x multiplier: dragging 1/4 of widget width doubles/halves bandwidth
        const double scale = std::pow(2.0, static_cast<double>(-dx) / (width() / 4.0));
        const double newBw = std::clamp(m_bwDragStartBw * scale, m_minBwMhz, m_maxBwMhz);
        const double mouseXFrac = static_cast<double>(m_bwDragStartX) / width() - 0.5;
        const double zoomCenter = m_bwDragAnchorMhz - mouseXFrac * newBw;
        reprojectWaterfall(m_centerMhz, m_bandwidthMhz, zoomCenter, newBw);
        if (!reprojectSpectrum(m_centerMhz, m_bandwidthMhz, zoomCenter, newBw)) {
            m_bins.clear();
            m_smoothed.clear();
        }
        m_bandwidthMhz = newBw;
        m_centerMhz = zoomCenter;
        markOverlayDirty();
        // Keep center and bandwidth coupled while dragging. Sending only the
        // bandwidth and waiting to send center on release caused the radio and
        // client waterfall to diverge under trackpad-heavy zoom workflows.
        emit frequencyRangeChangeRequested(zoomCenter, newBw);
        ev->accept();
        return;
    }

    if (m_draggingFilter != FilterEdge::None) {
        auto* ao = const_cast<SliceOverlay*>(activeOverlay());
        if (!ao) { m_draggingFilter = FilterEdge::None; return; }
        const int mx = static_cast<int>(ev->position().x());
        // Compute Hz delta from pixel delta — immune to freq/overlay changes (#764)
        const double hzPerPx = (m_bandwidthMhz * 1.0e6) / width();
        int hz = m_filterDragStartHz + static_cast<int>(std::round((mx - m_filterDragStartX) * hzPerPx));

        if (m_draggingFilter == FilterEdge::Low) {
            ao->filterLowHz = hz;
        } else {
            ao->filterHighHz = hz;
        }
        markOverlayDirty();
        emit filterChangeRequested(ao->filterLowHz, ao->filterHighHz);
        ev->accept();
        return;
    }

    if (m_draggingVfo) {
        const int mx = static_cast<int>(ev->position().x());
        const double mhz = snapToStep(xToMhz(mx) - m_vfoDragOffsetHz / 1.0e6, m_stepHz);
        emit incrementalTuneRequested(mhz);
        ev->accept();
        return;
    }

    if (m_draggingPan) {
        const int dx = static_cast<int>(ev->position().x()) - m_panDragStartX;
        // Dragging right moves the view right → center shifts left
        const double deltaMhz = -(static_cast<double>(dx) / width()) * m_bandwidthMhz;
        const double newCenter = m_panDragStartCenter + deltaMhz;
        reprojectWaterfall(m_centerMhz, m_bandwidthMhz, newCenter, m_bandwidthMhz);
        m_centerMhz = newCenter;
        markOverlayDirty();
        emit centerChangeRequested(newCenter);
        ev->accept();
        return;
    }

    // Update cursor based on hover position
    const int wfY = specH + DIVIDER_H + FREQ_SCALE_H;

    if (y >= specH && y < specH + DIVIDER_H) {
        setSpectrumCursor(Qt::SplitVCursor);
    } else if (y >= specH + DIVIDER_H && y < wfY) {
        const QRect wfRect(0, wfY, width(), height() - wfY);
        if (waterfallLiveButtonRect(wfRect).contains(ev->position().toPoint())) {
            setSpectrumCursor(Qt::PointingHandCursor);
        } else {
            setSpectrumCursor(Qt::SizeHorCursor);
        }
    } else if (y >= wfY) {
        const QRect wfRect(0, wfY, width(), height() - wfY);
        const QRect timeScaleRect = waterfallTimeScaleRect(wfRect);
        const QPoint pos = ev->position().toPoint();
        if (timeScaleRect.contains(pos)) {
            setSpectrumCursor(Qt::SizeVerCursor);
        } else {
            setSpectrumCursor(Qt::CrossCursor);
        }
    } else if (y < specH) {
        const QPoint pos(mx, y);
        updateTrackedCursorState(pos, rect().contains(pos));
        const bool hoveringTnf = m_hoveredTnfId >= 0;

        // Check off-screen slice indicator hover
        int oldHover = m_hoveringOffScreenIdx;
        m_hoveringOffScreenIdx = -1;
        for (int oi = 0; oi < m_offScreenRects.size(); ++oi) {
            if (!m_offScreenRects[oi].isNull() && m_offScreenRects[oi].contains(pos)) {
                m_hoveringOffScreenIdx = oi; break;
            }
        }
        if (m_hoveringOffScreenIdx != oldHover) markOverlayDirty();

        if (m_hoveringOffScreenIdx >= 0) {
            setSpectrumCursor(Qt::PointingHandCursor);
        } else {
            const int stripX = width() - DBM_STRIP_W;

            // Hovering over dBm scale strip
            if (mx >= stripX) {
                if (y < DBM_ARROW_H)
                    setSpectrumCursor(Qt::PointingHandCursor);
                else
                    setSpectrumCursor(Qt::SizeVerCursor);
            } else {
                // Check if hovering over a filter edge or inactive slice marker
                bool foundCursor = false;
                if (hoveringTnf) {
                    if (const TnfMarker* tnf = tnfMarkerById(m_hoveredTnfId)) {
                        setSpectrumCursor(Qt::SizeAllCursor);
                        m_hoveredTnfId = tnf->id;
                        foundCursor = true;
                    }
                }
                if (const auto* ao = activeOverlay()) {
                    const int loX = mhzToX(ao->freqMhz + ao->filterLowHz / 1.0e6);
                    const int hiX = mhzToX(ao->freqMhz + ao->filterHighHz / 1.0e6);
                    constexpr int GRAB = 5;
                    if (!foundCursor
                        && (std::abs(mx - loX) <= GRAB || std::abs(mx - hiX) <= GRAB)) {
                        setSpectrumCursor(Qt::SizeHorCursor);
                        foundCursor = true;
                    }
                }
                if (!foundCursor) {
                    // Check inactive slice markers + badges
                    for (const auto& so : m_sliceOverlays) {
                        if (so.isActive) continue;
                        int sliceX = mhzToX(so.freqMhz);
                        if ((mx >= sliceX - 8 && mx <= sliceX + 35 && y <= 25)
                            || std::abs(mx - sliceX) <= 8) {
                            setSpectrumCursor(Qt::PointingHandCursor);
                            foundCursor = true;
                            break;
                        }
                    }
                }
                if (!foundCursor && m_showSpots) {
                    bool spotHover = false;
                    for (const auto& hr : m_spotClickRects) {
                        if (hr.rect.contains(pos)) {
                            setSpectrumCursor(Qt::PointingHandCursor);
                            foundCursor = true;
                            // Show spot tooltip
                            if (hr.markerIndex >= 0 && hr.markerIndex < m_spotMarkers.size()) {
                                const auto& sm = m_spotMarkers[hr.markerIndex];
                                QString tip = QString("<b>%1</b>  %2 MHz")
                                    .arg(sm.callsign)
                                    .arg(sm.freqMhz, 0, 'f', 4);
                                if (!sm.source.isEmpty())
                                    tip += QString("<br>Source: %1").arg(sm.source);
                                if (!sm.spotterCallsign.isEmpty())
                                    tip += QString("<br>Spotter: %1").arg(sm.spotterCallsign);
                                if (!sm.comment.isEmpty())
                                    tip += QString("<br>%1").arg(sm.comment);
                                if (sm.timestampMs > 0)
                                    tip += QString("<br>Spotted: %1 UTC").arg(
                                        QDateTime::fromMSecsSinceEpoch(sm.timestampMs, QTimeZone::utc()).toString("yyyy-MM-dd HH:mm:ss"));
                                QToolTip::showText(ev->globalPosition().toPoint(), tip, this);
                            }
                            spotHover = true;
                            break;
                        }
                    }
                    if (!foundCursor) {
                        for (const auto& cluster : m_spotClusters) {
                            if (cluster.rect.contains(pos)) {
                                setSpectrumCursor(Qt::PointingHandCursor);
                                foundCursor = true;
                                break;
                            }
                        }
                    }
                }
                // Prop forecast overlay click target
                if (!foundCursor && !m_propClickRect.isNull()
                    && m_propClickRect.contains(QPoint(mx, y))) {
                    setSpectrumCursor(Qt::PointingHandCursor);
                    foundCursor = true;
                }
                if (!foundCursor) {
                    setSpectrumCursor(Qt::CrossCursor);
                }
            }
        }
    } else {
        updateTrackedCursorState(ev->position().toPoint(), false);
    }

    // Band plan spot tooltip on hover
    const int specH2 = static_cast<int>((height() - FREQ_SCALE_H - DIVIDER_H) * m_spectrumFrac);
    const int bandBarTop = specH2 - 8;
    if (m_hoveredTnfId >= 0) {
        QToolTip::hideText();
        return;
    }
    if (y >= bandBarTop && y <= specH2) {
        const int mx2 = static_cast<int>(ev->position().x());
        const auto& spots = m_bandPlanMgr ? m_bandPlanMgr->spots() : QVector<BandPlanManager::Spot>{};
        for (const auto& spot : spots) {
            const int sx = mhzToX(spot.freqMhz);
            if (std::abs(mx2 - sx) <= 5) {
                QToolTip::showText(ev->globalPosition().toPoint(),
                    QString("%1 MHz — %2")
                        .arg(spot.freqMhz, 0, 'f', 3)
                        .arg(spot.label),
                    this);
                return;
            }
        }
        QToolTip::hideText();
    }
}

void SpectrumWidget::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_draggingTnfId >= 0) {
        m_draggingTnfId = -1;
        setSpectrumCursor(Qt::CrossCursor);
        ev->accept();
        return;
    }
    if (m_draggingDivider) {
        m_draggingDivider = false;
        setSpectrumCursor(Qt::CrossCursor);
        auto& s = AppSettings::instance();
        s.setValue(settingsKey("SpectrumSplitRatio"), QString::number(m_spectrumFrac, 'f', 3));
        s.save();
        ev->accept();
        return;
    }
    if (m_draggingDbm) {
        m_draggingDbm = false;
        setSpectrumCursor(Qt::CrossCursor);
        ev->accept();
        return;
    }
    if (m_draggingTimeScale) {
        m_draggingTimeScale = false;
        setSpectrumCursor(Qt::CrossCursor);
        ev->accept();
        return;
    }
    if (m_draggingBandwidth) {
        m_draggingBandwidth = false;
        setSpectrumCursor(Qt::CrossCursor);
        // Re-send the final combined range so the release lands on the same
        // coherent center/bandwidth pair as the in-flight drag updates.
        emit frequencyRangeChangeRequested(m_centerMhz, m_bandwidthMhz);
        ev->accept();
        return;
    }
    if (m_draggingVfo) {
        m_draggingVfo = false;
        setSpectrumCursor(Qt::CrossCursor);
        ev->accept();
        return;
    }
    if (m_draggingFilter != FilterEdge::None) {
        m_draggingFilter = FilterEdge::None;
        setSpectrumCursor(Qt::CrossCursor);
        ev->accept();
        return;
    }
    if (m_draggingPan) {
        m_draggingPan = false;
        setSpectrumCursor(Qt::CrossCursor);

        // Single-click-to-tune: if the mouse didn't move during the
        // "pan drag", treat it as a click-to-tune instead
        if (m_singleClickTune && ev->button() == Qt::LeftButton && !m_spotClickConsumed) {
            QPoint delta = ev->position().toPoint() - m_clickPressPos;
            if (delta.manhattanLength() <= 4
                && !m_indicatorStripRect.contains(ev->position().toPoint())) {
                const int mx = static_cast<int>(ev->position().x());
                if (mx < width() - DBM_STRIP_W) {
                    double rawMhz = xToMhz(mx);
                    emit frequencyClicked(snapToStep(rawMhz, m_stepHz));
                }
            }
        }
        m_spotClickConsumed = false;
        ev->accept();
        return;
    }

    // Single-click-to-tune in FFT area (not consumed by pan drag)
    if (m_singleClickTune && ev->button() == Qt::LeftButton && !m_spotClickConsumed) {
        QPoint delta = ev->position().toPoint() - m_clickPressPos;
        if (delta.manhattanLength() <= 4
            && !m_indicatorStripRect.contains(ev->position().toPoint())) {
            const int mx = static_cast<int>(ev->position().x());
            if (mx < width() - DBM_STRIP_W) {
                double rawMhz = xToMhz(mx);
                emit frequencyClicked(snapToStep(rawMhz, m_stepHz));
                ev->accept();
                m_spotClickConsumed = false;
                return;
            }
        }
    }
    m_spotClickConsumed = false;
}

void SpectrumWidget::showAddSpotDialog(double freqMhz)
{
    // Snap to step size
    if (m_stepHz > 0) {
        const double stepMhz = m_stepHz / 1e6;
        freqMhz = std::round(freqMhz / stepMhz) * stepMhz;
    }
    auto& as = AppSettings::instance();
    QDialog dlg(this);
    dlg.setWindowTitle("Add Spot");
    dlg.setStyleSheet("QDialog { background: #1a1a2e; color: #c8d8e8; }"
                      "QLineEdit { background: #0f0f1a; color: #c8d8e8; border: 1px solid #304060; padding: 4px; }"
                      "QComboBox { background: #0f0f1a; color: #c8d8e8; border: 1px solid #304060; padding: 4px; }"
                      "QLabel { color: #c8d8e8; }"
                      "QCheckBox { color: #c8d8e8; }");

    auto* layout = new QFormLayout(&dlg);

    auto* freqLabel = new QLabel(QString("%1 MHz").arg(freqMhz, 0, 'f', 6));
    layout->addRow("Frequency:", freqLabel);

    auto* callEdit = new QLineEdit;
    callEdit->setPlaceholderText("Callsign (required)");
    layout->addRow("Callsign:", callEdit);

    auto* commentEdit = new QLineEdit;
    commentEdit->setPlaceholderText("Optional comment");
    layout->addRow("Comment:", commentEdit);

    auto* lifetimeCombo = new QComboBox;
    lifetimeCombo->addItem("5 minutes", 300);
    lifetimeCombo->addItem("15 minutes", 900);
    lifetimeCombo->addItem("30 minutes", 1800);
    lifetimeCombo->addItem("1 hour", 3600);
    lifetimeCombo->addItem("2 hours", 7200);
    int defaultLifetime = as.value("ManualSpotLifetime", 1800).toInt();
    for (int i = 0; i < lifetimeCombo->count(); ++i) {
        if (lifetimeCombo->itemData(i).toInt() == defaultLifetime) {
            lifetimeCombo->setCurrentIndex(i);
            break;
        }
    }
    layout->addRow("Lifetime:", lifetimeCombo);

    auto* forwardCheck = new QCheckBox("Forward to DX Cluster");
    forwardCheck->setChecked(as.value("SpotForwardToCluster", "False").toString() == "True");
    layout->addRow("", forwardCheck);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    const QString callsign = callEdit->text().trimmed().toUpper();
    if (callsign.isEmpty()) return;

    const QString comment = commentEdit->text().trimmed();
    const int lifetimeSec = lifetimeCombo->currentData().toInt();
    const bool forward = forwardCheck->isChecked();

    // Remember preferences
    as.setValue("ManualSpotLifetime", QString::number(lifetimeSec));
    as.setValue("SpotForwardToCluster", forward ? "True" : "False");

    emit spotAddRequested(freqMhz, callsign, comment, lifetimeSec, forward);
}

void SpectrumWidget::mouseDoubleClickEvent(QMouseEvent* ev)
{
    const int chromeH  = FREQ_SCALE_H + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int wfY = specH + DIVIDER_H + FREQ_SCALE_H;
    const int y = static_cast<int>(ev->position().y());
    const int mx = static_cast<int>(ev->position().x());
    const int rightStripW = (y >= wfY) ? waterfallStripWidth() : DBM_STRIP_W;

    // Consume double-clicks on the dBm and time strips
    if (mx >= width() - rightStripW) {
        ev->accept();
        return;
    }

    // Double-click on off-screen slice indicator → recenter on that slice
    for (int oi = 0; oi < m_offScreenRects.size(); ++oi) {
        if (!m_offScreenRects[oi].isNull() && m_offScreenRects[oi].contains(QPoint(mx, y))) {
            m_centerMhz = m_sliceOverlays[oi].freqMhz;
            markOverlayDirty();
            emit centerChangeRequested(m_centerMhz);
            // Suppress the second mouseRelease's single-click-to-tune emit
            // (Qt fires Press → Release → DoubleClick → Release; without this
            // flag the trailing release would re-tune against the new
            // center, landing roughly bandwidth/2 away from the slice). #2237
            m_spotClickConsumed = true;
            ev->accept();
            return;
        }
    }

    // Double-click in FFT or waterfall → tune to clicked frequency
    if (y < specH || y >= wfY) {
        const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
        double rawMhz = startMhz + (ev->position().x() / width()) * m_bandwidthMhz;

        emit frequencyClicked(snapToStep(rawMhz, m_stepHz));
        ev->accept();
        return;
    }

    QWidget::mouseDoubleClickEvent(ev);
}

void SpectrumWidget::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    updateTrackedCursorState(QPoint(-1, -1), false);
}

void SpectrumWidget::setBackgroundImage(const QString& path)
{
    m_bgImagePath = path;
    m_bgScaled = {};
    m_bgScaledSize = {};
    if (path.isEmpty()) {
        m_bgImage = {};
    } else {
        m_bgImage = QImage(path);
        qDebug() << "SpectrumWidget: background image" << path
                 << "loaded:" << !m_bgImage.isNull()
                 << "size:" << m_bgImage.size();
        if (m_bgImage.isNull())
            qWarning() << "SpectrumWidget: failed to load background image:" << path;
    }
    markOverlayDirty();
}

bool SpectrumWidget::event(QEvent* ev)
{
    // Re-assert mouse tracking after native window changes (reparenting into
    // QSplitter, window recreation). Without this, QRhiWidget's native Metal
    // surface loses mouse tracking and mouseMoveEvent stops firing.
    if (ev->type() == QEvent::WinIdChange || ev->type() == QEvent::ParentChange) {
        setMouseTracking(true);
    }

    if (ev->type() == QEvent::NativeGesture) {
        auto* ge = static_cast<QNativeGestureEvent*>(ev);
        if (ge->gestureType() == Qt::ZoomNativeGesture) {
            // value > 0 = pinch out (zoom in = narrower BW)
            // value < 0 = pinch in  (zoom out = wider BW)
            // Zoom anchored on the frequency under the cursor: the frequency
            // at the mouse position stays at that pixel after the zoom.
            const double delta = ge->value();
            if (qFuzzyIsNull(delta)) { return true; }
            const double factor = 1.0 / (1.0 + delta);  // invert: pinch-out narrows BW
            const double newBw = m_bandwidthMhz * factor;
            if (newBw < m_minBwMhz || newBw > m_maxBwMhz) { return true; }  // at limit
            // Anchor: keep the frequency under the cursor at the same pixel.
            const double mouseXFrac = ge->position().x() / width() - 0.5;
            const double anchorMhz = m_centerMhz + mouseXFrac * m_bandwidthMhz;
            const double newCenter = anchorMhz - mouseXFrac * newBw;
            reprojectWaterfall(m_centerMhz, m_bandwidthMhz, newCenter, newBw);
            if (!reprojectSpectrum(m_centerMhz, m_bandwidthMhz, newCenter, newBw)) {
                m_bins.clear();
                m_smoothed.clear();
            }
            m_bandwidthMhz = newBw;
            m_centerMhz = newCenter;
            markOverlayDirty();
            emit frequencyRangeChangeRequested(newCenter, newBw);
            return true;
        }
    }
    return SPECTRUM_BASE_CLASS::event(ev);
}

void SpectrumWidget::wheelEvent(QWheelEvent* ev)
{
    // Skip scroll on the divider + freq scale bar.
    const int chromeH  = FREQ_SCALE_H + DIVIDER_H;
    const int contentH2 = height() - chromeH;
    const int specH2 = static_cast<int>(contentH2 * m_spectrumFrac);
    const int wfY = specH2 + DIVIDER_H + FREQ_SCALE_H;
    const int chromeTop = specH2;
    const int chromeBot = specH2 + chromeH;
    if (ev->position().y() >= chromeTop && ev->position().y() < chromeBot) {
        ev->ignore();
        return;
    }
    // Consume wheel events on the dBm / time scale strips
    const int mx = static_cast<int>(ev->position().x());
    const int rightStripW = (ev->position().y() >= wfY) ? waterfallStripWidth() : DBM_STRIP_W;
    if (mx >= width() - rightStripW) {
        ev->accept();
        return;
    }

    // Handle both trackpad (pixelDelta) and mouse wheel (angleDelta)
    int steps = 0;
    if (!ev->pixelDelta().isNull()) {
        // Trackpad: accumulate pixel delta; 1 step per ~15px
        // Ignore momentum (inertial) scrolling
        if (ev->phase() == Qt::ScrollMomentum) { ev->accept(); return; }
        // Ignore horizontal-dominant swipes
        if (qAbs(ev->pixelDelta().x()) > qAbs(ev->pixelDelta().y())) {
            ev->ignore(); return;
        }
        m_scrollAccum += ev->pixelDelta().y();
        steps = m_scrollAccum / 15;
        m_scrollAccum -= steps * 15;
        // Clamp to ±1: on Linux/libinput regular mice often report pixelDelta
        // (e.g. 120px per notch) which would produce 8 steps and an 8× jump.
        steps = qBound(-1, steps, 1);
        // Debounce: same 50ms window as the angleDelta path below (#2150)
        if (steps != 0) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastWheelMs < 50) {
                steps = 0;
            } else {
                m_lastWheelMs = now;
            }
        }
    } else {
        // Standard mouse wheel: angleDelta is in 1/8° units, one notch = 120.
        // Some desktops (KDE Plasma, Cinnamon) send inflated deltas
        // (e.g. 960 per notch) or multiple rapid events per physical notch.
        // Clamp to ±1 per event AND debounce within 80ms window. (#504, #556)
        m_angleAccum += ev->angleDelta().y();
        steps = m_angleAccum / 120;
        m_angleAccum -= steps * 120;
        steps = qBound(-1, steps, 1);
        if (steps != 0) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastWheelMs < 50) {
                steps = 0;  // debounce: too soon after last step
            } else {
                m_lastWheelMs = now;
            }
        }
    }
    if (steps == 0) { ev->ignore(); return; }

    const auto* ao = activeOverlay();
    const double vfoMhz = ao ? ao->freqMhz : m_centerMhz;
    // Snap the base frequency to the step grid first, then add the delta.
    // This ensures every scroll moves by exactly m_stepHz rather than snapping
    // the destination, which would cause an alignment artifact on the first
    // scroll (e.g. step=500 Hz at .100 MHz → effective 400 Hz jump).
    const double baseMhz = snapToStep(vfoMhz, m_stepHz);
    const double newMhz  = baseMhz + steps * m_stepHz / 1e6;
    emit incrementalTuneRequested(newMhz);
    ev->accept();
}

// ─── Resize ───────────────────────────────────────────────────────────────────

void SpectrumWidget::resizeEvent(QResizeEvent* ev)
{
    SPECTRUM_BASE_CLASS::resizeEvent(ev);

    // Re-assert mouse tracking — on macOS with WA_NativeWindow, reparenting
    // into a QSplitter can reset native window properties.
    setMouseTracking(true);

    const int chromeH  = FREQ_SCALE_H + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int wfHeight = static_cast<int>(contentH * (1.0f - m_spectrumFrac));
    if (wfHeight > 0 && width() > 0) {
        QImage newWf(width(), wfHeight, QImage::Format_RGB32);
        newWf.fill(Qt::black);
        if (!m_waterfall.isNull()) {
            QImage scaled = m_waterfall.scaled(width(), wfHeight, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            if (!scaled.isNull())
                newWf = std::move(scaled);
        }
        m_waterfall = std::move(newWf);
        m_wfWriteRow = 0;
        ensureWaterfallHistory();
        if (m_wfHistoryRowCount > 0) {
            rebuildWaterfallViewport();
        }
    }

    // Position GPU renderer to cover FFT + waterfall area


    positionZoomButtons();

    // Notify MainWindow so it can re-push xpixels/ypixels to the radio (#1511)
    if (width() >= 100 && height() >= 100)
        emit dimensionsChanged(width(), height());
}

void SpectrumWidget::positionZoomButtons()
{
    constexpr int pad = 4;
    constexpr int sz = 22;
    const int botY = height() - pad;

    // Row 1 (bottom): − | + (bandwidth zoom)
    m_zoomOutBtn->move(pad, botY - sz);
    m_zoomInBtn->move(pad + sz + 2, botY - sz);
    // Row 0 (above): S | B (segment/band zoom)
    m_zoomSegBtn->move(pad, botY - sz - sz - 2);
    m_zoomBandBtn->move(pad + sz + 2, botY - sz - sz - 2);
}

// ─── Colour map ───────────────────────────────────────────────────────────────

QRgb SpectrumWidget::dbmToRgb(float dbm) const
{
    // Black level shifts the floor: higher black_level = more of the noise is black.
    // Color gain controls the visible range: higher gain = narrower range = more contrast.
    // black_level 0-125: higher = floor moves closer to signals (more black)
    // color_gain 0-100: higher = narrower visible range = more contrast
    const float floorShift = (125 - m_wfBlackLevel) * 0.4f;  // inverted: 0=max shift, 125=no shift
    const float visRange = 80.0f - m_wfColorGain * 0.7f;  // 80 dB down to 10 dB
    const float effectiveMin = m_wfMinDbm + floorShift;
    const float effectiveMax = effectiveMin + visRange;

    const float t = qBound(0.0f, (dbm - effectiveMin) / (effectiveMax - effectiveMin), 1.0f);

    int n = 0;
    const auto* stops = wfSchemeStops(m_wfColorScheme, n);
    return interpolateGradient(t, stops, n);
}

// Map native waterfall tile intensity to RGB.
// Intensity is int16(raw)/128.0f — observed range ~96-120 on HF.
// m_wfBlackLevel and m_wfColorGain control the mapping independently from FFT.
QRgb SpectrumWidget::intensityToRgb(float intensity) const
{
    // Map black_level (0-100) to an intensity threshold.
    // When auto-black is on, anchor to the measured noise floor and let the
    // user bias it via the auto-black offset slider:
    //   offset 50 → no bias (threshold sits at the noise floor)
    //   offset  0 → +25 intensity above the noise floor (darker waterfall)
    //   offset 100 → -25 intensity below the noise floor (lighter waterfall)
    float blackThresh;
    if (m_wfAutoBlack) {
        blackThresh = m_autoBlackThresh + (50 - m_wfAutoBlackOffset) * 0.5f;
    } else {
        // Manual: slider 0 → thresh 160 (well above noise), slider 100 → thresh 60.
        blackThresh = 160.0f - m_wfBlackLevel * 1.0f;
    }

    // Map color_gain (0-100) to the visible range width.
    // Higher gain = narrower range = more color contrast.
    // gain=0 → 120 dB range (very dim), gain=100 → 29 dB range (max contrast)
    const float rangeWidth = std::max(1.0f, 120.0f - m_wfColorGain * 0.91f);

    const float t = qBound(0.0f, (intensity - blackThresh) / rangeWidth, 1.0f);

    int n = 0;
    const auto* stops = wfSchemeStops(m_wfColorScheme, n);
    return interpolateGradient(t, stops, n);
}

// ─── Waterfall update ─────────────────────────────────────────────────────────

void SpectrumWidget::pushWaterfallRow(const QVector<float>& bins, int destWidth,
                                      double tileLowMhz, double tileHighMhz)
{
    // (time scale uses m_wfLineDuration from radio status — no measurement needed)

    if (m_waterfall.isNull() || destWidth <= 0) return;

    const int h = m_waterfall.height();
    if (h <= 1) return;

    Q_UNUSED(tileLowMhz);
    Q_UNUSED(tileHighMhz);

    QVector<QRgb> scanline(destWidth, qRgb(0, 0, 0));
    for (int x = 0; x < destWidth; ++x) {
        const int binIdx = x * bins.size() / destWidth;
        const float dbm = (binIdx >= 0 && binIdx < bins.size()) ? bins[binIdx] : m_wfMinDbm;
        scanline[x] = dbmToRgb(dbm);
    }

    appendHistoryRow(scanline.constData(), QDateTime::currentMSecsSinceEpoch());
    if (m_wfLive) {
        appendVisibleRow(scanline.constData());
    } else {
        rebuildWaterfallViewport();
    }
}

// ─── Paint ────────────────────────────────────────────────────────────────────

#ifdef AETHER_GPU_SPECTRUM

// Fullscreen quad: position (x,y) + texcoord (u,v)
static const float kQuadData[] = {
    -1, -1,  0, 1,   // bottom-left
     1, -1,  1, 1,   // bottom-right
    -1,  1,  0, 0,   // top-left
     1,  1,  1, 0,   // top-right
};

static QShader loadShader(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "SpectrumWidget: failed to load shader" << path;
        return {};
    }
    QShader s = QShader::fromSerialized(f.readAll());
    if (!s.isValid())
        qWarning() << "SpectrumWidget: invalid shader" << path;
    return s;
}

void SpectrumWidget::initWaterfallPipeline()
{
    QRhi* r = rhi();

    m_wfVbo = r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuadData));
    m_wfVbo->create();

    m_wfUbo = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16);
    m_wfUbo->create();

    m_wfGpuTexW = qMax(width(), 64);
    m_wfGpuTexH = qMax(m_waterfall.height(), 64);
    m_wfGpuTex = r->newTexture(QRhiTexture::RGBA8, QSize(m_wfGpuTexW, m_wfGpuTexH));
    m_wfGpuTex->create();

    m_wfSampler = r->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                 QRhiSampler::None,
                                 QRhiSampler::ClampToEdge, QRhiSampler::Repeat);
    m_wfSampler->create();

    m_wfSrb = r->newShaderResourceBindings();
    m_wfSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, m_wfUbo),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_wfGpuTex, m_wfSampler),
    });
    m_wfSrb->create();

    QShader vs = loadShader(":/shaders/resources/shaders/texturedquad.vert.qsb");
    QShader fs = loadShader(":/shaders/resources/shaders/texturedquad.frag.qsb");
    if (!vs.isValid() || !fs.isValid()) {
        qWarning() << "SpectrumWidget: waterfall shader load failed";
        return;
    }

    m_wfPipeline = r->newGraphicsPipeline();
    m_wfPipeline->setShaderStages({
        {QRhiShaderStage::Vertex, vs},
        {QRhiShaderStage::Fragment, fs},
    });

    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });
    m_wfPipeline->setVertexInputLayout(layout);
    m_wfPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_wfPipeline->setShaderResourceBindings(m_wfSrb);
    m_wfPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_wfPipeline->create();

    qDebug() << "SpectrumWidget: waterfall pipeline created"
             << m_wfGpuTexW << "x" << m_wfGpuTexH;
}

void SpectrumWidget::initOverlayPipeline()
{
    QRhi* r = rhi();

    m_ovVbo = r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuadData));
    m_ovVbo->create();

    int w = qMax(width(), 64);
    int h = qMax(height(), 64);
    const qreal dpr = devicePixelRatioF();
    const int pw = static_cast<int>(w * dpr);
    const int ph = static_cast<int>(h * dpr);
    m_ovGpuTex = r->newTexture(QRhiTexture::RGBA8, QSize(pw, ph));
    m_ovGpuTex->create();

    m_ovSampler = r->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                 QRhiSampler::None,
                                 QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_ovSampler->create();

    m_ovSrb = r->newShaderResourceBindings();
    m_ovSrb->setBindings({
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_ovGpuTex, m_ovSampler),
    });
    m_ovSrb->create();

    QShader vs = loadShader(":/shaders/resources/shaders/overlay.vert.qsb");
    QShader fs = loadShader(":/shaders/resources/shaders/overlay.frag.qsb");
    if (!vs.isValid() || !fs.isValid()) {
        qWarning() << "SpectrumWidget: overlay shader load failed";
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

    // Enable alpha blending for overlay compositing
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    m_ovPipeline->setTargetBlends({blend});

    m_ovPipeline->create();

    m_overlayStatic = QImage(pw, ph, QImage::Format_RGBA8888_Premultiplied);
    m_overlayStatic.setDevicePixelRatio(dpr);
    m_overlayDynamic = QImage(pw, ph, QImage::Format_RGBA8888_Premultiplied);
    m_overlayDynamic.setDevicePixelRatio(dpr);
    m_overlayDynamic.fill(Qt::transparent);

    qDebug() << "SpectrumWidget: overlay pipeline created" << pw << "x" << ph << "dpr:" << dpr;
}

void SpectrumWidget::initSpectrumPipeline()
{
    QRhi* r = rhi();

    // Dynamic vertex buffers: 2N × 6 floats for triangle strip line expansion
    m_fftLineVbo = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                                 kMaxFftBins * 2 * kFftVertStride * sizeof(float));
    m_fftLineVbo->create();

    m_fftFillVbo = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                                 kMaxFftBins * 2 * kFftVertStride * sizeof(float));
    m_fftFillVbo->create();

    // No uniforms — color is per-vertex
    m_fftSrb = r->newShaderResourceBindings();
    m_fftSrb->setBindings({});
    m_fftSrb->create();

    QShader vs = loadShader(":/shaders/resources/shaders/spectrum.vert.qsb");
    QShader fs = loadShader(":/shaders/resources/shaders/spectrum.frag.qsb");
    if (!vs.isValid() || !fs.isValid()) {
        qWarning() << "SpectrumWidget: spectrum shader load failed";
        return;
    }

    QRhiVertexInputLayout layout;
    layout.setBindings({{kFftVertStride * sizeof(float)}});  // stride: 6 floats
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},                     // position
        {0, 1, QRhiVertexInputAttribute::Float4, 2 * sizeof(float)},     // color
    });

    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

    // Fill pipeline (triangle strip)
    m_fftFillPipeline = r->newGraphicsPipeline();
    m_fftFillPipeline->setShaderStages({
        {QRhiShaderStage::Vertex, vs},
        {QRhiShaderStage::Fragment, fs},
    });
    m_fftFillPipeline->setVertexInputLayout(layout);
    m_fftFillPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_fftFillPipeline->setShaderResourceBindings(m_fftSrb);
    m_fftFillPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_fftFillPipeline->setTargetBlends({blend});
    m_fftFillPipeline->create();

    // Line pipeline (line strip)
    m_fftLinePipeline = r->newGraphicsPipeline();
    m_fftLinePipeline->setShaderStages({
        {QRhiShaderStage::Vertex, vs},
        {QRhiShaderStage::Fragment, fs},
    });
    m_fftLinePipeline->setVertexInputLayout(layout);
    m_fftLinePipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_fftLinePipeline->setShaderResourceBindings(m_fftSrb);
    m_fftLinePipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_fftLinePipeline->setTargetBlends({blend});
    m_fftLinePipeline->create();

    qDebug() << "SpectrumWidget: spectrum pipeline created (vertex-colored)";
}

void SpectrumWidget::initialize(QRhiCommandBuffer* cb)
{
    if (m_rhiInitialized) return;

    QRhi* r = rhi();
    if (!r) {
        qWarning() << "SpectrumWidget: QRhi initialization failed — no GPU backend";
        return;
    }

    qDebug() << "SpectrumWidget: QRhi initialized, backend:" << r->backendName();

    // Upload quad vertex data for both pipelines
    auto* batch = r->nextResourceUpdateBatch();

    initWaterfallPipeline();
    initOverlayPipeline();
    initSpectrumPipeline();

    // Upload VBO data
    batch->uploadStaticBuffer(m_wfVbo, kQuadData);
    batch->uploadStaticBuffer(m_ovVbo, kQuadData);

    // Initial full waterfall texture upload (convert RGB32→RGBA8)
    if (!m_waterfall.isNull()) {
        QImage rgba = m_waterfall.convertToFormat(QImage::Format_RGBA8888);
        QRhiTextureSubresourceUploadDescription desc(rgba);
        batch->uploadTexture(m_wfGpuTex, QRhiTextureUploadEntry(0, 0, desc));
    }

    cb->resourceUpdate(batch);
    m_wfTexFullUpload = false;
    m_wfLastUploadedRow = m_wfWriteRow;
    m_rhiInitialized = true;

    // Force full overlay repaint + upload — the new GPU texture is empty.
    // Without this, m_overlayStaticDirty and m_overlayNeedsUpload may be
    // false from the previous render cycle, leaving the overlay invisible.
    m_overlayStaticDirty = true;
    m_overlayNeedsUpload = true;

    // Re-apply cursor and mouse tracking now that the native surface exists.
    setCursor(cursor());
    setMouseTracking(true);
}

void SpectrumWidget::renderGpuFrame(QRhiCommandBuffer* cb)
{
    // Guard: QRhiWidget surface recreation (add/remove panadapter, reparent)
    // can silently clear mouse tracking on macOS. Re-assert cheaply per frame.
    if (!hasMouseTracking()) {
        setMouseTracking(true);
    }

    // Tune guide recovery: after reparenting (add/remove panadapter), Qt may
    // not deliver mouseMoveEvents even with mouse tracking enabled (missing
    // enterEvent, stale widget state). Poll the actual cursor position to
    // detect when the mouse is inside the widget but events aren't flowing.
    {
        QPoint localPos = mapFromGlobal(QCursor::pos());
        if (rect().contains(localPos)) {
            updateTrackedCursorState(localPos, true);
        } else if (m_cursorPos.x() >= 0 || m_hoveredTnfId >= 0 || m_tuneGuideVisible) {
            // Mouse left the widget without a leaveEvent
            updateTrackedCursorState(QPoint(-1, -1), false);
        }
    }

    QRhi* r = rhi();
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= FREQ_SCALE_H + DIVIDER_H + 2) return;

    const int chromeH = FREQ_SCALE_H + DIVIDER_H;
    const int contentH = h - chromeH;
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int wfY = specH + DIVIDER_H + FREQ_SCALE_H;
    const int wfH = h - wfY;
    const QRect specRect(0, 0, w, specH);
    const QRect wfRect(0, wfY, w, wfH);

    // Detect display state changes that may bypass markOverlayDirty()
    {
        if (m_centerMhz != m_lastDetectCenter || m_bandwidthMhz != m_lastDetectBw ||
            m_refLevel != m_lastDetectRef || m_dynamicRange != m_lastDetectDyn ||
            m_spectrumFrac != m_lastDetectFrac ||
            m_wnbActive != m_lastDetectWnb || m_rfGainValue != m_lastDetectRfGain ||
            m_wideActive != m_lastDetectWide) {
            markOverlayDirty();
            m_lastDetectCenter = m_centerMhz; m_lastDetectBw = m_bandwidthMhz;
            m_lastDetectRef = m_refLevel; m_lastDetectDyn = m_dynamicRange;
            m_lastDetectFrac = m_spectrumFrac;
            m_lastDetectWnb = m_wnbActive; m_lastDetectRfGain = m_rfGainValue;
            m_lastDetectWide = m_wideActive;
        }
    }

    auto* batch = r->nextResourceUpdateBatch();

    // Upload waterfall texture — full or incremental
    if (!m_waterfall.isNull()) {
        // Resize texture if needed — full re-upload
        if (m_waterfall.width() != m_wfGpuTexW || m_waterfall.height() != m_wfGpuTexH) {
            m_wfGpuTexW = m_waterfall.width();
            m_wfGpuTexH = m_waterfall.height();
            m_wfGpuTex->setPixelSize(QSize(m_wfGpuTexW, m_wfGpuTexH));
            m_wfGpuTex->create();
            m_wfSrb->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, m_wfUbo),
                QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_wfGpuTex, m_wfSampler),
            });
            m_wfSrb->create();
            m_wfTexFullUpload = true;
        }

        if (m_wfTexFullUpload) {
            // Full upload (init or resize)
            QImage rgba = m_waterfall.convertToFormat(QImage::Format_RGBA8888);
            QRhiTextureSubresourceUploadDescription desc(rgba);
            batch->uploadTexture(m_wfGpuTex, QRhiTextureUploadEntry(0, 0, desc));
            m_wfLastUploadedRow = m_wfWriteRow;
            m_wfTexFullUpload = false;
        } else if (m_wfWriteRow != m_wfLastUploadedRow) {
            // Incremental upload — only the rows that changed since last frame
            const int texH = m_wfGpuTexH;
            int from = m_wfLastUploadedRow;
            int to = m_wfWriteRow;

            // Walk backwards from 'from' to 'to' (ring buffer decrements)
            // Upload each dirty row individually
            QRhiTextureUploadDescription uploadDesc;
            QVector<QRhiTextureUploadEntry> entries;

            int row = from;
            int maxRows = texH;  // safety cap
            while (row != to && maxRows-- > 0) {
                row = (row - 1 + texH) % texH;
                // Extract one scanline from the waterfall QImage, convert to RGBA8
                const uchar* srcLine = m_waterfall.constScanLine(row);
                QImage rowImg(reinterpret_cast<const uchar*>(srcLine),
                              m_wfGpuTexW, 1, m_waterfall.bytesPerLine(),
                              QImage::Format_RGB32);
                QImage rowRgba = rowImg.convertToFormat(QImage::Format_RGBA8888);

                QRhiTextureSubresourceUploadDescription desc(rowRgba);
                desc.setDestinationTopLeft(QPoint(0, row));
                entries.append(QRhiTextureUploadEntry(0, 0, desc));
            }

            if (!entries.isEmpty()) {
                uploadDesc.setEntries(entries.begin(), entries.end());
                batch->uploadTexture(m_wfGpuTex, uploadDesc);
            }
            m_wfLastUploadedRow = m_wfWriteRow;
        }
    }

    // Update waterfall uniforms — just the ring buffer row offset
    float rowOffset = (m_wfGpuTexH > 0)
        ? static_cast<float>(m_wfWriteRow) / m_wfGpuTexH
        : 0.0f;
    float uniforms[] = {rowOffset, 0.0f, 0.0f, 0.0f};
    batch->updateDynamicBuffer(m_wfUbo, 0, sizeof(uniforms), uniforms);

    // Render overlays — split into static (on state change) and dynamic (every frame)
    {
        // Resize overlay images if needed
        const qreal dpr = devicePixelRatioF();
        const int pw = static_cast<int>(w * dpr);
        const int ph = static_cast<int>(h * dpr);
        if (m_overlayStatic.size() != QSize(pw, ph)) {
            m_overlayStatic = QImage(pw, ph, QImage::Format_RGBA8888_Premultiplied);
            m_overlayStatic.setDevicePixelRatio(dpr);
            m_overlayDynamic = QImage(pw, ph, QImage::Format_RGBA8888_Premultiplied);
            m_overlayDynamic.setDevicePixelRatio(dpr);
            m_overlayDynamic.fill(Qt::transparent);
            m_ovGpuTex->setPixelSize(QSize(pw, ph));
            m_ovGpuTex->create();
            m_ovSrb->setBindings({
                QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_ovGpuTex, m_ovSampler),
            });
            m_ovSrb->create();
            m_overlayStaticDirty = true;
        }

        // Static overlay: only repaint when state changes (markOverlayDirty).
        if (m_overlayStaticDirty) {
            m_overlayStatic.fill(Qt::transparent);
            QPainter p(&m_overlayStatic);
            p.setRenderHint(QPainter::Antialiasing, false);

            // Background image
            if (!m_bgImage.isNull()) {
                if (m_bgScaledSize != specRect.size()) {
                    // Scale to cover (keep aspect ratio, expand to fill) then center crop
                    QImage expanded = m_bgImage.scaled(specRect.size(),
                        Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                    int cx = (expanded.width()  - specRect.width())  / 2;
                    int cy = (expanded.height() - specRect.height()) / 2;
                    m_bgScaled = expanded.copy(cx, cy, specRect.width(), specRect.height());
                    m_bgScaledSize = specRect.size();
                }
                p.setOpacity(1.0 - m_bgOpacity / 100.0);
                p.drawImage(specRect.topLeft(), m_bgScaled);
                p.setOpacity(1.0);
            }

            drawGrid(p, specRect);
            if (m_bandPlanFontSize > 0)
                drawBandPlan(p, specRect);
            drawDbmScale(p, specRect);

            // Divider bar
            p.fillRect(0, specH, w, DIVIDER_H, QColor(0x30, 0x40, 0x50));

            drawFreqScale(p, QRect(0, specH + DIVIDER_H, w, FREQ_SCALE_H));
            drawTimeScale(p, wfRect);
            drawTnfMarkers(p, specRect);
            if (m_showSpots || m_showSHistory)
                drawSpotMarkers(p, specRect);
            drawSwrSweep(p, specRect);
            drawSliceMarkers(p, specRect, wfRect);
            drawOffScreenSlices(p, specRect);

            // WNB / RF gain / Prop forecast indicators (top-right of spectrum)
            {
                const bool showProp = m_propForecastVisible
                    && m_propKIndex >= 0
                    && m_propAIndex >= 0
                    && m_propSfi > 0;
                if (m_wnbActive || m_rfGainValue != 0 || showProp || m_wideActive) {
                    QFont indFont(p.font().family(), 14, QFont::Bold);
                    p.setFont(indFont);
                    p.setPen(QColor(0xc8, 0xd8, 0xe8, 180));
                    const QFontMetrics fm(indFont);
                    int y = specRect.top() + fm.ascent() + 4;
                    // Build combined label (left to right: prop, WNB, RF gain, WIDE), right-align
                    QString label;
                    if (showProp) {
                        label += QString("K%1  A%2  SFI %3")
                            .arg(m_propKIndex, 0, 'f', 2)
                            .arg(m_propAIndex)
                            .arg(m_propSfi);
                    }
                    if (m_wnbActive) {
                        if (!label.isEmpty()) { label += QStringLiteral("   "); }
                        label += QStringLiteral("WNB");
                    }
                    if (m_rfGainValue != 0) {
                        if (!label.isEmpty()) { label += QStringLiteral("   "); }
                        label += QStringLiteral("%1%2 dB")
                            .arg(m_rfGainValue > 0 ? "+" : "").arg(m_rfGainValue);
                    }
                    if (m_wideActive) {
                        if (!label.isEmpty()) { label += QStringLiteral("   "); }
                        label += QStringLiteral("WIDE");
                    }
                    int x = specRect.right() - DBM_STRIP_W - 8 - fm.horizontalAdvance(label);
                    p.drawText(x, y, label);

                    // Bounding rect of the full strip — used to suppress
                    // single-click-to-tune when clicking on these indicators (#1564).
                    m_indicatorStripRect = QRect(x, y - fm.ascent(),
                                                 fm.horizontalAdvance(label),
                                                 fm.height());

                    // Store click rect for the prop portion only
                    if (showProp) {
                        QString propText = QString("K%1  A%2  SFI %3")
                            .arg(m_propKIndex, 0, 'f', 2)
                            .arg(m_propAIndex)
                            .arg(m_propSfi);
                        int propW = fm.horizontalAdvance(propText);
                        m_propClickRect = QRect(x, y - fm.ascent(), propW, fm.height());
                    } else {
                        m_propClickRect = QRect();
                    }
                } else {
                    m_indicatorStripRect = QRect();
                }

                // MQTT device status overlay (#699)
                if (!m_mqttDisplayValues.isEmpty()) {
                    QFont mqttFont(p.font().family(), 12, QFont::Bold);
                    p.setFont(mqttFont);
                    p.setPen(QColor(0x80, 0xd0, 0xff, 200));
                    const QFontMetrics fm2(mqttFont);
                    QString mqttLabel;
                    for (auto it = m_mqttDisplayValues.constBegin();
                         it != m_mqttDisplayValues.constEnd(); ++it) {
                        if (!mqttLabel.isEmpty()) { mqttLabel += QStringLiteral("   "); }
                        mqttLabel += it.key() + QStringLiteral(": ") + it.value();
                    }
                    int mx = specRect.right() - DBM_STRIP_W - 8 - fm2.horizontalAdvance(mqttLabel);
                    int my = specRect.top() + fm2.ascent() + 22;
                    p.drawText(mx, my, mqttLabel);
                }
            }

            // Cursor frequency label (#726)
            if (m_showCursorFreq && m_hoveredTnfId < 0
                && m_cursorPos.x() >= 0
                && m_cursorPos.y() >= 0) {
                double freqMhz = xToMhz(m_cursorPos.x());
                // Snap to the exact VFO frequency when hovering within 8 px of a slice
                // marker so the readout exactly matches the flag value (#1369).
                for (const auto& so : m_sliceOverlays) {
                    if (std::abs(m_cursorPos.x() - mhzToX(so.freqMhz)) <= 8) {
                        freqMhz = so.freqMhz;
                        break;
                    }
                }
                const QString label = QString::number(freqMhz, 'f', 6);
                QFont f = p.font();
                f.setPointSize(9);
                p.setFont(f);
                const QFontMetrics fm(f);
                const int tw = fm.horizontalAdvance(label) + 8;
                const int th = fm.height() + 4;
                int lx = m_cursorPos.x() + 12;
                if (lx + tw > w) lx = m_cursorPos.x() - tw - 4;
                int ly = m_cursorPos.y() - th - 4;
                if (ly < 0) ly = m_cursorPos.y() + 16;
                p.fillRect(lx, ly, tw, th, QColor(0x0f, 0x0f, 0x1a, 200));
                p.setPen(QColor(0xc8, 0xd8, 0xe8));
                p.drawText(lx + 4, ly + fm.ascent() + 2, label);
            }

            // Tune guide overlay (vertical line + frequency label)
            if (m_showTuneGuides && m_hoveredTnfId < 0 && m_tuneGuideVisible
                && m_cursorPos.x() >= 0 && m_cursorPos.y() >= 0) {
                const int cx = m_cursorPos.x();
                p.setPen(QPen(QColor(0x60, 0x70, 0x80), 1));
                p.drawLine(cx, 0, cx, h);

                const double freqMhz = xToMhz(cx);
                long long hz = static_cast<long long>(std::round(freqMhz * 1e6));
                int mhzPart = static_cast<int>(hz / 1000000);
                int khzPart = static_cast<int>((hz / 1000) % 1000);
                int hzPart  = static_cast<int>(hz % 1000);
                const QString label = QString("%1.%2.%3")
                    .arg(mhzPart)
                    .arg(khzPart, 3, 10, QChar('0'))
                    .arg(hzPart, 3, 10, QChar('0'));
                QFont f = p.font();
                f.setPointSize(12);
                p.setFont(f);
                const QFontMetrics fm(f);
                const int tw = fm.horizontalAdvance(label) + 8;
                const int th = fm.height() + 4;
                int lx = cx + 12;
                if (lx + tw > w) { lx = cx - tw - 4; }
                int ly = m_cursorPos.y() - th - 4;
                if (ly < 0) { ly = m_cursorPos.y() + 16; }
                p.fillRect(lx, ly, tw, th, QColor(0x0f, 0x0f, 0x1a, 200));
                p.setPen(QColor(0xc8, 0xd8, 0xe8));
                p.drawText(lx + 4, ly + fm.ascent() + 2, label);
            }

            drawConnectionAnimation(p, specRect);

            m_overlayStaticDirty = false;
            m_overlayNeedsUpload = true;
        }

        // Upload overlay texture only when content changed
        if (m_overlayNeedsUpload) {
            QRhiTextureSubresourceUploadDescription ovDesc(m_overlayStatic);
            batch->uploadTexture(m_ovGpuTex, QRhiTextureUploadEntry(0, 0, ovDesc));
            m_overlayNeedsUpload = false;
        }

        // Generate FFT spectrum vertices with baked colors
        if (!m_smoothed.isEmpty() && m_fftLineVbo && m_fftFillVbo) {
            const int n = qMin(m_smoothed.size(), kMaxFftBins);
            const float minDbm = m_refLevel - m_dynamicRange;
            const float maxDbm = m_refLevel;
            const float range = maxDbm - minDbm;
            const float yBot = -1.0f;
            const float yTop = 1.0f;

            // Colors from settings
            const float fr = m_fftFillColor.redF();
            const float fg = m_fftFillColor.greenF();
            const float fb = m_fftFillColor.blueF();
            const float fa = m_fftFillAlpha;

            // Solid fill: slider sweeps from translucent gradient to solid.
            // At low slider: soft glow under curve (bright top, dark faint base)
            // At high slider: converges to uniform solid fill color
            const QColor dk = m_fftFillColor.darker(300);
            const float topAlpha = fa;
            const float botAlpha = fa * fa;
            // Blend bottom color from darker(300) toward fill color as slider increases
            const float colorBlend = fa;  // 0=full dark, 1=same as top
            const float dr = fr + (1.0f - colorBlend) * (dk.redF() - fr);
            const float dg = fg + (1.0f - colorBlend) * (dk.greenF() - fg);
            const float db = fb + (1.0f - colorBlend) * (dk.blueF() - fb);
            const float gradRange = yTop - yBot;

            auto yColor = [&](float vy, float* out) {
                const float gt = (gradRange > 0)
                    ? qBound(0.0f, (yTop - vy) / gradRange, 1.0f) : 0.0f;
                out[0] = fr + gt * (dr - fr);
                out[1] = fg + gt * (dg - fg);
                out[2] = fb + gt * (db - fb);
                out[3] = topAlpha + gt * (botAlpha - topAlpha);
            };

            // Line vertices: 2N × (x, y, r, g, b, a) — triangle strip expansion
            // for variable-width lines on GPU (LineStrip is fixed at 1px)
            QVector<float> lineVerts(n * 2 * kFftVertStride);
            // Fill vertices: 2N × (x, y, r, g, b, a)
            QVector<float> fillVerts(n * 2 * kFftVertStride);

            // Pre-compute positions for normal calculation
            struct Pt { float x, y; };
            QVector<Pt> pts(n);
            for (int i = 0; i < n; ++i) {
                pts[i].x = 2.0f * i / (n - 1) - 1.0f;
                float t = qBound(0.0f, (m_smoothed[i] - minDbm) / range, 1.0f);
                pts[i].y = yBot + t * (yTop - yBot);
            }

            // Half-width in NDC — convert pixel width to NDC using viewport
            const float halfW = m_fftLineWidth / static_cast<float>(qMax(1, width()));

            for (int i = 0; i < n; ++i) {
                float t = qBound(0.0f, (m_smoothed[i] - minDbm) / range, 1.0f);

                // Compute perpendicular normal from adjacent points
                float dx, dy;
                if (i == 0) {
                    dx = pts[1].x - pts[0].x;
                    dy = pts[1].y - pts[0].y;
                } else if (i == n - 1) {
                    dx = pts[n-1].x - pts[n-2].x;
                    dy = pts[n-1].y - pts[n-2].y;
                } else {
                    dx = pts[i+1].x - pts[i-1].x;
                    dy = pts[i+1].y - pts[i-1].y;
                }
                float len = std::sqrt(dx * dx + dy * dy);
                if (len < 1e-8f) len = 1e-8f;
                float nx = -dy / len * halfW;
                float ny =  dx / len * halfW;

                // Per-vertex color
                float cr, cg, cb2;
                if (m_fftHeatMap) {
                    if (t < 0.25f) {
                        float s = t / 0.25f;
                        cr = 0.0f; cg = s; cb2 = 1.0f;
                    } else if (t < 0.5f) {
                        float s = (t - 0.25f) / 0.25f;
                        cr = 0.0f; cg = 1.0f; cb2 = 1.0f - s;
                    } else if (t < 0.75f) {
                        float s = (t - 0.5f) / 0.25f;
                        cr = s; cg = 1.0f; cb2 = 0.0f;
                    } else {
                        float s = (t - 0.75f) / 0.25f;
                        cr = 1.0f; cg = 1.0f - s; cb2 = 0.0f;
                    }
                } else {
                    cr = fr; cg = fg; cb2 = fb;
                }

                // Two vertices per point: offset ± normal
                int li = i * 2 * kFftVertStride;
                lineVerts[li]     = pts[i].x + nx;
                lineVerts[li + 1] = pts[i].y + ny;
                lineVerts[li + 2] = cr;
                lineVerts[li + 3] = cg;
                lineVerts[li + 4] = cb2;
                lineVerts[li + 5] = 0.9f;
                lineVerts[li + 6]  = pts[i].x - nx;
                lineVerts[li + 7]  = pts[i].y - ny;
                lineVerts[li + 8]  = cr;
                lineVerts[li + 9]  = cg;
                lineVerts[li + 10] = cb2;
                lineVerts[li + 11] = 0.9f;

                // Fill vertices
                int fi = i * 2 * kFftVertStride;
                fillVerts[fi]     = pts[i].x;
                fillVerts[fi + 1] = pts[i].y;
                fillVerts[fi + 6] = pts[i].x;
                fillVerts[fi + 7] = yBot;

                if (m_fftHeatMap) {
                    // Heatmap: line color at top, fade to dark blue at base
                    fillVerts[fi + 2] = cr;
                    fillVerts[fi + 3] = cg;
                    fillVerts[fi + 4] = cb2;
                    fillVerts[fi + 5] = fa * 0.3f;
                    fillVerts[fi + 8]  = 0.0f;
                    fillVerts[fi + 9]  = 0.0f;
                    fillVerts[fi + 10] = 0.3f;
                    fillVerts[fi + 11] = fa;
                } else {
                    // Solid: Y-based gradient (bright at line, dark+faint at base)
                    yColor(pts[i].y, &fillVerts[fi + 2]);
                    yColor(yBot, &fillVerts[fi + 8]);
                }
            }

            batch->updateDynamicBuffer(m_fftLineVbo, 0,
                n * 2 * kFftVertStride * sizeof(float), lineVerts.constData());
            batch->updateDynamicBuffer(m_fftFillVbo, 0,
                n * 2 * kFftVertStride * sizeof(float), fillVerts.constData());
        }
    }

    cb->resourceUpdate(batch);

    // Begin render pass
    const QColor clearColor(0x0a, 0x0a, 0x14);
    cb->beginPass(renderTarget(), clearColor, {1.0f, 0});

    const QSize outputSize = renderTarget()->pixelSize();
    const float dpr = outputSize.width() / static_cast<float>(qMax(1, w));

    // Draw waterfall quad — viewport restricted to waterfall rect
    if (m_wfPipeline) {
        cb->setGraphicsPipeline(m_wfPipeline);
        cb->setShaderResources(m_wfSrb);
        // QRhiViewport: (x, y, width, height) — y is bottom-up in GL convention
        float vpX = static_cast<float>(wfRect.x()) * dpr;
        float vpY = static_cast<float>(h - wfRect.bottom() - 1) * dpr;
        float vpW = static_cast<float>(wfRect.width()) * dpr;
        float vpH = static_cast<float>(wfRect.height()) * dpr;

        cb->setViewport({vpX, vpY, vpW, vpH});
        const QRhiCommandBuffer::VertexInput vbuf(m_wfVbo, 0);
        cb->setVertexInput(0, 1, &vbuf);
        cb->draw(4);
    }

    // Draw FFT spectrum — viewport restricted to spectrum rect
    if (m_fftFillPipeline && m_fftLinePipeline && !m_smoothed.isEmpty()) {
        const int n = qMin(m_smoothed.size(), kMaxFftBins);
        float specVpX = static_cast<float>(specRect.x()) * dpr;
        float specVpY = static_cast<float>(h - specRect.bottom() - 1) * dpr;
        float specVpW = static_cast<float>(specRect.width()) * dpr;
        float specVpH = static_cast<float>(specRect.height()) * dpr;
        QRhiViewport specVp(specVpX, specVpY, specVpW, specVpH);

        // Fill pass
        cb->setGraphicsPipeline(m_fftFillPipeline);
        cb->setShaderResources(m_fftSrb);
        cb->setViewport(specVp);
        const QRhiCommandBuffer::VertexInput fillVbuf(m_fftFillVbo, 0);
        cb->setVertexInput(0, 1, &fillVbuf);
        cb->draw(n * 2);

        // Line pass (skip when line width is 0 = "Off")
        if (m_fftLineWidth > 0.0f) {
            cb->setGraphicsPipeline(m_fftLinePipeline);
            cb->setShaderResources(m_fftSrb);
            cb->setViewport(specVp);
            const QRhiCommandBuffer::VertexInput lineVbuf(m_fftLineVbo, 0);
            cb->setVertexInput(0, 1, &lineVbuf);
            cb->draw(n * 2);
        }
    }

    // Draw overlay quad — on top of FFT fill/line
    if (m_ovPipeline) {
        cb->setGraphicsPipeline(m_ovPipeline);
        cb->setShaderResources(m_ovSrb);
        cb->setViewport({0, 0,
            static_cast<float>(outputSize.width()),
            static_cast<float>(outputSize.height())});
        const QRhiCommandBuffer::VertexInput vbuf(m_ovVbo, 0);
        cb->setVertexInput(0, 1, &vbuf);
        cb->draw(4);
    }

    cb->endPass();

    // Reposition VFO widgets. paintEvent() is compiled only in software mode
    // (#else !AETHER_GPU_SPECTRUM), so in GPU mode this is the sole place VFOs
    // are repositioned. Logic mirrors the paintEvent() block below exactly.
    {
        struct VfoPos { int sliceId; int x; VfoWidget* w; int splitPartner; };
        QVector<VfoPos> vfos;
        for (const auto& so : m_sliceOverlays) {
            if (auto* vw = m_vfoWidgets.value(so.sliceId, nullptr)) {
                int x = mhzToX(so.freqMhz);
                if (so.mode == "RTTY" || so.mode == "DIGL") {
                    double hiMhz = so.freqMhz + so.filterHighHz / 1.0e6;
                    x = mhzToX(hiMhz) + 4;
                }
                vfos.append({so.sliceId, x, vw, so.splitPartnerId});
            }
        }
        std::sort(vfos.begin(), vfos.end(), [](const VfoPos& a, const VfoPos& b) {
            return a.x < b.x;
        });

        const int panelW = vfos.isEmpty() ? 0 : vfos[0].w->width();
        const int specW  = specRect.width();

        QMap<int, VfoWidget::FlagDir> dirMap;
        for (int i = 0; i < vfos.size(); ++i) {
            if (vfos[i].splitPartner < 0) continue;
            if (dirMap.contains(vfos[i].sliceId)) continue;
            int pi = -1;
            for (int j = 0; j < vfos.size(); ++j) {
                if (vfos[j].sliceId == vfos[i].splitPartner) { pi = j; break; }
            }
            if (pi < 0) continue;
            int leftIdx  = (vfos[i].x <= vfos[pi].x) ? i : pi;
            int rightIdx = (leftIdx == i) ? pi : i;
            dirMap[vfos[leftIdx].sliceId]  = VfoWidget::ForceLeft;
            dirMap[vfos[rightIdx].sliceId] = VfoWidget::ForceRight;
            if (vfos[leftIdx].x < panelW)
                dirMap[vfos[leftIdx].sliceId] = VfoWidget::ForceRight;
            if (vfos[rightIdx].x + panelW > specW)
                dirMap[vfos[rightIdx].sliceId] = VfoWidget::ForceLeft;
        }

        for (const auto& so : m_sliceOverlays) {
            if (so.mode == "RTTY" || so.mode == "DIGL")
                dirMap[so.sliceId] = VfoWidget::ForceRight;
        }

        if (vfos.size() == 1) {
            VfoWidget::FlagDir dir = dirMap.value(vfos[0].sliceId, VfoWidget::Auto);
            vfos[0].w->updatePosition(vfos[0].x, specRect.top(), dir);
        } else {
            for (int i = 0; i < vfos.size(); ++i) {
                VfoWidget::FlagDir dir = VfoWidget::Auto;
                if (dirMap.contains(vfos[i].sliceId)) {
                    dir = dirMap[vfos[i].sliceId];
                } else if (vfos.size() == 2) {
                    dir = (i == 0) ? VfoWidget::ForceLeft : VfoWidget::ForceRight;
                    if (i == 0 && vfos[i].x < panelW) dir = VfoWidget::ForceRight;
                    if (i == 1 && vfos[i].x + panelW > specW) dir = VfoWidget::ForceLeft;
                } else {
                    if (i == 0) {
                        dir = VfoWidget::ForceLeft;
                        if (vfos[i].x < panelW) dir = VfoWidget::ForceRight;
                    } else if (i == vfos.size() - 1) {
                        dir = VfoWidget::ForceRight;
                        if (vfos[i].x + panelW > specW) dir = VfoWidget::ForceLeft;
                    } else {
                        const int gapLeft  = vfos[i].x - vfos[i-1].x;
                        const int gapRight = vfos[i+1].x - vfos[i].x;
                        dir = (gapLeft >= gapRight) ? VfoWidget::ForceLeft : VfoWidget::ForceRight;
                    }
                }
                vfos[i].w->updatePosition(vfos[i].x, specRect.top(), dir);
            }
        }
    }
}

void SpectrumWidget::render(QRhiCommandBuffer* cb)
{
    if (!m_rhiInitialized) {
        initialize(cb);
        if (!m_rhiInitialized) return;
    }
    renderGpuFrame(cb);
}

void SpectrumWidget::releaseResources()
{
    delete m_wfPipeline;     m_wfPipeline = nullptr;
    delete m_wfSrb;          m_wfSrb = nullptr;
    delete m_wfVbo;          m_wfVbo = nullptr;
    delete m_wfUbo;          m_wfUbo = nullptr;
    delete m_wfGpuTex;       m_wfGpuTex = nullptr;
    delete m_wfSampler;      m_wfSampler = nullptr;

    delete m_ovPipeline;     m_ovPipeline = nullptr;
    delete m_ovSrb;          m_ovSrb = nullptr;
    delete m_ovVbo;          m_ovVbo = nullptr;
    delete m_ovGpuTex;       m_ovGpuTex = nullptr;
    delete m_ovSampler;      m_ovSampler = nullptr;

    delete m_fftLinePipeline; m_fftLinePipeline = nullptr;
    delete m_fftFillPipeline; m_fftFillPipeline = nullptr;
    delete m_fftSrb;          m_fftSrb = nullptr;
    delete m_fftLineVbo;      m_fftLineVbo = nullptr;
    delete m_fftFillVbo;      m_fftFillVbo = nullptr;

    m_rhiInitialized = false;
    qDebug() << "SpectrumWidget: QRhi resources released";
}

#else // !AETHER_GPU_SPECTRUM

void SpectrumWidget::paintEvent(QPaintEvent* ev)
{
    if (width() <= 0 || height() <= FREQ_SCALE_H + DIVIDER_H + 2) return;

#ifdef AETHER_GPU_SPECTRUM
    // GPU mode: render() handles everything via QRhi. Skip the full
    // QPainter path to avoid redundant rendering + compositing overhead.
    // This is the single biggest CPU optimization on macOS (100% → 20%).
    if (m_rhiInitialized) {
        SPECTRUM_BASE_CLASS::paintEvent(ev);
        return;
    }
#endif
    Q_UNUSED(ev);

    QElapsedTimer frameTimer;
    frameTimer.start();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const int chromeH  = FREQ_SCALE_H + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int specH    = static_cast<int>(contentH * m_spectrumFrac);
    const int wfH      = contentH - specH;

    const int divY     = specH;
    const int scaleY   = specH + DIVIDER_H;
    const int wfY      = scaleY + FREQ_SCALE_H;

    const QRect specRect (0, 0,       width(), specH);
    const QRect divRect  (0, divY,    width(), DIVIDER_H);
    const QRect scaleRect(0, scaleY,  width(), FREQ_SCALE_H);
    const QRect wfRect   (0, wfY,     width(), wfH);

    {
        // Software fallback: full QPainter rendering
        if (m_bgImage.isNull()) {
            p.fillRect(specRect, QColor(0x0a, 0x0a, 0x14));
        } else {
            // Cache scaled image to avoid per-frame scaling
            if (m_bgScaledSize != specRect.size()) {
                // Scale to cover (keep aspect ratio, expand to fill) then crop to fit
                QImage expanded = m_bgImage.scaled(specRect.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                int cx = (expanded.width()  - specRect.width())  / 2;
                int cy = (expanded.height() - specRect.height()) / 2;
                m_bgScaled = expanded.copy(cx, cy, specRect.width(), specRect.height());
                m_bgScaledSize = specRect.size();
            }
            p.drawImage(specRect.topLeft(), m_bgScaled);
            // Dark overlay to mute the image (0%=full image, 100%=solid dark)
            int alpha = m_bgOpacity * 255 / 100;
            p.fillRect(specRect, QColor(0x0a, 0x0a, 0x14, alpha));
        }
        drawGrid(p, specRect);
        drawSpectrum(p, specRect);
        if (m_bandPlanFontSize > 0) drawBandPlan(p, specRect);
        drawDbmScale(p, specRect);

        p.fillRect(divRect, QColor(0x18, 0x28, 0x38));
        p.setPen(QColor(m_draggingDivider ? 0x00b4d8 : 0x304050));
        p.drawLine(divRect.left(), divRect.center().y(), divRect.right(), divRect.center().y());

        drawFreqScale(p, scaleRect);
        drawWaterfall(p, wfRect);
        drawTimeScale(p, wfRect);
        drawTnfMarkers(p, specRect);
        if (m_showSpots || m_showSHistory) drawSpotMarkers(p, specRect);
        drawSwrSweep(p, specRect);
        drawSliceMarkers(p, specRect, wfRect);
        drawOffScreenSlices(p, specRect);
        drawConnectionAnimation(p, specRect);
    }

    // Reposition all VFO widgets — deconflict flags so they fly away from each other
    // Split pairs always face each other: RX←  →TX
    {
        // Collect visible VFOs sorted by screen X position
        struct VfoPos { int sliceId; int x; VfoWidget* w; int splitPartner; };
        QVector<VfoPos> vfos;
        for (const auto& so : m_sliceOverlays) {
            if (auto* w = m_vfoWidgets.value(so.sliceId, nullptr)) {
                int x = mhzToX(so.freqMhz);
                // In RTTY/DIGL, anchor the flag past the filter high edge
                // so it doesn't cover the mark/space passband.
                if (so.mode == "RTTY" || so.mode == "DIGL") {
                    double hiMhz = so.freqMhz + so.filterHighHz / 1.0e6;
                    x = mhzToX(hiMhz) + 4;  // 4px padding past filter edge
                }
                vfos.append({so.sliceId, x, w, so.splitPartnerId});
            }
        }
        std::sort(vfos.begin(), vfos.end(), [](const VfoPos& a, const VfoPos& b) {
            return a.x < b.x;
        });

        const int panelW = vfos.isEmpty() ? 0 : vfos[0].w->width();
        const int specW = specRect.width();

        // First pass: assign directions for split pairs
        QMap<int, VfoWidget::FlagDir> dirMap;  // sliceId → direction
        for (int i = 0; i < vfos.size(); ++i) {
            if (vfos[i].splitPartner < 0) continue;
            if (dirMap.contains(vfos[i].sliceId)) continue;  // already assigned

            // Find partner index
            int pi = -1;
            for (int j = 0; j < vfos.size(); ++j) {
                if (vfos[j].sliceId == vfos[i].splitPartner) { pi = j; break; }
            }
            if (pi < 0) continue;

            // Left partner flies left, right partner flies right
            int leftIdx  = (vfos[i].x <= vfos[pi].x) ? i : pi;
            int rightIdx = (leftIdx == i) ? pi : i;
            dirMap[vfos[leftIdx].sliceId]  = VfoWidget::ForceLeft;
            dirMap[vfos[rightIdx].sliceId] = VfoWidget::ForceRight;

            // Edge clamp
            if (vfos[leftIdx].x < panelW)
                dirMap[vfos[leftIdx].sliceId] = VfoWidget::ForceRight;
            if (vfos[rightIdx].x + panelW > specW)
                dirMap[vfos[rightIdx].sliceId] = VfoWidget::ForceLeft;
        }

        // Second pass: assign remaining (non-split) VFOs
        // In RTTY/DIGL, force flag to fly right so it doesn't cover M/S passband
        for (const auto& so : m_sliceOverlays) {
            if (so.mode == "RTTY" || so.mode == "DIGL")
                dirMap[so.sliceId] = VfoWidget::ForceRight;
        }

        if (vfos.size() == 1) {
            VfoWidget::FlagDir dir = dirMap.value(vfos[0].sliceId, VfoWidget::Auto);
            vfos[0].w->updatePosition(vfos[0].x, specRect.top(), dir);
        } else {
            for (int i = 0; i < vfos.size(); ++i) {
                VfoWidget::FlagDir dir = VfoWidget::Auto;

                if (dirMap.contains(vfos[i].sliceId)) {
                    // Split pair or RTTY: use pre-assigned direction
                    dir = dirMap[vfos[i].sliceId];
                } else if (vfos.size() == 2) {
                    dir = (i == 0) ? VfoWidget::ForceLeft : VfoWidget::ForceRight;
                    if (i == 0 && vfos[i].x < panelW) dir = VfoWidget::ForceRight;
                    if (i == 1 && vfos[i].x + panelW > specW) dir = VfoWidget::ForceLeft;
                } else {
                    if (i == 0) {
                        dir = VfoWidget::ForceLeft;
                        if (vfos[i].x < panelW) dir = VfoWidget::ForceRight;
                    } else if (i == vfos.size() - 1) {
                        dir = VfoWidget::ForceRight;
                        if (vfos[i].x + panelW > specW) dir = VfoWidget::ForceLeft;
                    } else {
                        int gapLeft = vfos[i].x - vfos[i-1].x;
                        int gapRight = vfos[i+1].x - vfos[i].x;
                        dir = (gapLeft >= gapRight) ? VfoWidget::ForceLeft : VfoWidget::ForceRight;
                    }
                }

                vfos[i].w->updatePosition(vfos[i].x, specRect.top(), dir);
            }
        }
    }
    // Active widget on top, but overlay stays above all
    if (m_vfoWidget) {
        m_vfoWidget->raise();
        m_overlayMenu->raiseAll();
    }

    // ── WNB / RF Gain / Prop Forecast indicators (top-right of FFT area) ────
    {
        const bool showProp = m_propForecastVisible
            && m_propKIndex >= 0
            && m_propAIndex >= 0
            && m_propSfi > 0;
        if (m_wnbActive || m_rfGainValue != 0 || showProp || m_wideActive) {
            QFont indFont = p.font();
            indFont.setPointSize(18);
            indFont.setBold(true);
            p.setFont(indFont);
            p.setPen(QColor(255, 255, 255, 84));

            const QFontMetrics fm(indFont);
            const int rightEdge = specRect.right() - DBM_STRIP_W - 6;
            const int topY = specRect.top() + fm.ascent() + 2;

            int x = rightEdge;

            // WIDE (rightmost)
            if (m_wideActive) {
                int ww = fm.horizontalAdvance("WIDE");
                x -= ww;
                p.drawText(x, topY, "WIDE");
                x -= 10;
            }

            // RF Gain (to the left of WIDE)
            if (m_rfGainValue != 0) {
                QString gainStr = (m_rfGainValue > 0)
                    ? QString("+%1dB").arg(m_rfGainValue)
                    : QString("%1dB").arg(m_rfGainValue);
                int gw = fm.horizontalAdvance(gainStr);
                x -= gw;
                p.drawText(x, topY, gainStr);
                x -= 10;  // gap between labels
            }

            // WNB (to the left of RF Gain)
            if (m_wnbActive) {
                int ww = fm.horizontalAdvance("WNB");
                x -= ww;
                p.drawText(x, topY, "WNB");
                x -= 10;
            }

            // Prop forecast (leftmost: "K3  A12  SFI 110")
            if (showProp) {
                QString propStr = QString("K%1  A%2  SFI %3")
                    .arg(m_propKIndex, 0, 'f', 2)
                    .arg(m_propAIndex)
                    .arg(m_propSfi);
                int pw = fm.horizontalAdvance(propStr);
                x -= pw;
                p.drawText(x, topY, propStr);
                m_propClickRect = QRect(x, topY - fm.ascent(), pw, fm.height());
            } else {
                m_propClickRect = QRect();
            }

            // Bounding rect of the full strip (prop + WNB + RF Gain + WIDE) —
            // used to suppress single-click-to-tune within (#1564).
            m_indicatorStripRect = QRect(x, topY - fm.ascent(),
                                         rightEdge - x, fm.height());
        } else {
            m_indicatorStripRect = QRect();
        }
    }

    // ── Cursor frequency label (#456) ──────────────────────────────────────
    if (m_showCursorFreq && m_hoveredTnfId < 0
        && m_cursorPos.x() >= 0
        && m_cursorPos.y() >= 0) {
        double freqMhz = xToMhz(m_cursorPos.x());
        // Snap to the exact VFO frequency when hovering within 8 px of a slice
        // marker so the readout exactly matches the flag value (#1369).
        for (const auto& so : m_sliceOverlays) {
            if (std::abs(m_cursorPos.x() - mhzToX(so.freqMhz)) <= 8) {
                freqMhz = so.freqMhz;
                break;
            }
        }
        const QString label = QString::number(freqMhz, 'f', 6);
        QFont f = p.font();
        f.setPointSize(9);
        p.setFont(f);
        const QFontMetrics fm(f);
        const int tw = fm.horizontalAdvance(label) + 8;
        const int th = fm.height() + 4;
        // Position label to the right of cursor, flip left if near right edge
        int lx = m_cursorPos.x() + 12;
        if (lx + tw > width()) lx = m_cursorPos.x() - tw - 4;
        int ly = m_cursorPos.y() - th - 4;
        if (ly < 0) ly = m_cursorPos.y() + 16;
        p.fillRect(lx, ly, tw, th, QColor(0x0f, 0x0f, 0x1a, 200));
        p.setPen(QColor(0xc8, 0xd8, 0xe8));
        p.drawText(lx + 4, ly + fm.ascent() + 2, label);
    }

    // ── Tune guide overlay (vertical line + frequency label) ──────────────
    if (m_showTuneGuides && m_hoveredTnfId < 0 && m_tuneGuideVisible
        && m_cursorPos.x() >= 0 && m_cursorPos.y() >= 0) {
        const int cx = m_cursorPos.x();
        p.setPen(QPen(QColor(0x60, 0x70, 0x80), 1));
        p.drawLine(cx, 0, cx, height());

        const double freqMhz = xToMhz(cx);
        long long hz = static_cast<long long>(std::round(freqMhz * 1e6));
        int mhzPart = static_cast<int>(hz / 1000000);
        int khzPart = static_cast<int>((hz / 1000) % 1000);
        int hzPart  = static_cast<int>(hz % 1000);
        const QString label = QString("%1.%2.%3")
            .arg(mhzPart)
            .arg(khzPart, 3, 10, QChar('0'))
            .arg(hzPart, 3, 10, QChar('0'));
        QFont f = p.font();
        f.setPointSize(12);
        p.setFont(f);
        const QFontMetrics fm(f);
        const int tw = fm.horizontalAdvance(label) + 8;
        const int th = fm.height() + 4;
        int lx = cx + 12;
        if (lx + tw > width()) { lx = cx - tw - 4; }
        int ly = m_cursorPos.y() - th - 4;
        if (ly < 0) { ly = m_cursorPos.y() + 16; }
        p.fillRect(lx, ly, tw, th, QColor(0x0f, 0x0f, 0x1a, 200));
        p.setPen(QColor(0xc8, 0xd8, 0xe8));
        p.drawText(lx + 4, ly + fm.ascent() + 2, label);
    }

    qCDebug(lcPerf) << "paintEvent:" << static_cast<int>(frameTimer.elapsed()) << "ms";
}

#endif // AETHER_GPU_SPECTRUM

// ─── Grid ─────────────────────────────────────────────────────────────────────

// Compute the effective frequency grid step in MHz, honouring user override (#1390).
// When m_freqGridSpacingKhz is 0 (Auto), uses the 1-2-5 sequence for ~5 grid lines.
// When a manual value is set, clamps up to the next valid option if labels would overlap.
double SpectrumWidget::effectiveGridStepMhz(int widgetWidth) const
{
    // 1-2-5 auto algorithm
    auto autoStep = [&]() {
        const double rawStep = m_bandwidthMhz / 5.0;
        const double mag = std::pow(10.0, std::floor(std::log10(rawStep)));
        const double norm = rawStep / mag;
        if      (norm >= 5.0) return 5.0 * mag;
        else if (norm >= 2.0) return 2.0 * mag;
        else                  return 1.0 * mag;
    };

    if (m_freqGridSpacingKhz <= 0)
        return autoStep();

    // Manual spacing — always respect the user's choice for grid lines.
    // Labels are thinned separately in drawFreqScale() to prevent overlap.
    return m_freqGridSpacingKhz * 0.001;
}

void SpectrumWidget::drawGrid(QPainter& p, const QRect& r)
{
    if (!m_showGrid) return;
    const int w = r.width();
    const int h = r.height();

    // Horizontal dB grid lines — adaptive step matching the dBm scale strip
    float rawDbStep = m_dynamicRange / 5.0f;
    float dbStep;
    if      (rawDbStep >= 20.0f) dbStep = 20.0f;
    else if (rawDbStep >= 10.0f) dbStep = 10.0f;
    else if (rawDbStep >= 5.0f)  dbStep = 5.0f;
    else                          dbStep = 2.0f;

    const float bottomDbm = m_refLevel - m_dynamicRange;
    const float firstDb = std::ceil(bottomDbm / dbStep) * dbStep;
    p.setPen(QPen(QColor(0x20, 0x30, 0x40), 1, Qt::DotLine));
    for (float dbm = firstDb; dbm <= m_refLevel; dbm += dbStep) {
        const float frac = (m_refLevel - dbm) / m_dynamicRange;
        const int y = r.top() + static_cast<int>(frac * h);
        p.drawLine(0, y, w, y);
    }

    // Vertical frequency grid lines (#1390: honour user spacing override)
    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double endMhz   = m_centerMhz + m_bandwidthMhz / 2.0;
    const double gridStep = effectiveGridStepMhz(w);
    const double firstLine = std::ceil(startMhz / gridStep) * gridStep;

    p.setPen(QPen(QColor(0x20, 0x30, 0x40), 1, Qt::DotLine));
    for (double f = firstLine; f <= endMhz; f += gridStep)
        p.drawLine(mhzToX(f), r.top(), mhzToX(f), r.bottom());
}

// ─── Spectrum line ────────────────────────────────────────────────────────────

void SpectrumWidget::drawSpectrum(QPainter& p, const QRect& r)
{
    if (m_smoothed.isEmpty()) {
        p.setPen(QColor(0x00, 0x60, 0x80));
        p.drawText(r, Qt::AlignCenter, "No panadapter data — waiting for radio stream");
        return;
    }

    const int w = r.width();
    const int h = r.height();
    const int n = m_smoothed.size();

    // Heat map: blue(0) → cyan(0.25) → green(0.5) → yellow(0.75) → red(1.0)
    auto heatColor = [](float t) -> QColor {
        float cr, cg, cb;
        if (t < 0.25f) {
            float s = t / 0.25f;
            cr = 0.0f; cg = s; cb = 1.0f;
        } else if (t < 0.5f) {
            float s = (t - 0.25f) / 0.25f;
            cr = 0.0f; cg = 1.0f; cb = 1.0f - s;
        } else if (t < 0.75f) {
            float s = (t - 0.5f) / 0.25f;
            cr = s; cg = 1.0f; cb = 0.0f;
        } else {
            float s = (t - 0.75f) / 0.25f;
            cr = 1.0f; cg = 1.0f - s; cb = 0.0f;
        }
        return QColor::fromRgbF(cr, cg, cb);
    };

    // Pre-compute positions and normalized levels
    struct Pt { int x, y; float t; };
    QVector<Pt> pts(n);
    for (int i = 0; i < n; ++i) {
        const float dbm  = m_smoothed[i];
        const float norm = qBound(0.0f, (m_refLevel - dbm) / m_dynamicRange, 1.0f);
        pts[i].x = r.left() + static_cast<int>(static_cast<float>(i) / n * w);
        pts[i].y = r.top()  + qMin(static_cast<int>(norm * h), h - 1);
        pts[i].t = 1.0f - norm;  // 0=noise floor, 1=strong signal
    }

    p.setRenderHint(QPainter::Antialiasing, true);

    if (m_fftHeatMap) {
        // Heat map fill: per-column vertical gradient from heat color at top to dark blue at base
        const int bottom = r.bottom();
        for (int i = 0; i < n - 1; ++i) {
            QPolygonF trapezoid;
            trapezoid << QPointF(pts[i].x, pts[i].y)
                      << QPointF(pts[i+1].x, pts[i+1].y)
                      << QPointF(pts[i+1].x, bottom)
                      << QPointF(pts[i].x, bottom);

            float avgT = (pts[i].t + pts[i+1].t) * 0.5f;
            QColor top = heatColor(avgT);
            top.setAlphaF(m_fftFillAlpha * 0.3f);
            QColor bot(0, 0, 77, static_cast<int>(255 * m_fftFillAlpha));
            QLinearGradient grad(0, qMin(pts[i].y, pts[i+1].y), 0, bottom);
            grad.setColorAt(0.0, top);
            grad.setColorAt(1.0, bot);
            p.setPen(Qt::NoPen);
            p.setBrush(grad);
            p.drawPolygon(trapezoid);
        }

        // Heat map line: per-segment coloring
        for (int i = 0; i < n - 1; ++i) {
            float avgT = (pts[i].t + pts[i+1].t) * 0.5f;
            p.setPen(QPen(heatColor(avgT), 1.5));
            p.drawLine(pts[i].x, pts[i].y, pts[i+1].x, pts[i+1].y);
        }
    } else {
        // Solid color fill + line
        QPainterPath linePath;
        linePath.moveTo(pts[0].x, pts[0].y);
        for (int i = 1; i < n; ++i) {
            linePath.lineTo(pts[i].x, pts[i].y);
        }

        QPainterPath fillPath(linePath);
        fillPath.lineTo(r.right(), r.bottom());
        fillPath.lineTo(r.left(),  r.bottom());
        fillPath.closeSubpath();

        const int alphaTop = static_cast<int>(200 * m_fftFillAlpha);
        const int alphaBot = static_cast<int>(60 * m_fftFillAlpha);
        QColor topColor(m_fftFillColor);
        topColor.setAlpha(alphaTop);
        QColor botColor = m_fftFillColor.darker(300);
        botColor.setAlpha(alphaBot);
        QLinearGradient grad(0, r.top(), 0, r.bottom());
        grad.setColorAt(0.0, topColor);
        grad.setColorAt(1.0, botColor);

        p.fillPath(fillPath, grad);
        p.setPen(QPen(m_fftFillColor, 1.5));
        p.drawPath(linePath);
    }

    p.setRenderHint(QPainter::Antialiasing, false);
}

// ─── Waterfall ────────────────────────────────────────────────────────────────

void SpectrumWidget::drawWaterfall(QPainter& p, const QRect& r)
{
    if (m_waterfall.isNull()) {
        p.fillRect(r, Qt::black);
        return;
    }

    // Ring buffer rendering: m_wfWriteRow is the newest row.
    // Draw in two halves: [writeRow..end] then [0..writeRow)
    const int h = m_waterfall.height();
    const int topRows = h - m_wfWriteRow;  // rows from writeRow to bottom of image
    const int botRows = m_wfWriteRow;       // rows from top of image to writeRow

    if (topRows >= h) {
        // writeRow == 0, no split needed
        p.drawImage(r, m_waterfall);
    } else {
        const double scale = static_cast<double>(r.height()) / h;
        const int topH = static_cast<int>(topRows * scale);
        const int botH = r.height() - topH;

        // Top part: newest rows (from writeRow to end of image)
        p.drawImage(QRect(r.x(), r.y(), r.width(), topH),
                    m_waterfall,
                    QRect(0, m_wfWriteRow, m_waterfall.width(), topRows));

        // Bottom part: older rows (from start of image to writeRow)
        if (botRows > 0 && botH > 0) {
            p.drawImage(QRect(r.x(), r.y() + topH, r.width(), botH),
                        m_waterfall,
                        QRect(0, 0, m_waterfall.width(), botRows));
        }
    }
}

// ─── Band plan overlay (bottom 8px of FFT area) ─────────────────────────────

void SpectrumWidget::drawBandPlan(QPainter& p, const QRect& specRect)
{
    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double endMhz   = m_centerMhz + m_bandwidthMhz / 2.0;
    const int bandH = m_bandPlanFontSize + 4;  // scale strip height with font
    const int bandY = specRect.bottom() - bandH + 1;

    const auto& segments = m_bandPlanMgr ? m_bandPlanMgr->segments()
                                         : QVector<BandPlanManager::Segment>{};
    for (const auto& seg : segments) {
        if (seg.highMhz <= startMhz || seg.lowMhz >= endMhz) continue;

        const int x1 = mhzToX(std::max(seg.lowMhz, startMhz));
        const int x2 = mhzToX(std::min(seg.highMhz, endMhz));
        if (x2 <= x1) continue;

        // License class contrast: Extra-only = dim, wider access = brighter.
        // Fully opaque — mix segment color with dark background to simulate
        // the old alpha look without letting FFT fill bleed through.
        const QString& lic = seg.license;
        float blend = 0.6f;  // how much of the segment color to show
        if (lic == "E")          blend = 0.20f;
        else if (lic == "E,G")   blend = 0.40f;
        else if (lic.contains("T")) blend = 0.60f;
        else if (lic.isEmpty())  blend = 0.50f;

        const QColor bg(0x0a, 0x0a, 0x14);  // dark background
        QColor fill(
            static_cast<int>(seg.color.red()   * blend + bg.red()   * (1.0f - blend)),
            static_cast<int>(seg.color.green() * blend + bg.green() * (1.0f - blend)),
            static_cast<int>(seg.color.blue()  * blend + bg.blue()  * (1.0f - blend)),
            255);
        p.fillRect(x1, bandY, x2 - x1, bandH, fill);

        // Draw separator lines between adjacent segments
        p.setPen(QColor(0x0f, 0x0f, 0x1a, 200));
        p.drawLine(x1, bandY, x1, bandY + bandH);

        // Label: mode + lowest license class allowed
        if (x2 - x1 > 20) {
            QFont f = p.font();
            f.setPointSize(m_bandPlanFontSize);
            f.setBold(true);
            p.setFont(f);

            // Determine lowest (least restrictive) license class
            QString lowestClass;
            if (lic.contains("T"))       lowestClass = "Tech";
            else if (lic.contains("G"))  lowestClass = "General";
            else if (lic == "E")         lowestClass = "Extra";

            QString label = seg.label;
            if (!lowestClass.isEmpty() && x2 - x1 > 60)
                label = QString("%1 %2").arg(seg.label, lowestClass);

            p.setPen(Qt::white);
            p.drawText(QRect(x1, bandY, x2 - x1, bandH),
                       Qt::AlignCenter, label);
        }
    }

    // Draw single-frequency spot markers (white circles)
    const auto& spots = m_bandPlanMgr ? m_bandPlanMgr->spots()
                                       : QVector<BandPlanManager::Spot>{};
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    for (const auto& spot : spots) {
        if (spot.freqMhz < startMhz || spot.freqMhz > endMhz) continue;
        const int sx = mhzToX(spot.freqMhz);
        p.drawEllipse(QPoint(sx, bandY + bandH / 2), 4, 4);
    }
    p.setRenderHint(QPainter::Antialiasing, false);
}

// ─── TNF markers ────────────────────────────────────────────────────────────

void SpectrumWidget::setTnfMarkers(const QVector<TnfMarker>& markers)
{
    m_tnfMarkers = markers;
    markOverlayDirty();
}

void SpectrumWidget::setSpotMarkers(const QVector<SpotMarker>& markers)
{
    m_spotMarkers = markers;
    markOverlayDirty();
}

void SpectrumWidget::setSHistoryMarkers(const QVector<SpotMarker>& markers)
{
    m_sHistoryMarkers = markers;
    markOverlayDirty();
}

void SpectrumWidget::setSwrSweepPoints(const QVector<SwrSweepPoint>& points,
                                       bool running,
                                       double currentFreqMhz,
                                       const QString& sourceLabel)
{
    m_swrSweepPoints = points;
    m_swrSweepRunning = running;
    m_swrSweepCurrentFreqMhz = currentFreqMhz;
    m_swrSweepSourceLabel = sourceLabel;
    markOverlayDirty();
}

void SpectrumWidget::clearSwrSweepPoints()
{
    m_swrSweepPoints.clear();
    m_swrSweepRunning = false;
    m_swrSweepCurrentFreqMhz = -1.0;
    m_swrSweepSourceLabel.clear();
    markOverlayDirty();
}

void SpectrumWidget::setTnfGlobalEnabled(bool on)
{
    m_tnfGlobalEnabled = on;
    markOverlayDirty();
}

void SpectrumWidget::drawTnfMarkers(QPainter& p, const QRect& specRect)
{
    if (m_tnfMarkers.isEmpty()) return;

    const auto drawDepthHatch = [&](const QRect& rect, const QColor& color, int left, int right, int spacing) {
        if (rect.isEmpty()) {
            return;
        }
        p.save();
        p.setClipRect(rect);
        p.setPen(QPen(color, 1));
        const int height = rect.height();
        for (int x = left - height; x < right; x += spacing) {
            p.drawLine(x, rect.bottom(), x + height, rect.top());
        }
        p.restore();
    };

    for (const auto& tnf : m_tnfMarkers) {
        const int cx = mhzToX(tnf.freqMhz);
        const int halfW = std::max(2, mhzToX(tnf.freqMhz + tnf.widthHz / 2.0e6) - cx);
        const int left = cx - halfW;
        const int right = cx + halfW;

        // Skip if fully off-screen
        if (right < 0 || left > width()) continue;

        // Permanent = green, temporary = yellow
        const QColor baseColor = tnfColor(tnf);
        const QColor fillColor = tnfFillColor(tnf);
        const QColor lineColor = tnfLineColor(tnf);
        p.fillRect(left, specRect.top(), right - left, specRect.height(), fillColor);
        const int hatchSpacing = (tnf.depthDb <= 1) ? 12 : (tnf.depthDb == 2 ? 8 : 5);
        drawDepthHatch(QRect(left, specRect.top(), right - left, specRect.height()), lineColor, left, right, hatchSpacing);

        // Edge lines
        const QPen edgePen(lineColor, 1, Qt::SolidLine);
        p.setPen(edgePen);
        p.drawLine(left, specRect.top(), left, specRect.bottom());
        p.drawLine(right, specRect.top(), right, specRect.bottom());

        // Center triangle (grab handle) at top of spectrum
        const int triH = 8 + tnf.depthDb * 2;  // bigger triangle for deeper notch
        QPolygon tri;
        tri << QPoint(cx - 5, specRect.top())
            << QPoint(cx + 5, specRect.top())
            << QPoint(cx, specRect.top() + triH);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(baseColor.red(), baseColor.green(), baseColor.blue(), m_tnfGlobalEnabled ? 200 : 80));
        p.drawPolygon(tri);
    }
}

const SpectrumWidget::TnfMarker* SpectrumWidget::tnfMarkerById(int id) const
{
    for (const auto& tnf : m_tnfMarkers) {
        if (tnf.id == id) {
            return &tnf;
        }
    }
    return nullptr;
}

QColor SpectrumWidget::tnfColor(const TnfMarker& tnf) const
{
    return tnf.permanent ? QColor(0x30, 0xc0, 0x30) : QColor(0xff, 0xc0, 0x00);
}

QColor SpectrumWidget::tnfFillColor(const TnfMarker& tnf) const
{
    const QColor baseColor = tnfColor(tnf);
    const int alpha = m_tnfGlobalEnabled ? 40 : 15;
    return QColor(baseColor.red(), baseColor.green(), baseColor.blue(), alpha);
}

QColor SpectrumWidget::tnfLineColor(const TnfMarker& tnf) const
{
    const QColor baseColor = tnfColor(tnf);
    const int alpha = m_tnfGlobalEnabled ? 160 : 70;
    const QColor bgBase(0x0f, 0x0f, 0x1a);
    const auto blend = [alpha](int bg, int fg) {
        return (bg * (255 - alpha) + fg * alpha) / 255;
    };
    return QColor(blend(bgBase.red(), baseColor.red()),
                  blend(bgBase.green(), baseColor.green()),
                  blend(bgBase.blue(), baseColor.blue()));
}

void SpectrumWidget::updateTnfHoverPopup()
{
    const TnfMarker* tnf = tnfMarkerById(m_hoveredTnfId);
    if (!tnf || m_cursorPos.x() < 0 || m_cursorPos.y() < 0) {
        if (m_tnfHoverPopup) {
            m_tnfHoverPopup->hide();
        }
        return;
    }

    const QString title = QStringLiteral("RF Tracking Notch");
    const QString freq = QString("%1 MHz").arg(formatFlagFrequency(tnf->freqMhz));
    const QString widthText = QString("%1 Hz Wide").arg(tnf->widthHz);
    const QColor baseColor = tnfColor(*tnf);
    QColor fillBase(0x0f, 0x0f, 0x1a, 220);
    fillBase.setRed((fillBase.red() * 3 + baseColor.red()) / 4);
    fillBase.setGreen((fillBase.green() * 3 + baseColor.green()) / 4);
    fillBase.setBlue((fillBase.blue() * 3 + baseColor.blue()) / 4);
    const QColor lineColor = baseColor.lighter(115);

    const QString html = QString(
        "<div style='font-size:9pt; font-weight:600;'>%1</div>"
        "<div style='font-size:11pt;'>%2</div>"
        "<div style='font-size:11pt;'>%3</div>")
            .arg(title, freq, widthText);

    m_tnfHoverPopup->setStyleSheet(QString(
        "QLabel {"
        " background-color: rgba(%1,%2,%3,%4);"
        " color: rgb(%5,%6,%7);"
        " border: 1px solid rgb(%5,%6,%7);"
        " border-radius: 2px;"
        " }")
            .arg(fillBase.red())
            .arg(fillBase.green())
            .arg(fillBase.blue())
            .arg(fillBase.alpha())
            .arg(lineColor.red())
            .arg(lineColor.green())
            .arg(lineColor.blue()));
    m_tnfHoverPopup->setText(html);
    m_tnfHoverPopup->adjustSize();

    const QSize boxSize = m_tnfHoverPopup->sizeHint();
    int left = m_cursorPos.x() + 12;
    if (left + boxSize.width() > width()) {
        left = m_cursorPos.x() - boxSize.width() - 4;
    }
    int top = m_cursorPos.y() - boxSize.height() - 4;
    if (top < 0) {
        top = std::min(height() - boxSize.height(), m_cursorPos.y() + 16);
    }
    m_tnfHoverPopup->move(left, top);
    m_tnfHoverPopup->show();
    m_tnfHoverPopup->raise();
}

int SpectrumWidget::tnfAtPixel(int x, int preferredId) const
{
    const auto containsPixel = [this, x](const TnfMarker& tnf) {
        const int cx = mhzToX(tnf.freqMhz);
        const int halfW = std::max(2, mhzToX(tnf.freqMhz + tnf.widthHz / 2.0e6) - cx);
        const int left = cx - halfW - 3;
        const int right = cx + halfW + 3;
        return x >= left && x <= right;
    };

    if (preferredId >= 0) {
        if (const TnfMarker* preferredTnf = tnfMarkerById(preferredId)) {
            if (containsPixel(*preferredTnf)) {
                return preferredId;
            }
        }
    }

    int bestId = -1;
    int bestDistance = INT_MAX;
    for (int i = m_tnfMarkers.size() - 1; i >= 0; --i) {
        const TnfMarker& tnf = m_tnfMarkers[i];
        if (!containsPixel(tnf)) {
            continue;
        }

        const int distance = std::abs(x - mhzToX(tnf.freqMhz));
        if (distance < bestDistance) {
            bestDistance = distance;
            bestId = tnf.id;
        }
    }
    return bestId;
}

// ─── VFO marker (filter passband + tuned frequency line) ──────────────────────

void SpectrumWidget::drawSpotMarkers(QPainter& p, const QRect& specRect)
{
    // Merge DX spots, Signal History markers, and QRM History markers.
    // Each category is gated by its own flag.  S-History markers within 3 kHz of
    // an active DX spot are suppressed — the spot carries richer callsign/DXCC info.
    QVector<SpotMarker> allMarkers;
    if (m_showSpots) { allMarkers = m_spotMarkers; }
    if (m_showSHistory || m_showSHistoryQrm) {
        constexpr double kSpotOverrideMhz = 0.003;
        for (const auto& sh : m_sHistoryMarkers) {
            const bool isQrm = (sh.source == QStringLiteral("QRM"));
            if (isQrm  && !m_showSHistoryQrm) { continue; }
            if (!isQrm && !m_showSHistory)    { continue; }
            bool masked = false;
            for (const auto& sp : m_spotMarkers) {
                if (std::abs(sh.freqMhz - sp.freqMhz) < kSpotOverrideMhz) {
                    masked = true;
                    break;
                }
            }
            if (!masked) { allMarkers.append(sh); }
        }
    }
    if (allMarkers.isEmpty()) return;

    QFont spotFont = p.font();
    spotFont.setPixelSize(m_spotFontSize);
    spotFont.setBold(true);
    p.setFont(spotFont);
    const QFontMetrics fm(spotFont);

    // Starting Y position based on percentage setting
    const int startY = specRect.top() + specRect.height() * m_spotStartPct / 100;
    const int th = fm.height() + 2;
    const int maxBottom = startY + th * m_spotMaxLevels;

    // Track label positions to avoid overlap and for click detection
    QVector<QRect> placed;
    m_spotClickRects.clear();
    m_spotClusters.clear();

    // Track which spots overflow (can't be placed within max levels)
    // Key: x pixel position (quantized to label width), Value: list of overflowed spots
    QMap<int, QVector<SpotMarker>> overflowGroups;
    constexpr int ClusterBinWidth = 40;  // pixels — spots within this range cluster together

    int mIdx = 0;
    for (const auto& spot : allMarkers) {
        const int x = mhzToX(spot.freqMhz);
        if (x < 0 || x > width()) { ++mIdx; continue; }

        // Color priority: override → DXCC → spot-provided → default cyan
        QColor col(0x00, 0xb4, 0xd8);  // default cyan
        if (m_spotOverrideColors) {
            col = m_spotColor;
        } else if (spot.dxccColor.isValid()) {
            col = spot.dxccColor;
        } else if (!spot.color.isEmpty() && spot.color.startsWith('#')) {
            QColor parsed(spot.color);
            if (parsed.isValid()) col = parsed;
        }

        // Draw callsign label
        const QString label = spot.callsign;
        const int tw = fm.horizontalAdvance(label) + 6;

        // Start at configured position, nudge down to avoid overlap.
        // Re-scan from the start after each nudge to handle cases where
        // nudging past label A lands on top of label B.
        QRect labelRect(x - tw / 2, startY, tw, th);
        bool collision = true;
        while (collision) {
            collision = false;
            for (const auto& r : placed) {
                if (labelRect.intersects(r)) {
                    labelRect.moveTop(r.bottom() + 1);
                    collision = true;
                    break;
                }
            }
        }
        // Overflow — collect for cluster badge
        if (labelRect.bottom() > maxBottom) {
            int bin = x / ClusterBinWidth;
            overflowGroups[bin].append(spot);
            ++mIdx;
            continue;
        }

        // Draw vertical tick line from bottom of spectrum up to the label
        if (m_spotShowLines) {
            p.setPen(QPen(QColor(col.red(), col.green(), col.blue(), 120), 1, Qt::DotLine));
            p.drawLine(x, specRect.bottom(), x, labelRect.bottom());
        }

        placed.append(labelRect);
        m_spotClickRects.append({labelRect, spot.freqMhz, mIdx});

        // QRM markers draw at 30% opacity (70% transparent)
        const bool isQrm = (spot.source == QStringLiteral("QRM"));
        if (isQrm) { p.setOpacity(0.3); }

        // Background pill — only draw when override background is enabled (#768)
        if (m_spotOverrideBg) {
            int bgAlpha = m_spotBgOpacity * 255 / 100;
            QColor bgCol = m_spotBgColor;
            bgCol.setAlpha(bgAlpha);
            p.setPen(Qt::NoPen);
            p.setBrush(bgCol);
            p.drawRoundedRect(labelRect, 3, 3);
        }

        // Text
        p.setPen(col);
        p.drawText(labelRect, Qt::AlignCenter, label);

        if (isQrm) { p.setOpacity(1.0); }
        ++mIdx;
    }

    // Draw cluster badges for overflow groups
    if (!overflowGroups.isEmpty()) {
        QFont badgeFont = spotFont;
        badgeFont.setPixelSize(m_spotFontSize - 2);
        p.setFont(badgeFont);
        const QFontMetrics bfm(badgeFont);

        for (auto it = overflowGroups.constBegin(); it != overflowGroups.constEnd(); ++it) {
            const auto& spots = it.value();
            if (spots.isEmpty()) continue;

            // Position badge at average x of the group, at maxBottom
            int avgX = 0;
            for (const auto& s : spots)
                avgX += mhzToX(s.freqMhz);
            avgX /= spots.size();

            const QString badgeText = QString("+%1").arg(spots.size());
            const int bw = bfm.horizontalAdvance(badgeText) + 10;
            QRect badgeRect(avgX - bw / 2, maxBottom + 2, bw, th);

            // Nudge horizontally to avoid overlapping other badges/labels
            for (const auto& r : placed) {
                if (badgeRect.intersects(r))
                    badgeRect.moveLeft(r.right() + 3);
            }
            placed.append(badgeRect);

            // Draw badge with distinct style
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x30, 0x50, 0x70, 200));
            p.drawRoundedRect(badgeRect, 3, 3);

            p.setPen(QColor(0xff, 0xc0, 0x40));  // amber text
            p.drawText(badgeRect, Qt::AlignCenter, badgeText);

            // Store for click detection
            SpotCluster cluster;
            cluster.rect = badgeRect;
            cluster.spots = spots;
            m_spotClusters.append(cluster);
        }

        p.setFont(spotFont);  // restore spot font
    }

    p.setFont(QFont());  // restore default
}

void SpectrumWidget::drawSwrSweep(QPainter& p, const QRect& specRect)
{
    if (m_swrSweepPoints.isEmpty())
        return;

    const int rightEdge = specRect.right() - DBM_STRIP_W - 8;
    const int plotLeft = specRect.left() + 8;
    const int plotHeight = qBound(42, specRect.height() / 5, 64);

    int wantedTop = specRect.top() + 6;
    bool sawVfo = false;
    for (auto it = m_vfoWidgets.cbegin(); it != m_vfoWidgets.cend(); ++it) {
        auto* vfo = it.value();
        if (!vfo || !vfo->isVisible())
            continue;
        sawVfo = true;
        wantedTop = qMax(wantedTop, vfo->geometry().bottom() + 8);
    }
    if (!sawVfo && !m_sliceOverlays.isEmpty())
        wantedTop = specRect.top() + 96;

    const int minTop = specRect.top() + 6;
    const int maxTop = qMax(minTop, specRect.bottom() - plotHeight - 8);
    const int plotTop = qBound(minTop, wantedTop, maxTop);
    const QRect plotRect(plotLeft, plotTop,
                         qMax(0, rightEdge - plotLeft), plotHeight);
    if (plotRect.width() < 80 || plotRect.height() < 28)
        return;

    struct ValidSweepPoint {
        double freqMhz{0.0};
        float swr{1.0f};
    };
    QVector<ValidSweepPoint> validPoints;
    validPoints.reserve(m_swrSweepPoints.size());

    int bestPointIndex = -1;
    double bestFreqMhz = 0.0;
    float bestSwr = 0.0f;
    float maxSwr = 3.0f;
    for (const auto& point : m_swrSweepPoints) {
        if (!std::isfinite(static_cast<double>(point.swr))
            || !std::isfinite(static_cast<double>(point.freqMhz))) {
            continue;
        }

        const float swr = qMax(1.0f, point.swr);
        if (bestPointIndex < 0 || swr < bestSwr) {
            bestPointIndex = validPoints.size();
            bestSwr = swr;
            bestFreqMhz = point.freqMhz;
        }

        maxSwr = qMax(maxSwr, swr);
        validPoints.append({point.freqMhz, swr});
    }
    const bool hasBest = bestPointIndex >= 0;
    maxSwr = qBound(2.0f, std::ceil(maxSwr * 2.0f) / 2.0f, 10.0f);
    constexpr float minSwr = 1.0f;
    const float logRange = qMax(0.1f, static_cast<float>(std::log(maxSwr / minSwr)));

    auto yForSwr = [&](float swr) {
        const float clipped = qBound(minSwr, swr, maxSwr);
        const float frac = static_cast<float>(std::log(clipped / minSwr)) / logRange;
        return plotRect.bottom() - frac * plotRect.height();
    };

    struct BandwidthSpan {
        bool valid{false};
        float threshold{0.0f};
        double lowMhz{0.0};
        double highMhz{0.0};
    };

    auto interpolateThreshold = [](const ValidSweepPoint& a,
                                   const ValidSweepPoint& b,
                                   float threshold) {
        const double denom = static_cast<double>(b.swr) - static_cast<double>(a.swr);
        if (std::abs(denom) < 1.0e-9)
            return (a.freqMhz + b.freqMhz) * 0.5;

        const double frac = qBound(0.0,
                                   (static_cast<double>(threshold) - a.swr) / denom,
                                   1.0);
        return a.freqMhz + (b.freqMhz - a.freqMhz) * frac;
    };

    auto spanForThreshold = [&](float threshold) {
        BandwidthSpan span;
        span.threshold = threshold;
        if (bestPointIndex < 0 || validPoints.size() < 2
            || validPoints[bestPointIndex].swr > threshold) {
            return span;
        }

        int left = bestPointIndex;
        while (left > 0 && validPoints[left - 1].swr <= threshold)
            --left;

        int right = bestPointIndex;
        while (right + 1 < validPoints.size() && validPoints[right + 1].swr <= threshold)
            ++right;

        span.lowMhz = (left == 0)
            ? validPoints.first().freqMhz
            : interpolateThreshold(validPoints[left - 1], validPoints[left], threshold);
        span.highMhz = (right == validPoints.size() - 1)
            ? validPoints.last().freqMhz
            : interpolateThreshold(validPoints[right], validPoints[right + 1], threshold);
        span.valid = std::isfinite(span.lowMhz)
            && std::isfinite(span.highMhz)
            && span.highMhz > span.lowMhz;
        return span;
    };

    const BandwidthSpan bw15 = spanForThreshold(1.5f);
    const BandwidthSpan bw20 = spanForThreshold(2.0f);

    auto formatBandwidth = [](double widthMhz) {
        const double khz = widthMhz * 1000.0;
        if (khz >= 1000.0)
            return QStringLiteral("%1 MHz").arg(widthMhz, 0, 'f', 2);
        if (khz >= 100.0)
            return QStringLiteral("%1 kHz").arg(khz, 0, 'f', 0);
        return QStringLiteral("%1 kHz").arg(khz, 0, 'f', 1);
    };

    p.save();
    p.setClipRect(specRect);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x06, 0x0d, 0x12, 190));
    p.drawRoundedRect(plotRect, 3, 3);

    auto fillThresholdBand = [&](float lowSwr, float highSwr, const QColor& color) {
        const float high = qMin(highSwr, maxSwr);
        if (high <= lowSwr)
            return;
        const qreal top = yForSwr(high);
        const qreal bottom = yForSwr(lowSwr);
        p.fillRect(QRectF(plotRect.left(), top, plotRect.width(), bottom - top), color);
    };
    fillThresholdBand(1.0f, 1.5f, QColor(0x20, 0xc0, 0x60, 28));
    fillThresholdBand(1.5f, 2.0f, QColor(0xff, 0xc0, 0x40, 22));
    fillThresholdBand(2.0f, maxSwr, QColor(0xff, 0x58, 0x58, 16));

    p.setPen(QPen(QColor(0x80, 0x90, 0xa0, 120), 1, Qt::DotLine));
    const float gridSwrs[] = {1.5f, 2.0f, 3.0f, 5.0f, 10.0f};
    for (float gridSwr : gridSwrs) {
        if (gridSwr < maxSwr)
            p.drawLine(plotRect.left(), yForSwr(gridSwr), plotRect.right(), yForSwr(gridSwr));
    }

    struct PlotPoint {
        QPointF pos;
        double freqMhz{0.0};
        float swr{1.0f};
    };
    QVector<PlotPoint> visiblePoints;
    int bestVisibleIndex = -1;
    QPolygonF poly;
    for (int i = 0; i < validPoints.size(); ++i) {
        const auto& point = validPoints[i];
        const int x = mhzToX(point.freqMhz);
        if (x < plotRect.left() - 4 || x > plotRect.right() + 4)
            continue;
        const QPointF pos(x, yForSwr(point.swr));
        if (i == bestPointIndex)
            bestVisibleIndex = visiblePoints.size();
        visiblePoints.append({pos, point.freqMhz, point.swr});
        poly << pos;
    }

    if (poly.size() >= 2) {
        QPen sweepPen(QColor(0xff, 0xc0, 0x40, 235), 2);
        sweepPen.setCapStyle(Qt::RoundCap);
        sweepPen.setJoinStyle(Qt::RoundJoin);
        p.setPen(sweepPen);
        p.drawPolyline(poly);
    }
    p.setPen(QPen(QColor(0x0a, 0x0a, 0x14, 220), 1));
    p.setBrush(QColor(0xff, 0xc0, 0x40, 240));
    for (const QPointF& pt : poly)
        p.drawEllipse(pt, 3.2, 3.2);

    if (!visiblePoints.isEmpty()) {
        auto drawEndpointNotch = [&](const QPointF& pt) {
            const qreal top = std::clamp(pt.y() - 6.0, qreal(plotRect.top() + 2),
                                         qreal(plotRect.bottom() - 2));
            const qreal bottom = std::clamp(pt.y() + 6.0, qreal(plotRect.top() + 2),
                                            qreal(plotRect.bottom() - 2));
            p.drawLine(QPointF(pt.x(), top), QPointF(pt.x(), bottom));
        };

        QPen endpointPen(QColor(0xff, 0xe0, 0x90, 245), 2);
        endpointPen.setCapStyle(Qt::RoundCap);
        p.setPen(endpointPen);
        drawEndpointNotch(visiblePoints.first().pos);
        if (visiblePoints.size() > 1)
            drawEndpointNotch(visiblePoints.last().pos);
    }

    if (bestVisibleIndex >= 0 && bestVisibleIndex < visiblePoints.size()) {
        const QPointF bestPos = visiblePoints[bestVisibleIndex].pos;
        QPen resonancePen(QColor(0x7c, 0xf4, 0xa4, 245), 2);
        resonancePen.setCapStyle(Qt::RoundCap);
        resonancePen.setJoinStyle(Qt::RoundJoin);
        p.setPen(resonancePen);
        p.setBrush(Qt::NoBrush);

        const bool placeBelow = bestPos.y() - 11.0 < plotRect.top() + 2;
        const qreal tipY = std::clamp(bestPos.y() + (placeBelow ? 2.0 : -2.0),
                                      qreal(plotRect.top() + 2),
                                      qreal(plotRect.bottom() - 2));
        const qreal wingY = std::clamp(bestPos.y() + (placeBelow ? 11.0 : -11.0),
                                       qreal(plotRect.top() + 2),
                                       qreal(plotRect.bottom() - 2));
        QPolygonF caret;
        caret << QPointF(bestPos.x() - 6.0, wingY)
              << QPointF(bestPos.x(), tipY)
              << QPointF(bestPos.x() + 6.0, wingY);
        p.drawPolyline(caret);
        p.drawEllipse(bestPos, 4.4, 4.4);
    }

    auto drawBandwidthSpan = [&](const BandwidthSpan& span, int row, const QColor& color) {
        if (m_swrSweepRunning || !span.valid)
            return;
        int x1 = mhzToX(span.lowMhz);
        int x2 = mhzToX(span.highMhz);
        if (x2 < x1)
            std::swap(x1, x2);
        x1 = qBound(plotRect.left() + 2, x1, plotRect.right() - 2);
        x2 = qBound(plotRect.left() + 2, x2, plotRect.right() - 2);
        if (x2 - x1 < 6)
            return;

        const qreal y = plotRect.bottom() - 5.0 - row * 5.0;
        QPen spanPen(color, 2);
        spanPen.setCapStyle(Qt::RoundCap);
        p.setPen(spanPen);
        p.drawLine(QPointF(x1, y), QPointF(x2, y));
        p.drawLine(QPointF(x1, y - 3.0), QPointF(x1, y + 3.0));
        p.drawLine(QPointF(x2, y - 3.0), QPointF(x2, y + 3.0));
    };
    drawBandwidthSpan(bw20, 0, QColor(0xff, 0xc0, 0x40, 205));
    drawBandwidthSpan(bw15, 1, QColor(0x7c, 0xf4, 0xa4, 225));

    if (m_swrSweepCurrentFreqMhz > 0.0) {
        const int cx = mhzToX(m_swrSweepCurrentFreqMhz);
        if (cx >= plotRect.left() && cx <= plotRect.right()) {
            p.setPen(QPen(QColor(0x00, 0xb4, 0xd8, 210), 1, Qt::DashLine));
            p.drawLine(cx, plotRect.top(), cx, plotRect.bottom());
        }
    }

    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);
    const QFontMetrics fm(f);
    QStringList labelLines;
    if (hasBest) {
        QString bestLine = QStringLiteral("SWR %1:1").arg(bestSwr, 0, 'f', 2);
        if (m_swrSweepRunning)
            bestLine += QStringLiteral("  RUN");
        labelLines << bestLine;

        QString freqLine = QStringLiteral("Res %1 MHz").arg(bestFreqMhz, 0, 'f', 3);
        if (!m_swrSweepSourceLabel.isEmpty())
            freqLine += QStringLiteral("  ") + m_swrSweepSourceLabel;
        labelLines << freqLine;

        QStringList bandwidthParts;
        if (!m_swrSweepRunning) {
            if (bw15.valid)
                bandwidthParts << QStringLiteral("1.5 %1").arg(formatBandwidth(bw15.highMhz - bw15.lowMhz));
            if (bw20.valid)
                bandwidthParts << QStringLiteral("2.0 %1").arg(formatBandwidth(bw20.highMhz - bw20.lowMhz));
        }
        if (!bandwidthParts.isEmpty())
            labelLines << QStringLiteral("BW ") + bandwidthParts.join(QStringLiteral("  "));
    } else {
        labelLines << (m_swrSweepRunning
            ? QStringLiteral("SWR Sweep  RUN")
            : QStringLiteral("SWR Sweep"));
        if (!m_swrSweepSourceLabel.isEmpty())
            labelLines << m_swrSweepSourceLabel;
    }

    int labelW = 0;
    for (const QString& line : labelLines)
        labelW = qMax(labelW, fm.horizontalAdvance(line));
    labelW += 8;
    const int labelH = fm.lineSpacing() * labelLines.size() + 4;
    int labelX = plotRect.left() + 4;
    int labelY = plotRect.top() - labelH - 3;
    if (labelY < specRect.top() + 3)
        labelY = plotRect.top() + 3;

    QRect labelRect(labelX, labelY, labelW, labelH);
    if (m_overlayMenu && m_overlayMenu->isVisible()
        && labelRect.intersects(m_overlayMenu->geometry().adjusted(-4, -4, 8, 4))) {
        labelX = m_overlayMenu->geometry().right() + 8;
        const int maxLabelX = qMax(plotRect.left() + 4, plotRect.right() - labelW - 4);
        labelX = qBound(plotRect.left() + 4, labelX, maxLabelX);
        labelRect.moveLeft(labelX);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x0a, 0x0a, 0x14, 220));
    p.drawRoundedRect(labelRect, 2, 2);
    p.setPen(QColor(0xff, 0xd0, 0x70));
    for (int i = 0; i < labelLines.size(); ++i) {
        const QRect lineRect(labelRect.left() + 4,
                             labelRect.top() + 2 + i * fm.lineSpacing(),
                             labelRect.width() - 8,
                             fm.lineSpacing());
        p.drawText(lineRect, Qt::AlignVCenter | Qt::AlignLeft, labelLines[i]);
    }

    if (!visiblePoints.isEmpty()) {
        const int valueGap = 4;
        const int valueH = fm.height() + 4;
        const bool placeBelow = plotRect.bottom() + valueGap + valueH
            <= specRect.bottom() - 4;
        const int valueY = placeBelow
            ? plotRect.bottom() + valueGap
            : plotRect.bottom() - valueH - 4;
        constexpr int kMinValueSpacingPx = 86;
        int lastLabelRight = -1000000;
        int lastLabeled = -1;

        auto drawValueLabel = [&](int i, bool force) {
            const PlotPoint& point = visiblePoints[i];
            const QString text = QString::number(point.swr, 'f', 2);
            const int valueW = fm.horizontalAdvance(text) + 8;
            int x = qRound(point.pos.x()) - valueW / 2;
            x = qBound(plotRect.left() + 2, x, plotRect.right() - valueW - 2);
            const QRect valueRect(x, valueY, valueW, valueH);
            if (!force && valueRect.left() <= lastLabelRight + 6)
                return;

            p.setPen(QPen(QColor(0xff, 0xd0, 0x70, 130), 1));
            const qreal notchY = std::clamp(point.pos.y(),
                                            qreal(plotRect.top() + 2),
                                            qreal(plotRect.bottom()));
            const QPointF notchTop(point.pos.x(), notchY);
            const QPointF notchBottom(point.pos.x(),
                                      placeBelow ? valueRect.top() : valueRect.bottom());
            p.drawLine(notchTop, notchBottom);

            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x0a, 0x0a, 0x14, 230));
            p.drawRoundedRect(valueRect, 2, 2);
            p.setPen(QColor(0xff, 0xe0, 0x90));
            p.drawText(valueRect, Qt::AlignCenter, text);

            lastLabelRight = valueRect.right();
            lastLabeled = i;
        };

        for (int i = 0; i < visiblePoints.size(); ++i) {
            const bool first = (i == 0);
            const bool spaced = qRound(visiblePoints[i].pos.x()) > lastLabelRight + kMinValueSpacingPx;
            if (first || spaced)
                drawValueLabel(i, first);
        }

        const int lastIndex = visiblePoints.size() - 1;
        if (lastLabeled != lastIndex)
            drawValueLabel(lastIndex, false);
    }

    const QString scaleLabel = QStringLiteral("%1").arg(maxSwr, 0, 'f', 1);
    p.setPen(QColor(0x80, 0x90, 0xa0, 180));
    p.drawText(plotRect.right() - fm.horizontalAdvance(scaleLabel) - 4,
               plotRect.top() + fm.ascent() + 3,
               scaleLabel);
    p.drawText(plotRect.right() - fm.horizontalAdvance("1.0") - 4,
               plotRect.bottom() - 3,
               "1.0");

    p.restore();
}

void SpectrumWidget::showSpotClusterPopup(const SpotCluster& cluster, const QPoint& globalPos)
{
    auto* menu = new QMenu(this);
    menu->setStyleSheet(
        "QMenu {"
        "  background: #0f0f1a;"
        "  border: 1px solid #305070;"
        "  padding: 4px;"
        "}"
        "QMenu::item {"
        "  color: #c8d8e8;"
        "  padding: 4px 12px;"
        "  font-size: 12px;"
        "}"
        "QMenu::item:selected {"
        "  background: #1a3a5a;"
        "  color: #00b4d8;"
        "}");

    for (const auto& spot : cluster.spots) {
        QString text = QString("%1  %2 kHz")
            .arg(spot.callsign, -10)
            .arg(spot.freqMhz * 1000.0, 0, 'f', 1);
        if (!spot.mode.isEmpty())
            text += "  " + spot.mode;
        auto* action = menu->addAction(text);
        connect(action, &QAction::triggered, this, [this, spot] {
            if (spot.source == "Memory") {
                emit spotTriggered(spot.index);
            } else {
                emit frequencyClicked(spot.freqMhz);
            }
        });
    }

    menu->popup(globalPos);
    // QMenu self-deletes on close with WA_DeleteOnClose
    menu->setAttribute(Qt::WA_DeleteOnClose);
}

void SpectrumWidget::drawSliceMarkers(QPainter& p, const QRect& specRect, const QRect& wfRect)
{
    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double endMhz   = m_centerMhz + m_bandwidthMhz / 2.0;

    // Draw inactive slices first, then active slice on top
    auto drawOne = [&](const SliceOverlay& so) {
        if (so.freqMhz < startMhz || so.freqMhz > endMhz) return;

        const QColor col = sliceColor(so.sliceId, so.isActive);
        const int freqLineBottom = m_extendedFrequencyLine ? wfRect.bottom() : specRect.bottom();
        const double fLoMhz = so.freqMhz + so.filterLowHz / 1.0e6;
        const double fHiMhz = so.freqMhz + so.filterHighHz / 1.0e6;

        const int vfoX = mhzToX(so.freqMhz);
        const int fX1  = mhzToX(fLoMhz);
        const int fX2  = mhzToX(fHiMhz);
        const int fW   = fX2 - fX1;

        // ── Filter passband shading ──────────────────────────────────────
        // Drawn only in the spectrum area. The waterfall is a historical
        // record of received signals; painting a UI affordance over it
        // makes the passband look like a signal in the history (#1270).
        p.fillRect(QRect(fX1, specRect.top(), fW, specRect.height()),
                   QColor(col.red(), col.green(), col.blue(), 35));

        // Filter edge lines — user-hidden via per-slice VFO flag toggle (#1526)
        if (!so.filterEdgesHidden) {
            p.setPen(QPen(QColor(col.red(), col.green(), col.blue(), 130), 1));
            p.drawLine(fX1, specRect.top(), fX1, specRect.bottom());
            p.drawLine(fX2, specRect.top(), fX2, specRect.bottom());
        }

        // ── RTTY/DIGL: mark/space lines replace the VFO center line ────
        const bool isRttyMode = (so.mode == "RTTY" || so.mode == "DIGL");

        if (isRttyMode) {
            double markMhz, spaceMhz;
            if (so.mode == "RTTY") {
                // In RTTY mode, RF_frequency IS the mark (radio applies IF shift).
                markMhz  = so.freqMhz;
                spaceMhz = so.freqMhz - so.rttyShift / 1.0e6;
            } else {
                // In DIGL mode, RF_frequency is the carrier (no IF shift).
                markMhz  = so.freqMhz - so.rttyMark / 1.0e6;
                spaceMhz = markMhz - so.rttyShift / 1.0e6;
            }
            const int markX  = mhzToX(markMhz);
            const int spaceX = mhzToX(spaceMhz);

            // Mark line — green, dashed
            p.setPen(QPen(QColor(0, 200, 80, 200), 1, Qt::DashLine));
            p.drawLine(markX, specRect.top(), markX, freqLineBottom);

            // Space line — red, dashed
            p.setPen(QPen(QColor(220, 60, 60, 200), 1, Qt::DashLine));
            p.drawLine(spaceX, specRect.top(), spaceX, freqLineBottom);

            // Labels at top
            QFont f = p.font();
            f.setPixelSize(10);
            f.setBold(true);
            p.setFont(f);
            p.setPen(QColor(0, 200, 80, 240));
            p.drawText(markX + 2, specRect.top() + 12, "M");
            p.setPen(QColor(220, 60, 60, 240));
            p.drawText(spaceX + 2, specRect.top() + 12, "S");
        } else if (so.markerWidth > 0) {
            // ── Standard VFO center line ─────────────────────────────────
            // Skipped entirely when markerWidth == 0 (user chose
            // "Marker: Off") — passband bracket only.
            int markerX = vfoX;

            // Per-slice VFO marker thickness — user-toggled via VFO flag (#1526)
            const qreal vfoLineW = static_cast<qreal>(so.markerWidth);
            p.setPen(QPen(QColor(col.red(), col.green(), col.blue(), 220), vfoLineW));
            p.drawLine(markerX, specRect.top(), markerX, freqLineBottom);

            // ── Triangle marker at top ───────────────────────────────────
            const int triHalf = 6;
            const int triH = 10;
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            QPolygon tri;
            tri << QPoint(markerX - triHalf, specRect.top())
                << QPoint(markerX + triHalf, specRect.top())
                << QPoint(markerX, specRect.top() + triH);
            p.drawPolygon(tri);
        }

        // ── RIT/XIT offset lines ──────────────────────────────────────
        if (so.ritOn && so.ritFreq != 0) {
            const int ritX = mhzToX(so.freqMhz + so.ritFreq / 1.0e6);
            QPen ritPen(QColor(col.red(), col.green(), col.blue(), 160), 1, Qt::DashLine);
            p.setPen(ritPen);
            p.drawLine(ritX, specRect.top(), ritX, wfRect.bottom());
            QFont f = p.font();
            f.setPixelSize(10);
            f.setBold(true);
            p.setFont(f);
            p.setPen(QColor(col.red(), col.green(), col.blue(), 200));
            p.drawText(ritX + 2, specRect.top() + 12, "R");
        }
        if (so.xitOn && so.xitFreq != 0) {
            const int xitX = mhzToX(so.freqMhz + so.xitFreq / 1.0e6);
            QPen xitPen(QColor(220, 80, 80, 180), 1, Qt::DashLine);
            p.setPen(xitPen);
            p.drawLine(xitX, specRect.top(), xitX, wfRect.bottom());
            QFont f = p.font();
            f.setPixelSize(10);
            f.setBold(true);
            p.setFont(f);
            p.setPen(QColor(220, 80, 80, 220));
            p.drawText(xitX + 2, specRect.top() + 12, "X");
        }

        // Slice letter badge and TX badge are now rendered by each
        // slice's VfoWidget — no need to draw them on the spectrum.
    };

    // Draw all slices (active last so its marker is on top)
    for (const auto& so : m_sliceOverlays)
        if (!so.isActive) drawOne(so);
    for (const auto& so : m_sliceOverlays)
        if (so.isActive) drawOne(so);
}

// ─── Frequency scale bar ──────────────────────────────────────────────────────

void SpectrumWidget::drawFreqScale(QPainter& p, const QRect& r)
{
    p.fillRect(r, QColor(0x06, 0x06, 0x10));

    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double endMhz   = m_centerMhz + m_bandwidthMhz / 2.0;

    // Grid step — honours user spacing override (#1390)
    const double stepMhz = effectiveGridStepMhz(width());
    const double firstLine = std::ceil(startMhz / stepMhz) * stepMhz;

    QFont f = p.font();
    f.setPointSize(8);
    p.setFont(f);
    const QFontMetrics fm(f);

    // Decimal places: enough to distinguish labels at this step size
    int decimals;
    if      (stepMhz < 0.0001) decimals = 6;
    else if (stepMhz < 0.001)  decimals = 5;
    else if (stepMhz < 0.01)   decimals = 4;
    else if (stepMhz < 1.0)    decimals = 3;
    else                        decimals = 2;

    // Compute label thinning: draw a tick on every grid line but only label
    // every Nth line so labels don't overlap (~60px minimum between labels).
    int labelEvery = 1;
    if (m_freqGridSpacingKhz > 0 && width() > 0) {
        double pxPerStep = (stepMhz / m_bandwidthMhz) * width();
        if (pxPerStep < 60.0)
            labelEvery = static_cast<int>(std::ceil(60.0 / pxPerStep));
    }

    int stepIdx = 0;
    for (double freq = firstLine; freq <= endMhz; freq += stepMhz, ++stepIdx) {
        const int x = mhzToX(freq);

        // Tick mark on every grid line
        p.setPen(QColor(0x40, 0x60, 0x80));
        p.drawLine(x, r.top(), x, r.top() + 4);

        // Label only every Nth line to prevent overlap
        if (stepIdx % labelEvery != 0) continue;

        const QString label = QString::number(freq, 'f', decimals);
        const int tw = fm.horizontalAdvance(label);
        const int lx = qBound(0, x - tw / 2, width() - tw);

        p.setPen(QColor(0x70, 0x90, 0xb0));
        p.drawText(lx, r.bottom() - 2, label);
    }
}

// ─── dBm scale strip (right edge of FFT area) ────────────────────────────────

void SpectrumWidget::drawDbmScale(QPainter& p, const QRect& specRect)
{
    const int stripX = specRect.right() - DBM_STRIP_W + 1;
    const QRect strip(stripX, specRect.top(), DBM_STRIP_W, specRect.height());

    // Semi-opaque background
    p.fillRect(strip, QColor(0x0a, 0x0a, 0x18, 220));

    // Left border line
    p.setPen(QColor(0x30, 0x40, 0x50));
    p.drawLine(stripX, specRect.top(), stripX, specRect.bottom());

    // ── Up/Down arrows side by side at top ─────────────────────────────
    const int halfW = DBM_STRIP_W / 2;
    const int upCx  = stripX + halfW / 2;       // left half center
    const int dnCx  = stripX + halfW + halfW / 2; // right half center
    const int arrowTop = specRect.top() + 2;
    const int arrowBot = specRect.top() + DBM_ARROW_H - 2;

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x60, 0x80, 0xa0));

    // Up arrow (▲) — left side
    QPolygon upTri;
    upTri << QPoint(upCx - 5, arrowBot)
          << QPoint(upCx + 5, arrowBot)
          << QPoint(upCx,     arrowTop);
    p.drawPolygon(upTri);

    // Down arrow (▼) — right side
    QPolygon dnTri;
    dnTri << QPoint(dnCx - 5, arrowTop)
          << QPoint(dnCx + 5, arrowTop)
          << QPoint(dnCx,     arrowBot);
    p.drawPolygon(dnTri);

    // ── dBm labels ───────────────────────────────────────────────────────
    QFont f = p.font();
    f.setPointSize(7);
    p.setFont(f);
    const QFontMetrics fm(f);

    const int labelTop = specRect.top() + DBM_ARROW_H + 4;

    // Use adaptive step: aim for ~4-6 labels
    float rawStep = m_dynamicRange / 5.0f;
    float stepDb;
    if      (rawStep >= 20.0f) stepDb = 20.0f;
    else if (rawStep >= 10.0f) stepDb = 10.0f;
    else if (rawStep >= 5.0f)  stepDb = 5.0f;
    else                        stepDb = 2.0f;

    const float bottomDbm = m_refLevel - m_dynamicRange;
    const float firstLabel = std::ceil(bottomDbm / stepDb) * stepDb;

    for (float dbm = firstLabel; dbm <= m_refLevel; dbm += stepDb) {
        const float frac = (m_refLevel - dbm) / m_dynamicRange;
        const int y = specRect.top() + static_cast<int>(frac * specRect.height());
        if (y < labelTop || y > specRect.bottom() - 5) continue;

        // Tick mark
        p.setPen(QColor(0x50, 0x70, 0x80));
        p.drawLine(stripX, y, stripX + 4, y);

        // Label
        const QString label = QString::number(static_cast<int>(dbm));
        p.setPen(QColor(0x80, 0xa0, 0xb0));
        p.drawText(stripX + 6, y + fm.ascent() / 2, label);
    }
}

// ─── Time scale (right edge of waterfall) ─────────────────────────────────────

void SpectrumWidget::drawTimeScale(QPainter& p, const QRect& wfRect)
{
    const QRect strip = waterfallTimeScaleRect(wfRect);
    const int stripX = strip.x();

    // Semi-opaque background
    p.fillRect(strip, QColor(0x0a, 0x0a, 0x18, 220));

    // Left border line
    p.setPen(QColor(0x30, 0x40, 0x50));
    p.drawLine(stripX, wfRect.top(), stripX, wfRect.bottom());

    const QRect liveRect = waterfallLiveButtonRect(wfRect);
    p.setPen(QColor(0x40, 0x50, 0x60));
    p.setBrush(m_wfLive ? QColor(0x45, 0x45, 0x45) : QColor(0xc0, 0x20, 0x20));
    p.drawRoundedRect(liveRect, 3, 3);

    QFont liveFont = p.font();
    liveFont.setPointSize(7);
    liveFont.setBold(true);
    p.setFont(liveFont);
    p.setPen(m_wfLive ? QColor(0xb0, 0xb0, 0xb0) : Qt::white);
    p.drawText(liveRect, Qt::AlignCenter, "LIVE");

    // Total time depth: use ms-per-row derived from radio tile timecodes.
    // This is the radio's own clock — stable and accurate to actual scroll rate.
    const float msPerRow = m_wfMsPerRow > 0 ? m_wfMsPerRow : 100.0f;
    const QRect labelRect = strip.adjusted(0, 4, 0, 0);
    const float totalSec = labelRect.height() * msPerRow / 1000.0f;
    if (totalSec <= 0) return;

    QFont f = p.font();
    f.setPointSize(7);
    f.setBold(false);
    p.setFont(f);
    const QFontMetrics fm(f);

    const float stepSec = 5.0f;

    for (float sec = 0; sec <= totalSec; sec += stepSec) {
        const float frac = sec / totalSec;
        const int y = labelRect.top() + static_cast<int>(frac * labelRect.height());
        if (y > wfRect.bottom() - 5) continue;

        // Tick mark
        p.setPen(QColor(0x50, 0x70, 0x80));
        p.drawLine(stripX, y, stripX + 4, y);

        const QString label = m_wfLive
            ? QString("%1s").arg(static_cast<int>(sec))
            : pausedTimeLabelForAge(m_wfHistoryOffsetRows
                                    + static_cast<int>(std::round(sec * 1000.0f / msPerRow)));

        p.setPen(QColor(0x80, 0xa0, 0xb0));
        if (m_wfLive) {
            p.drawText(stripX + 6, y + fm.ascent() / 2, label);
        } else {
            const QRect textRect(stripX + 6, y - fm.height() / 2, strip.width() - 10, fm.height());
            p.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, label);
        }
    }
}

// ─── Off-screen VFO indicator ─────────────────────────────────────────────────

void SpectrumWidget::drawOffScreenSlices(QPainter& p, const QRect& specRect)
{
    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double endMhz   = m_centerMhz + m_bandwidthMhz / 2.0;

    m_offScreenRects.resize(m_sliceOverlays.size());
    int leftStack = 0, rightStack = 0;  // vertical stacking counters

    for (int oi = 0; oi < m_sliceOverlays.size(); ++oi) {
        const auto& so = m_sliceOverlays[oi];
        m_offScreenRects[oi] = QRect();

        if (so.freqMhz >= startMhz && so.freqMhz <= endMhz) continue;

        const bool isRight = (so.freqMhz > endMhz);
        const QColor col = sliceColor(so.sliceId, so.isActive);
        const QChar letter = QChar('A' + (so.sliceId % kSliceColorCount));

        long long hz = static_cast<long long>(std::round(so.freqMhz * 1e6));
        int mhzPart = static_cast<int>(hz / 1000000);
        int khzPart = static_cast<int>((hz / 1000) % 1000);
        const QString freqStr = QString("%1.%2").arg(mhzPart).arg(khzPart, 3, 10, QChar('0'));

        const int hovAlpha = (oi == m_hoveringOffScreenIdx) ? 230 : (so.isActive ? 180 : 100);
        const int padH = 4;

        QFont bigFont = p.font(); bigFont.setPointSize(16); bigFont.setBold(true);
        QFont smallFont = p.font(); smallFont.setPointSize(10); smallFont.setBold(true);
        const QFontMetrics bigFm(bigFont);
        const QFontMetrics smallFm(smallFont);

        const QString chevron = isRight ? " >" : "< ";
        const QString sliceAndChevron = isRight ? QString(letter) + chevron : chevron + letter;

        int topLineW = bigFm.horizontalAdvance(sliceAndChevron);
        int txW = 0;
        if (so.isTxSlice) { txW = smallFm.horizontalAdvance("TX "); topLineW += txW; }
        const int freqW = smallFm.horizontalAdvance(freqStr);
        const int boxW = std::max(topLineW, freqW) + 2 * padH;
        const int boxH = bigFm.height() + smallFm.height() + 4;

        int& stackCount = isRight ? rightStack : leftStack;
        const int boxY = specRect.top() + 20 + stackCount * (boxH + 4);
        ++stackCount;

        int boxX;
        if (isRight) {
            boxX = specRect.right() - DBM_STRIP_W - boxW - 4;
        } else {
            int leftMargin = 4;
            if (m_overlayMenu && m_overlayMenu->isVisible())
                leftMargin = m_overlayMenu->width() + 2;
            boxX = specRect.left() + leftMargin;
        }

        m_offScreenRects[oi] = QRect(boxX, boxY, boxW, boxH);

        // Draw with slice color
        p.setPen(QColor(col.red(), col.green(), col.blue(), hovAlpha));
        int tx = boxX + padH;
        const int topBaseline = boxY + bigFm.ascent();

        if (isRight) {
            if (so.isTxSlice) { p.setFont(smallFont); p.drawText(tx, boxY + smallFm.ascent(), "TX "); tx += txW; }
            p.setFont(bigFont); p.drawText(tx, topBaseline, sliceAndChevron);
        } else {
            p.setFont(bigFont); p.drawText(tx, topBaseline, sliceAndChevron);
            tx += bigFm.horizontalAdvance(sliceAndChevron);
            if (so.isTxSlice) { p.setFont(smallFont); p.drawText(tx, boxY + smallFm.ascent(), " TX"); }
        }

        p.setFont(smallFont);
        p.setPen(QColor(col.red(), col.green(), col.blue(), hovAlpha));
        const int freqY = topBaseline + smallFm.height() + 2;
        if (isRight) p.drawText(boxX + boxW - padH - freqW, freqY, freqStr);
        else         p.drawText(boxX + padH, freqY, freqStr);
    }
}

} // namespace AetherSDR
