#include "SpectrumWidget.h"
#include "SpectrumOverlayMenu.h"
#include "VfoWidget.h"
#include "SliceColors.h"
#include "SliceColorManager.h"
#include "SliceLabel.h"
#include <QVariant>
#include <QVariantAnimation>

#ifdef AETHER_GPU_SPECTRUM
#include <rhi/qrhi.h>
#include <QFile>
#endif

#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScopeGuard>
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
#include <QDoubleSpinBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QEvent>
#include <QStringList>
#include <QUrl>
#include "core/AppSettings.h"
#include "core/KiwiSdrProtocol.h"
#include "InteractionSettings.h"
#include "models/BandPlanManager.h"
#include "models/BandDefs.h"
#include <QDateTime>
#include <QTimeZone>
#include <QElapsedTimer>
#include <QVarLengthArray>
#include "core/LogManager.h"
#include "core/PerfTelemetry.h"
#include <QSoundEffect>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include "core/ThemeManager.h"

namespace AetherSDR {

bool SpectrumWidget::s_starstruckMode = false;
QSoundEffect* SpectrumWidget::s_starstruckSound = nullptr;

inline QColor kAetherBrandBlue() { return AetherSDR::ThemeManager::instance().color("color.accent"); }
inline QColor kAetherBrandGreen() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
inline QColor kConnectionTextColor() { return AetherSDR::ThemeManager::instance().color("color.text.primary"); }
static constexpr float kMinDisplayDbm = -180.0f;
static constexpr int kWaterfallLineDurationMinMs = 1;
static constexpr int kWaterfallLineDurationMaxMs = 100;
static constexpr int kWaterfallHistoryCapacityMsPerRow = 50;
static constexpr int kWaterfallRatePercentMin = 1;
static constexpr int kWaterfallRatePercentMax = 100;
static constexpr float kKiwiSdrWaterfallMinDbm = -130.0f;
static constexpr float kKiwiSdrWaterfallMaxDbm = 0.0f;
static constexpr int kNativeWaterfallFallbackMinTimeoutMs = 2000;
static constexpr int kNativeWaterfallFallbackMaxTimeoutMs = 20000;
static constexpr int kNativeWaterfallRateChangeGraceMaxMs = 14000;
static constexpr int kDbmReleaseHoldFrames = 10;
static constexpr int kDbmReleaseErrorSampleCount = 256;
static constexpr float kDbmReleasePreviewChangeThresholdDb = 0.05f;
static constexpr float kDbmReleaseRebaseMinImprovementDb = 0.75f;
static constexpr int kFilterEdgeGrabPx = 8;
static constexpr int kFilterPassbandMinBodyPx = 6;
static constexpr const char* kSliceCursorOverrideActiveProperty =
    "_aetherSliceCursorOverrideActive";
static constexpr const char* kSliceCursorOverrideHadCursorProperty =
    "_aetherSliceCursorOverrideHadCursor";
static constexpr const char* kSliceCursorOverrideShapeProperty =
    "_aetherSliceCursorOverrideShape";

static Qt::CursorShape normalizedSpectrumCursorShape(Qt::CursorShape shape)
{
#ifdef Q_OS_MAC
    switch (shape) {
    case Qt::SplitVCursor:
        return Qt::SizeVerCursor;
    case Qt::SizeAllCursor:
        return Qt::OpenHandCursor;
    default:
        break;
    }
#endif
    return shape;
}

static void setCursorOverride(QWidget* widget, Qt::CursorShape shape)
{
    if (!widget) {
        return;
    }
    if (!widget->property(kSliceCursorOverrideActiveProperty).toBool()) {
        widget->setProperty(kSliceCursorOverrideHadCursorProperty,
                            widget->testAttribute(Qt::WA_SetCursor));
        widget->setProperty(kSliceCursorOverrideShapeProperty,
                            static_cast<int>(widget->cursor().shape()));
        widget->setProperty(kSliceCursorOverrideActiveProperty, true);
    }
    widget->setCursor(normalizedSpectrumCursorShape(shape));
}

static void clearCursorOverride(QWidget* widget)
{
    if (!widget
        || !widget->property(kSliceCursorOverrideActiveProperty).toBool()) {
        return;
    }

    const bool hadCursor =
        widget->property(kSliceCursorOverrideHadCursorProperty).toBool();
    const Qt::CursorShape previousShape = static_cast<Qt::CursorShape>(
        widget->property(kSliceCursorOverrideShapeProperty).toInt());
    widget->setProperty(kSliceCursorOverrideActiveProperty, false);
    widget->setProperty(kSliceCursorOverrideHadCursorProperty, QVariant());
    widget->setProperty(kSliceCursorOverrideShapeProperty, QVariant());
    if (hadCursor) {
        widget->setCursor(previousShape);
    } else {
        widget->unsetCursor();
    }
}

static int filterInteriorGrabPx(int loX, int hiX, int grabPx)
{
    const int widthPx = std::abs(hiX - loX);
    if (widthPx <= kFilterPassbandMinBodyPx) {
        return 0;
    }
    return std::min(grabPx, (widthPx - kFilterPassbandMinBodyPx - 1) / 2);
}

static int filterEdgeHitAtPixel(int mx, int loX, int hiX, int grabPx)
{
    const int left = std::min(loX, hiX);
    const int right = std::max(loX, hiX);
    const bool insidePassband = mx >= left && mx <= right;
    if (insidePassband && right - left <= kFilterPassbandMinBodyPx) {
        return 0;
    }

    const int insideGrabPx = filterInteriorGrabPx(loX, hiX, grabPx);
    const bool lowIsLeft = loX <= hiX;
    const bool lowHit = lowIsLeft
        ? (mx >= loX - grabPx && mx <= loX + insideGrabPx)
        : (mx >= loX - insideGrabPx && mx <= loX + grabPx);
    const bool highHit = lowIsLeft
        ? (mx >= hiX - insideGrabPx && mx <= hiX + grabPx)
        : (mx >= hiX - grabPx && mx <= hiX + insideGrabPx);

    if (lowHit && highHit) {
        return (std::abs(mx - loX) <= std::abs(mx - hiX)) ? -1 : 1;
    }
    if (lowHit) {
        return -1;
    }
    if (highHit) {
        return 1;
    }
    return 0;
}

static bool filterPassbandBodyHitAtPixel(int mx, int loX, int hiX, int grabPx)
{
    const int left = std::min(loX, hiX);
    const int right = std::max(loX, hiX);
    return mx >= left && mx <= right
        && filterEdgeHitAtPixel(mx, loX, hiX, grabPx) == 0;
}

static constexpr int lineDurationToRatePercent(int lineDurationMs)
{
    return std::clamp(lineDurationMs,
                      kWaterfallLineDurationMinMs,
                      kWaterfallLineDurationMaxMs);
}

static constexpr int ratePercentToLineDuration(int ratePercent)
{
    // See SpectrumOverlayMenu.cpp: the rate control value is intentionally sent
    // directly as line_duration. The tested behavior is 1 slowest, 100 fastest.
    return std::clamp(ratePercent,
                      kWaterfallRatePercentMin,
                      kWaterfallRatePercentMax);
}

static_assert(ratePercentToLineDuration(1) == 1);
static_assert(ratePercentToLineDuration(100) == 100);
static_assert(lineDurationToRatePercent(1) == 1);
static_assert(lineDurationToRatePercent(100) == 100);

static float lineDurationToVisualMsPerRow(int lineDurationMs)
{
    const int clamped = std::clamp(lineDurationMs,
                                   kWaterfallLineDurationMinMs,
                                   kWaterfallLineDurationMaxMs);
    struct RateCalibration {
        int ratePercent;
        float msPerRow;
    };
    static constexpr RateCalibration kMeasuredRateCurve[] = {
        {1, 6000.0f},
        {8, 4000.9f},
        {50, 677.2f},
        {56, 473.2f},
        {67, 223.0f},
        {69, 192.1f},
        {71, 163.9f},
        {75, 120.7f},
        {77, 102.0f},
        {78, 90.5f},
        {79, 88.8f},
        {80, 81.2f},
        {83, 64.2f},
        {90, 46.3f},
        {93, 42.0f},
        {100, 42.0f},
    };

    if (clamped <= kMeasuredRateCurve[0].ratePercent) {
        return kMeasuredRateCurve[0].msPerRow;
    }
    constexpr int kMeasuredRateCurveSize =
        static_cast<int>(sizeof(kMeasuredRateCurve) / sizeof(kMeasuredRateCurve[0]));
    for (int i = 1; i < kMeasuredRateCurveSize; ++i) {
        const RateCalibration& lower = kMeasuredRateCurve[i - 1];
        const RateCalibration& upper = kMeasuredRateCurve[i];
        if (clamped <= upper.ratePercent) {
            const float fraction = static_cast<float>(clamped - lower.ratePercent)
                / static_cast<float>(upper.ratePercent - lower.ratePercent);
            const float lowerLog = std::log(lower.msPerRow);
            const float upperLog = std::log(upper.msPerRow);
            return std::exp(lowerLog + (upperLog - lowerLog) * fraction);
        }
    }
    return kMeasuredRateCurve[kMeasuredRateCurveSize - 1].msPerRow;
}

static bool spotMarkersVisuallyEqual(const QVector<SpectrumWidget::SpotMarker>& lhs,
                                     const QVector<SpectrumWidget::SpotMarker>& rhs)
{
    constexpr double kFrequencyEpsilonMhz = 1.0e-6;

    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (qsizetype i = 0; i < lhs.size(); ++i) {
        const SpectrumWidget::SpotMarker& a = lhs.at(i);
        const SpectrumWidget::SpotMarker& b = rhs.at(i);
        if (a.callsign != b.callsign
            || std::abs(a.freqMhz - b.freqMhz) > kFrequencyEpsilonMhz
            || a.color != b.color
            || a.backgroundColor != b.backgroundColor
            || a.dxccColor != b.dxccColor
            || a.source != b.source) {
            return false;
        }
    }

    return true;
}

class PerfInputScope {
public:
    explicit PerfInputScope(const char* kind)
        : m_kind(kind),
          m_enabled(PerfTelemetry::instance().enabled()),
          m_startNs(m_enabled ? PerfTelemetry::nowNs() : 0)
    {
    }

    ~PerfInputScope()
    {
        if (!m_enabled)
            return;
        PerfTelemetry::instance().recordInputEvent(
            m_kind,
            static_cast<double>(PerfTelemetry::nowNs() - m_startNs) / 1000000.0);
    }

private:
    const char* m_kind;
    bool m_enabled;
    qint64 m_startNs;
};

class PerfUpdateScope {
public:
    enum class Kind {
        Panadapter,
        Waterfall
    };

    explicit PerfUpdateScope(Kind kind)
        : m_kind(kind),
          m_enabled(PerfTelemetry::instance().enabled()),
          m_startNs(m_enabled ? PerfTelemetry::nowNs() : 0)
    {
    }

    ~PerfUpdateScope()
    {
        if (!m_enabled)
            return;
        const double durationMs = static_cast<double>(PerfTelemetry::nowNs() - m_startNs) / 1000000.0;
        if (m_kind == Kind::Waterfall)
            PerfTelemetry::instance().recordWaterfallUpdate(durationMs);
        else
            PerfTelemetry::instance().recordPanUpdate(durationMs);
    }

private:
    Kind m_kind;
    bool m_enabled;
    qint64 m_startNs;
};

template <typename F>
class ScopeExit {
public:
    explicit ScopeExit(F&& fn)
        : m_fn(std::forward<F>(fn))
    {
    }

    ~ScopeExit()
    {
        m_fn();
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F m_fn;
};

template <typename F>
ScopeExit<F> makeScopeExit(F&& fn)
{
    return ScopeExit<F>(std::forward<F>(fn));
}

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

static QString formatFreqScaleLabel(double freqMhz, int decimals)
{
    QString label = QString::number(freqMhz, 'f', decimals);
    const qsizetype dot = label.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        return label;
    }

    const qsizetype minDecimals = std::min(decimals, 3);
    const qsizetype minLength = dot + 1 + minDecimals;
    while (label.size() > minLength && label.endsWith(QLatin1Char('0'))) {
        label.chop(1);
    }
    return label;
}

// ─── Waterfall color scheme gradient cache ────────────────────────────────────
//
// The five preset schemes (Default, Grayscale, Blue-Green, Fire, Plasma) used
// to live as compile-time const tables.  They now resolve through ThemeManager
// against `color.waterfall.colormap.{default,grayscale,blueGreen,fire,plasma}`
// gradient tokens so a theme switch (or user theme override) reshapes any of
// them.  Cached once per theme load — `intensityToRgb` and `fftDbmToRgb` hit
// this hundreds of times per second per row, so we can't afford a token
// lookup on every pixel.
//
// Invalidated by ThemeManager::themeChanged via a SpectrumWidget connection
// in the constructor; first call lazily populates if the cache hasn't been
// initialized yet.

namespace {

struct WfStopsCache {
    std::array<std::vector<WfGradientStop>, static_cast<int>(WfColorScheme::Count)> stops;
    bool initialized = false;
};

WfStopsCache& wfStopsCache()
{
    static WfStopsCache c;
    return c;
}

const char* wfSchemeToken(WfColorScheme s)
{
    switch (s) {
    case WfColorScheme::Grayscale: return "color.waterfall.colormap.grayscale";
    case WfColorScheme::BlueGreen: return "color.waterfall.colormap.blueGreen";
    case WfColorScheme::Fire:      return "color.waterfall.colormap.fire";
    case WfColorScheme::Plasma:    return "color.waterfall.colormap.plasma";
    case WfColorScheme::Purple:    return "color.waterfall.colormap.purple";
    default:                       return "color.waterfall.colormap.default";
    }
}

void rebuildWfStopsCacheFromTheme()
{
    auto& c = wfStopsCache();
    auto& tm = ThemeManager::instance();
    for (int i = 0; i < static_cast<int>(WfColorScheme::Count); ++i) {
        const QBrush br = tm.brush(QString::fromLatin1(
            wfSchemeToken(static_cast<WfColorScheme>(i))));
        std::vector<WfGradientStop> v;
        if (br.gradient()) {
            for (const auto& gs : br.gradient()->stops()) {
                WfGradientStop s;
                s.pos = static_cast<float>(gs.first);
                s.r   = gs.second.red();
                s.g   = gs.second.green();
                s.b   = gs.second.blue();
                v.push_back(s);
            }
        }
        // Fallback so a missing/malformed token still produces a usable
        // black→white ramp instead of a divide-by-zero on the first paint.
        if (v.size() < 2)
            v = { {0.0f, 0, 0, 0}, {1.0f, 255, 255, 255} };
        c.stops[i] = std::move(v);
    }
    c.initialized = true;
}

} // namespace

const WfGradientStop* wfSchemeStops(WfColorScheme scheme, int& count)
{
    auto& c = wfStopsCache();
    if (!c.initialized)
        rebuildWfStopsCacheFromTheme();
    const auto& v = c.stops[static_cast<int>(scheme)];
    count = static_cast<int>(v.size());
    return v.data();
}

const char* wfSchemeName(WfColorScheme scheme)
{
    switch (scheme) {
    case WfColorScheme::Grayscale: return "Grayscale";
    case WfColorScheme::BlueGreen: return "Blue-Green";
    case WfColorScheme::Fire:      return "Fire";
    case WfColorScheme::Plasma:    return "Plasma";
    case WfColorScheme::Purple:    return "Purple";
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

#ifdef AETHER_GPU_SPECTRUM
static bool qtSoftwareOpenGlRequested()
{
    return qEnvironmentVariableIsSet("AETHER_NO_GPU")
        || qEnvironmentVariable("QT_OPENGL").compare(
            QStringLiteral("software"), Qt::CaseInsensitive) == 0;
}

static bool qrhiDeviceNameLooksSoftware(const QString& deviceName)
{
    const QString lower = deviceName.toLower();
    return lower.contains(QStringLiteral("llvmpipe"))
        || lower.contains(QStringLiteral("softpipe"))
        || lower.contains(QStringLiteral("swiftshader"))
        || lower.contains(QStringLiteral("software"))
        || lower.contains(QStringLiteral("warp"))
        || lower.contains(QStringLiteral("microsoft basic render"));
}
#endif

QString SpectrumWidget::rendererDescription() const
{
#ifdef AETHER_GPU_SPECTRUM
    QRhi* currentRhi = rhi();
    if (!currentRhi) {
        return QStringLiteral("QRhi initializing");
    }

    const QString backendName = QString::fromLatin1(currentRhi->backendName());
    const QRhiDriverInfo driverInfo = currentRhi->driverInfo();
    const QString deviceName = QString::fromUtf8(driverInfo.deviceName).trimmed();
    const bool softwareOpenGl = currentRhi->backend() == QRhi::OpenGLES2
        && qtSoftwareOpenGlRequested();
    const bool cpuDevice = driverInfo.deviceType == QRhiDriverInfo::CpuDevice
        || softwareOpenGl
        || qrhiDeviceNameLooksSoftware(deviceName);

    QStringList details;
    details << backendName;
    if (!deviceName.isEmpty()) {
        details << deviceName;
    }
    if (softwareOpenGl) {
        details << QStringLiteral("software OpenGL");
    }

    return QStringLiteral("%1 QRhi (%2)")
        .arg(cpuDevice ? QStringLiteral("CPU") : QStringLiteral("GPU"),
             details.join(QStringLiteral("; ")));
#else
    return QStringLiteral("CPU QPainter");
#endif
}

SpectrumWidget::SpectrumWidget(QWidget* parent)
    : SPECTRUM_BASE_CLASS(parent)
{
    // Container declaration — every token lookup made by this widget or
    // any of its children (without their own declaration) resolves
    // through the "spectrum" scope chain.  Token migration into this
    // scope happens in step 4 of the refactor.
    theme::setContainer(this, QStringLiteral("spectrum"));

    setMinimumHeight(100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
    setAccessibleName(tr("Panadapter spectrum display"));
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

    m_interlockNotificationLabel = new QLabel(this);
    m_interlockNotificationLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_interlockNotificationLabel->setAlignment(Qt::AlignCenter);
    m_interlockNotificationLabel->setWordWrap(true);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_interlockNotificationLabel, "QLabel { background: rgba(10,10,20,225); color: #d7fbff; "
        "border: 2px solid {{color.accent}}; padding: 10px 14px; "
        "font-size: 13px; font-weight: bold; }");
    m_interlockNotificationLabel->hide();
    m_interlockNotificationLabel->raise();

    m_interlockNotificationTimer = new QTimer(this);
    m_interlockNotificationTimer->setSingleShot(true);
    connect(m_interlockNotificationTimer, &QTimer::timeout, this, [this]() {
        m_interlockNotificationLabel->hide();
    });

    // Tune guide auto-hide timer (2-second inactivity timeout)
    m_tuneGuideTimer = new QTimer(this);
    m_tuneGuideTimer->setSingleShot(true);
    m_tuneGuideTimer->setInterval(4000);
    connect(m_tuneGuideTimer, &QTimer::timeout, this, [this]() {
        m_tuneGuideVisible = false;
        markOverlayDirty();
    });

    // SQL threshold line auto-hide (manual SQL only — see setSquelchLine).
    // Shows for 3 s on enable or slider movement; slider re-adjust resets it.
    // Auto SQL pins the line at the tracked level so the timer is skipped.
    m_squelchLineHideTimer = new QTimer(this);
    m_squelchLineHideTimer->setSingleShot(true);
    m_squelchLineHideTimer->setInterval(3000);
    connect(m_squelchLineHideTimer, &QTimer::timeout, this, [this]() {
        m_squelchLineVisible = false;
        markOverlayDirty();
    });

    m_connectionAnimationTimer = new QTimer(this);
    m_connectionAnimationTimer->setInterval(40);
    connect(m_connectionAnimationTimer, &QTimer::timeout, this, [this]() {
        if (m_connectionAnimationVisible)
            markOverlayDirty();
    });

    m_fpsMeterTimer = new QTimer(this);
    m_fpsMeterTimer->setInterval(1000);
    connect(m_fpsMeterTimer, &QTimer::timeout, this, [this]() {
        updateFpsMeterValues();
    });
    createFpsMeterLabels();

    // Continuous edge auto-pan while dragging a slice (user-reported).  While the
    // cursor sits in the edge zone this timer drives a real pan velocity (see
    // edgePanVelocityStep) so the band keeps scrolling without further
    // mouse-move events.  Velocity knobs are env-tunable so the feel can be
    // swept without rebuilding; AETHER_NO_DRAG_EDGEPAN=1 disables the whole
    // thing (restores the legacy reveal-only behaviour for A/B testing).
    m_vfoDragEdgePanDisabled = qEnvironmentVariableIntValue("AETHER_NO_DRAG_EDGEPAN") != 0;
    if (int v = qEnvironmentVariableIntValue("AETHER_DRAG_EDGEPAN_VMAX"); v > 0) {
        m_edgePanVmaxPctBw = v;
    }
    if (int v = qEnvironmentVariableIntValue("AETHER_DRAG_EDGEPAN_RAMP"); v > 0) {
        m_edgePanRampMs = v;
    }
    if (int v = qEnvironmentVariableIntValue("AETHER_DRAG_EDGEPAN_INTERVAL"); v > 0) {
        m_edgePanIntervalMs = v;
    }
    m_vfoDragEdgePanTimer = new QTimer(this);
    m_vfoDragEdgePanTimer->setInterval(m_edgePanIntervalMs);
    connect(m_vfoDragEdgePanTimer, &QTimer::timeout, this, [this]() {
        if (m_draggingVfo) {
            edgePanVelocityStep();
        } else {
            m_vfoDragEdgePanTimer->stop();
        }
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
        "QPushButton:checked { background: rgba(0,180,216,210); color: #000; }"
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
        newCenter = std::max(newCenter, newBw / 2.0);

        handleWaterfallFrequencyFrameChange(m_centerMhz, m_bandwidthMhz,
                                            newCenter, newBw);
        if (!reprojectSpectrum(m_centerMhz, m_bandwidthMhz, newCenter, newBw)) {
            m_bins.clear();
            m_smoothed.clear();
        }
        m_centerMhz = newCenter;
        m_bandwidthMhz = newBw;
        resetNoiseFloorBaseline();
        markOverlayDirty();
        emit frequencyRangeChangeRequested(newCenter, newBw);
    };
    connect(m_zoomOutBtn, &QPushButton::clicked, this, [emitZoom]() { emitZoom(1.5); });
    connect(m_zoomInBtn,  &QPushButton::clicked, this, [emitZoom]() { emitZoom(1.0 / 1.5); });

    connect(&SliceColorManager::instance(), &SliceColorManager::colorsChanged,
            this, [this]() { markOverlayDirty(); });

    // Refresh the waterfall colormap cache when the theme changes.  Existing
    // already-rendered rows keep their pre-switch colours (same behaviour as
    // changing the Scheme: dropdown — recolouring history would force a full
    // O(rows × cols) repaint we don't currently amortise).  New rows pick up
    // the new palette on the next push.
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this]() {
        rebuildWfStopsCacheFromTheme();
        markOverlayDirty();
        update();
    });

    // Phase 5 PR 3 — inspector coverage.  SpectrumWidget paints through
    // raw QPainter calls keyed off ThemeManager::instance().color(),
    // bypassing the applyStyleSheet reverse-map.  Declare the tokens it
    // reads so an Inspect-mode click anywhere on the panadapter or
    // waterfall surfaces a meaningful (if coarse) hit-list.  Sub-region
    // splits (trace vs grid vs waterfall vs slice triangles) come in a
    // follow-up PR via declareWidgetRegions().
    ThemeManager::instance().declareWidgetTokens(this, QStringList{
        "color.background.0",
        "color.background.1",
        "color.background.2",
        "color.background.3",
        "color.background.spectrum",
        "color.spectrum.trace",
        "color.spectrum.peakHold",
        "color.spectrum.average",
        "color.spectrum.grid",
        "color.text.primary",
        "color.text.secondary",
        "color.text.label",
        "color.accent",
        "color.accent.bright",
        "color.accent.dim",
        "color.accent.warning",
        "color.accent.success",
        "color.waterfall.colormap",
        "color.slice.a", "color.slice.b", "color.slice.c", "color.slice.d",
        "color.slice.e", "color.slice.f", "color.slice.g", "color.slice.h",
        "color.slice.tx",
    });
}

SpectrumWidget::~SpectrumWidget()
{
    prepareForShutdown();
}

void SpectrumWidget::prepareForTopLevelChange()
{
#ifdef AETHER_GPU_SPECTRUM
#ifdef Q_OS_MAC
    // QRhiWidget registers a cleanup callback with the current top-level
    // backing-store QRhi. Direct splitter/floating-window reparenting can miss
    // Qt's internal notification, leaving the old QRhi with a stale callback.
    QEvent event(QEvent::WindowAboutToChangeInternal);
    QCoreApplication::sendEvent(this, &event);
#endif
#endif
}

void SpectrumWidget::prepareForShutdown()
{
    if (m_shutdownPrepared) {
        return;
    }
    m_shutdownPrepared = true;

    prepareForTopLevelChange();
    setUpdatesEnabled(false);
    hide();

#ifdef AETHER_GPU_SPECTRUM
    releaseResources();
#ifdef Q_OS_MAC
    // Drop the native child window while its parent backing store is still
    // alive, so any remaining platform resources are gone before QWidgetWindow
    // destruction runs on app exit.
    destroy(true, true);
#endif
#endif
}

// ── Multi-VfoWidget management ────────────────────────────────────────────────

VfoWidget* SpectrumWidget::vfoWidget(int sliceId) const
{
    return m_vfoWidgets.value(sliceId, nullptr);
}

bool SpectrumWidget::sliceHasSplitPartner(int sliceId) const
{
    for (const auto& so : m_sliceOverlays) {
        if (so.sliceId == sliceId)
            return so.splitPartnerId >= 0;
    }
    return false;
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
    // Auto-black source defaults to client-side (legacy look); radio-side opt-in.
    m_wfAutoBlackRadioSide = s.value(settingsKey("DisplayWfAutoBlackRadioSide"), "False").toString() == "True";
    m_wfLineDuration = std::clamp(s.value(settingsKey("DisplayWfLineDuration"), "100").toInt(),
                                  kWaterfallLineDurationMinMs,
                                  kWaterfallLineDurationMaxMs);
    PerfTelemetry::instance().setWaterfallLineDurationMs(m_wfLineDuration);
    resetWfTimeScale();
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
    m_bandPlanShowSpots = s.value("BandPlanShowSpots", "True").toString() == "True";
    m_fftHeatMap     = s.value(settingsKey("DisplayFftHeatMap"), "True").toString() == "True";
    m_showGrid       = s.value(settingsKey("DisplayShowGrid"), "True").toString() == "True";
    m_freqGridSpacingKhz = s.value(settingsKey("DisplayFreqGridSpacing"), "0").toInt();
    m_freqScaleFontPt = std::clamp(
        s.value(settingsKey("DisplayFreqScaleFontPt"), "8").toInt(), 8, 14);
    m_fftLineWidth   = s.value(settingsKey("DisplayFftLineWidth"), "2.0").toFloat();
    m_noiseFloorEnable   = s.value(settingsKey("DisplayNoiseFloorEnable"), "False").toString() == "True";
    m_noiseFloorPosition = std::clamp(
        s.value(settingsKey("DisplayNoiseFloorPosition"), "75").toInt(), 1, 99);
    // Match the enable-time fresh-frame seed used by setNoiseFloorEnable so a
    // restored Floor=on locks onto the current floor without smoothing from a
    // stale value.
    if (m_noiseFloorEnable) {
        armNoiseFloorFastLock(5, 1);
    }
    applyFpsMeterVisibility(
        s.value("DisplayFpsMeters", "False").toString() == "True");
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
    {
        const QString hex = s.value(settingsKey("BackgroundFillColor"), "#0a0a14").toString();
        QColor c(hex);
        if (c.isValid()) m_bgFillColor = c;
    }

    // Sync overlay menu sliders with restored settings
    if (m_overlayMenu) {
        m_overlayMenu->syncDisplaySettings(m_fftAverage, m_fftFps,
            static_cast<int>(m_fftFillAlpha * 100), m_fftWeightedAvg, m_fftFillColor,
            m_wfColorGain, m_wfBlackLevel, m_wfAutoBlack, m_wfAutoBlackOffset,
            m_wfLineDuration,
            m_noiseFloorPosition, m_noiseFloorEnable,
            m_fftHeatMap, static_cast<int>(m_wfColorScheme), m_showGrid,
            m_fftLineWidth, m_wfAutoBlackRadioSide);
        m_overlayMenu->syncExtraDisplaySettings(m_wfBlankerEnabled,
            m_wfBlankerThreshold, m_bgOpacity, m_freqGridSpacingKhz, m_bgFillColor,
            m_freqScaleFontPt);
    }
    // Refresh the noise-floor target so the slider position takes effect
    // even when the overlay menu is built but not yet shown.
    refreshNoiseFloorTarget();
}

VfoWidget* SpectrumWidget::addVfoWidget(int sliceId)
{
    if (m_vfoWidgets.contains(sliceId))
        return m_vfoWidgets[sliceId];

    auto* w = new VfoWidget(this);
    installVfoCursorEventFilter(w);
    m_vfoWidgets[sliceId] = w;
    w->show();
    w->raise();
    m_overlayMenu->raiseAll();  // keep overlay + panels on top of all VFO widgets
    if (m_interlockNotificationLabel && m_interlockNotificationLabel->isVisible())
        m_interlockNotificationLabel->raise();
    return w;
}

void SpectrumWidget::installVfoCursorEventFilter(VfoWidget* widget)
{
    if (!widget) {
        return;
    }

    widget->setMouseTracking(true);
    widget->installEventFilter(this);
    const QList<QWidget*> children = widget->findChildren<QWidget*>();
    for (QWidget* child : children) {
        child->setMouseTracking(true);
        child->installEventFilter(this);
    }
}

void SpectrumWidget::setVfoCursorOverride(Qt::CursorShape shape)
{
    for (QMap<int, VfoWidget*>::const_iterator it = m_vfoWidgets.cbegin();
         it != m_vfoWidgets.cend(); ++it) {
        VfoWidget* widget = it.value();
        if (!widget) {
            continue;
        }
        setCursorOverride(widget, shape);
        const QList<QWidget*> children = widget->findChildren<QWidget*>();
        for (QWidget* child : children) {
            setCursorOverride(child, shape);
        }
    }
}

void SpectrumWidget::clearVfoCursorOverride()
{
    for (QMap<int, VfoWidget*>::const_iterator it = m_vfoWidgets.cbegin();
         it != m_vfoWidgets.cend(); ++it) {
        VfoWidget* widget = it.value();
        if (!widget) {
            continue;
        }
        clearCursorOverride(widget);
        const QList<QWidget*> children = widget->findChildren<QWidget*>();
        for (QWidget* child : children) {
            clearCursorOverride(child);
        }
    }
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
        if (m_interlockNotificationLabel && m_interlockNotificationLabel->isVisible())
            m_interlockNotificationLabel->raise();
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
void SpectrumWidget::setNoiseFloorPosition(int pos) {
    m_noiseFloorPosition = std::clamp(pos, 1, 99);
    m_pendingDbmRangeEcho = false;
    m_pendingDbmRangeEchoFromAutoFloor = false;
    m_pendingDbmRangeEchoStartMs = 0;
    refreshNoiseFloorTarget();
    if (m_noiseFloorEnable) {
        const QVector<float>& baselineBins = noiseFloorAutoLevelBins();
        if (!m_noiseFloorBaselineValid && !baselineBins.isEmpty()) {
            const float baselineDbm = estimateNoiseFloorDbm(baselineBins);
            if (baselineDbm > -500.0f) {
                m_noiseFloorBaselineDbm = baselineDbm;
                m_noiseFloorBaselineValid = true;
                m_noiseFloorLastSampleMs = QDateTime::currentMSecsSinceEpoch();
                m_noiseFloorCandidateValid = false;
                m_noiseFloorCandidateFrames = 0;
            }
        }
        applyNoiseFloorAutoAdjust(QDateTime::currentMSecsSinceEpoch());
    }
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayNoiseFloorPosition"), QString::number(m_noiseFloorPosition));
    s.save();
}
void SpectrumWidget::setNoiseFloorEnable(bool on) {
    m_pendingDbmRangeEcho = false;
    m_pendingDbmRangeEchoFromAutoFloor = false;
    m_pendingDbmRangeEchoStartMs = 0;
    m_noiseFloorEnable = on;
    resetNoiseFloorBaseline();
    // Five fresh frames after enable so we lock onto the current
    // floor without smoothing from a stale value.
    if (on) {
        armNoiseFloorFastLock(5, 1);
    }
    if (on) {
        refreshNoiseFloorTarget();
    }
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayNoiseFloorEnable"), on ? "True" : "False");
    s.save();
}
void SpectrumWidget::reacquireNoiseFloorLock() {
    if (!m_noiseFloorEnable) return;
    m_pendingDbmRangeEcho = false;
    m_pendingDbmRangeEchoFromAutoFloor = false;
    m_pendingDbmRangeEchoStartMs = 0;
    resetNoiseFloorBaseline();
    // Antenna changes can take several FFT frames to settle after the
    // slice command, so keep cold-acquiring long enough to catch the new floor.
    // 30 frames ≈ 1 s of cold-acquire at the default 30 Hz FFT update rate —
    // long enough for the antenna change to settle through the radio.
    armNoiseFloorFastLock(30, 1);
    m_measuredNoiseFloorDbm = -1000.0f;
}

void SpectrumWidget::reacquireNoiseFloorLockFromVisibleSource()
{
    reacquireNoiseFloorLock();
    if (!m_noiseFloorEnable) {
        return;
    }

    const QVector<float>& bins = noiseFloorAutoLevelBins();
    if (bins.isEmpty()) {
        return;
    }

    const float frameFloor = estimateNoiseFloorDbm(bins);
    if (frameFloor > -500.0f) {
        m_measuredNoiseFloorDbm = frameFloor;
    }
    updateNoiseFloorBaseline(bins, true);
    if (m_noiseFloorFreshFrameCount > 0) {
        --m_noiseFloorFreshFrameCount;
    }
}

void SpectrumWidget::suspendNoiseFloorAutoAdjustUntil(qint64 untilMs)
{
    if (untilMs <= 0) {
        return;
    }

    m_noiseFloorAutoAdjustHoldUntilMs =
        std::max(m_noiseFloorAutoAdjustHoldUntilMs, untilMs);
    if (m_pendingDbmRangeEchoFromAutoFloor) {
        m_pendingDbmRangeEcho = false;
        m_pendingDbmRangeEchoFromAutoFloor = false;
        m_pendingDbmRangeEchoStartMs = 0;
    }
    m_noiseFloorCandidateValid = false;
    m_noiseFloorCandidateFrames = 0;
}

void SpectrumWidget::resumeNoiseFloorAutoAdjust()
{
    m_noiseFloorAutoAdjustHoldUntilMs = 0;
    if (!m_noiseFloorEnable) {
        return;
    }

    resetNoiseFloorBaseline();
    armNoiseFloorFastLock(8, 1);
}

void SpectrumWidget::prepareForFftScaleChange()
{
    m_resetFftSmoothingOnNextFrame = true;
    if (!m_noiseFloorEnable) return;

    constexpr qint64 kScaleSettleMs = 750;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_noiseFloorScaleSettlingUntilMs =
        std::max(m_noiseFloorScaleSettlingUntilMs, nowMs + kScaleSettleMs);
    armNoiseFloorFastLock(8, 1);
    m_noiseFloorCandidateValid = false;
    m_noiseFloorCandidateFrames = 0;
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
void SpectrumWidget::setFreqScaleFontPt(int pt) {
    m_freqScaleFontPt = std::clamp(pt, 8, 14);
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayFreqScaleFontPt"), QString::number(m_freqScaleFontPt));
    s.save();
    markOverlayDirty();
    update();
}
void SpectrumWidget::setShowFpsMeters(bool on) {
    applyFpsMeterVisibility(on);
    auto& s = AppSettings::instance();
    s.setValue("DisplayFpsMeters", on ? "True" : "False");
    s.save();

    // Keep FPS meters global across docked and sibling panadapters.
    if (QWidget* top = window()) {
        const auto siblings = top->findChildren<SpectrumWidget*>();
        for (SpectrumWidget* sw : siblings) {
            if (sw != this)
                sw->applyFpsMeterVisibility(on);
        }
    }
}
void SpectrumWidget::applyFpsMeterVisibility(bool on) {
    m_showFpsMeters = on;
    resetFpsMeterWindow();
    if (m_fpsMeterTimer) {
        if (on)
            m_fpsMeterTimer->start();
        else
            m_fpsMeterTimer->stop();
    }
    updateFpsMeterLabels();
    markOverlayDirty();
}
void SpectrumWidget::resetFpsMeterWindow() {
    m_panadapterFrameCount = 0;
    m_waterfallFrameCount = 0;
    m_kiwiSdrWaterfallFrameCount = 0;
    m_panadapterFps = 0.0;
    m_waterfallFps = 0.0;
    m_kiwiSdrWaterfallFps = 0.0;
    m_fpsMeterWindow.restart();
}
void SpectrumWidget::updateFpsMeterValues() {
    if (!m_showFpsMeters)
        return;
    if (!m_fpsMeterWindow.isValid()) {
        resetFpsMeterWindow();
        return;
    }
    const qint64 elapsedMs = m_fpsMeterWindow.elapsed();
    if (elapsedMs <= 0)
        return;

    const double scale = 1000.0 / static_cast<double>(elapsedMs);
    m_panadapterFps = m_panadapterFrameCount * scale;
    m_waterfallFps = m_waterfallFrameCount * scale;
    m_kiwiSdrWaterfallFps = m_kiwiSdrWaterfallFrameCount * scale;
    m_panadapterFrameCount = 0;
    m_waterfallFrameCount = 0;
    m_kiwiSdrWaterfallFrameCount = 0;
    m_fpsMeterWindow.restart();
    updateFpsMeterLabels();
}
void SpectrumWidget::recordPanadapterFrame() {
    if (m_showFpsMeters)
        ++m_panadapterFrameCount;
}
void SpectrumWidget::recordWaterfallFrame(int rows) {
    if (m_showFpsMeters && rows > 0)
        m_waterfallFrameCount += rows;
}
void SpectrumWidget::recordKiwiSdrWaterfallFrame(int rows) {
    if (m_showFpsMeters && rows > 0)
        m_kiwiSdrWaterfallFrameCount += rows;
}

void SpectrumWidget::createFpsMeterLabels() {
    const QString style = QStringLiteral(
        "QLabel {"
        " background: rgba(15, 15, 26, 210);"
        " border: 1px solid rgba(255, 255, 255, 170);"
        " border-radius: 3px;"
        " color: #9ceeff;"
        " font-size: 9pt;"
        " font-weight: bold;"
        " padding: 3px 6px;"
        "}");

    auto makeLabel = [this, &style]() {
        auto* label = new QLabel(this);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(style);
        if (m_overlayMenu) {
            label->stackUnder(m_overlayMenu);
        }
        label->hide();
        return label;
    };

    m_panFpsMeterLabel = makeLabel();
    m_wfFpsMeterLabel = makeLabel();
    updateFpsMeterLabels();
}

void SpectrumWidget::updateFpsMeterLabels() {
    if (!m_panFpsMeterLabel || !m_wfFpsMeterLabel) {
        return;
    }

    const double visibleWaterfallFps = m_kiwiSdrWaterfallActive
        ? m_kiwiSdrWaterfallFps
        : m_waterfallFps;
    const double visiblePanadapterFps = m_kiwiSdrWaterfallActive
        ? m_kiwiSdrWaterfallFps
        : m_panadapterFps;
    m_panFpsMeterLabel->setText(QStringLiteral("PAN %1 FPS").arg(visiblePanadapterFps, 0, 'f', 1));
    m_wfFpsMeterLabel->setText(QStringLiteral("WF %1 FPS").arg(visibleWaterfallFps, 0, 'f', 1));
    m_panFpsMeterLabel->adjustSize();
    m_wfFpsMeterLabel->adjustSize();
    positionFpsMeterLabels();
}

int SpectrumWidget::spectrumPixelHeight() const
{
    const int chromeH = freqScaleH() + DIVIDER_H;
    const int contentH = std::max(0, height() - chromeH);
    return std::max(1, static_cast<int>(contentH * m_spectrumFrac));
}

void SpectrumWidget::positionFpsMeterLabels() {
    if (!m_panFpsMeterLabel || !m_wfFpsMeterLabel) {
        return;
    }

    auto hideMeters = [this]() {
        m_panFpsMeterLabel->hide();
        m_wfFpsMeterLabel->hide();
    };

    if (!m_showFpsMeters || width() <= 0 || height() <= 0) {
        hideMeters();
        return;
    }

    const int chromeH = freqScaleH() + DIVIDER_H;
    const int contentH = height() - chromeH;
    if (contentH <= 0) {
        hideMeters();
        return;
    }

    const int specH = spectrumPixelHeight();
    const int wfY = specH + DIVIDER_H + freqScaleH();
    const QRect specRect(0, 0, width(), specH);
    const QRect wfRect(0, wfY, width(), height() - wfY);

    auto positionMeter = [](QLabel* label, const QRect& area,
                            int bottomInset, int rightInset) {
        if (area.width() < 56 || area.height() < 18) {
            label->hide();
            return;
        }

        const QSize labelSize = label->sizeHint();
        if (area.width() < labelSize.width() + rightInset + 12
            || area.height() < labelSize.height() + 8) {
            label->hide();
            return;
        }

        const int plotRight = area.right() - rightInset;
        int x = plotRight - labelSize.width() - 8;
        int y = area.bottom() - labelSize.height() - bottomInset;
        if (x + labelSize.width() > plotRight - 4) {
            x = plotRight - labelSize.width() - 4;
        }
        if (x < area.left() + 4) {
            x = area.left() + 4;
        }
        if (y + labelSize.height() > area.bottom() - 4) {
            y = area.bottom() - labelSize.height() - 4;
        }
        if (y < area.top() + 4) {
            y = area.top() + 4;
        }

        label->move(x, y);
        label->show();
    };

    const int panBottomInset = (m_bandPlanFontSize > 0)
        ? m_bandPlanFontSize + 12
        : 6;
    positionMeter(m_panFpsMeterLabel, specRect, panBottomInset, DBM_STRIP_W);
    positionMeter(m_wfFpsMeterLabel, wfRect, 6, waterfallStripWidth());
    if (m_overlayMenu) {
        m_overlayMenu->raiseAll();
    }
}

bool SpectrumWidget::anyDragActive() const {
    return m_draggingDivider
        || m_draggingBandwidth
        || m_draggingPan
        || m_draggingFilter != FilterEdge::None
        || m_draggingVfo
        || m_draggingDbm
        || m_draggingDbmRange
        || m_draggingTimeScale
        || m_draggingTimeScaleRate
        || m_draggingTnfId >= 0;
}
void SpectrumWidget::publishPerfDragState() const {
    PerfTelemetry::instance().setDragActive(anyDragActive());
}

float SpectrumWidget::estimateNoiseFloorDbm(const QVector<float>& bins) const
{
    if (bins.isEmpty()) return -1000.0f;

    // Stride-sample to cap work at ~512 reads even on very wide pans.
    const int stride = std::max(1, static_cast<int>(bins.size() / 512));
    float sum = 0.0f;
    int count = 0;
    for (int i = 0; i < bins.size(); i += stride) {
        const float v = bins[i];
        if (std::isfinite(v)) { sum += v; ++count; }
    }
    if (count <= 0) return -1000.0f;

    const float mean = sum / static_cast<float>(count);
    float baselineSum = 0.0f;
    int baselineCount = 0;
    for (int i = 0; i < bins.size(); i += stride) {
        const float v = bins[i];
        if (std::isfinite(v) && v <= mean) { baselineSum += v; ++baselineCount; }
    }
    return (baselineCount > 0) ? baselineSum / static_cast<float>(baselineCount) : mean;
}

void SpectrumWidget::clearDbmReleaseRebase()
{
    m_holdFftUpdatesAfterDbmRelease = 0;
    m_dbmReleasePreviewOldMinDbm = 0.0f;
    m_dbmReleasePreviewOldMaxDbm = 0.0f;
    m_dbmReleasePreviewNewMinDbm = 0.0f;
    m_dbmReleasePreviewNewMaxDbm = 0.0f;
}

void SpectrumWidget::resetNoiseFloorBaseline()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_noiseFloorBaselineDbm = -1000.0f;
    m_noiseFloorBaselineValid = false;
    m_noiseFloorTargetValid = false;
    m_noiseFloorLastSampleMs = 0;
    m_noiseFloorLastMotionMs = nowMs - 100;
    m_noiseFloorLastCommandMs = 0;
    m_noiseFloorLastCommandRef = m_refLevel;
    m_noiseFloorScaleSettlingUntilMs = 0;
    m_noiseFloorCandidateValid = false;
    m_noiseFloorCandidateDbm = -1000.0f;
    m_noiseFloorCandidateStartMs = 0;
    m_noiseFloorCandidateFrames = 0;
    m_noiseFloorFreshFrameCount = m_noiseFloorEnable ? 5 : 0;
    m_noiseFloorFastLockFrames = 0;
    m_pendingDbmRangeEcho = false;
    m_pendingDbmRangeEchoFromAutoFloor = false;
    m_pendingDbmRangeEchoStartMs = 0;
}

void SpectrumWidget::refreshNoiseFloorTarget(bool captureCurrentScale, bool persistCapture)
{
    if (captureCurrentScale) {
        captureNoiseFloorTargetFromCurrentScale(true, persistCapture);
    }

    if (m_noiseFloorEnable) {
        m_noiseFloorTargetFrac = std::clamp(m_noiseFloorPosition / 100.0f, 0.02f, 0.98f);
        m_noiseFloorTargetValid = true;
        if (m_noiseFloorLastMotionMs <= 0) {
            m_noiseFloorLastMotionMs = QDateTime::currentMSecsSinceEpoch() - 100;
        }
        m_noiseFloorLastCommandMs = 0;
        m_noiseFloorLastCommandRef = m_refLevel;
    } else {
        m_noiseFloorTargetValid = false;
    }
}

bool SpectrumWidget::captureNoiseFloorTargetFromCurrentScale(bool notify, bool persist)
{
    if (m_dynamicRange <= 0.0f) {
        qDebug() << "SpectrumWidget: noise-floor capture skipped — "
                    "dynamic range not yet valid";
        return false;
    }

    float baselineDbm = -1000.0f;
    const QVector<float>& baselineBins = noiseFloorAutoLevelBins();
    if (!baselineBins.isEmpty()) {
        baselineDbm = estimateNoiseFloorDbm(baselineBins);
    }
    if (baselineDbm <= -500.0f && m_noiseFloorBaselineValid) {
        baselineDbm = m_noiseFloorBaselineDbm;
    }
    if (baselineDbm <= -500.0f && m_measuredNoiseFloorDbm > -500.0f) {
        baselineDbm = m_measuredNoiseFloorDbm;
    }
    if (baselineDbm <= -500.0f) {
        qDebug() << "SpectrumWidget: noise-floor capture skipped — "
                    "no baseline (bins empty, cached invalid, measured invalid)";
        return false;
    }

    const float targetFrac = std::clamp((m_refLevel - baselineDbm) / m_dynamicRange,
                                        0.02f,
                                        0.98f);
    const int newPosition = std::clamp(
        static_cast<int>(std::lround(targetFrac * 100.0f)),
        1,
        99);

    m_noiseFloorTargetFrac = targetFrac;
    m_noiseFloorPosition = newPosition;
    if (persist) {
        auto& s = AppSettings::instance();
        s.setValue(settingsKey("DisplayNoiseFloorPosition"), QString::number(m_noiseFloorPosition));
        s.save();
    }
    if (notify) {
        emit noiseFloorPositionResolved(newPosition);
    }
    return true;
}

bool SpectrumWidget::updateNoiseFloorBaseline(const QVector<float>& bins, bool forceBaseline)
{
    if (!m_noiseFloorEnable || m_transmitting || bins.isEmpty()) return false;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_pendingDbmRangeEcho
        && m_pendingDbmRangeEchoStartMs > 0
        && nowMs - m_pendingDbmRangeEchoStartMs > kDbmRangeHandshakeTimeoutMs) {
        m_pendingDbmRangeEcho = false;
        m_pendingDbmRangeEchoFromAutoFloor = false;
        m_pendingDbmRangeEchoStartMs = 0;
    }
    if (noiseFloorAutoAdjustHeld(nowMs)) {
        if (m_pendingDbmRangeEchoFromAutoFloor) {
            m_pendingDbmRangeEcho = false;
            m_pendingDbmRangeEchoFromAutoFloor = false;
            m_pendingDbmRangeEchoStartMs = 0;
        }
        return false;
    }
    if (m_pendingDbmRangeEcho) {
        if (m_pendingDbmRangeEchoFromAutoFloor
            && m_noiseFloorBaselineValid
            && m_noiseFloorTargetValid
            && !isDraggingDbmScale()) {
            applyNoiseFloorAutoAdjust(nowMs);
        }
        return false;
    }

    const bool fastLockFrame = forceBaseline && m_noiseFloorFastLockFrames > 0;
    if (m_noiseFloorScaleSettlingUntilMs > nowMs && !fastLockFrame) {
        return false;
    }
    if (m_noiseFloorScaleSettlingUntilMs > 0) {
        m_noiseFloorScaleSettlingUntilMs = 0;
        m_noiseFloorCandidateValid = false;
        m_noiseFloorCandidateFrames = 0;
    }

    const float frameFloor = estimateNoiseFloorDbm(bins);
    if (frameFloor <= -500.0f || m_dynamicRange <= 0.0f) return false;

    if (!m_noiseFloorBaselineValid || m_noiseFloorLastSampleMs <= 0 || forceBaseline) {
        // Cold-acquire: force the baseline to this frame's reading.
        m_noiseFloorBaselineDbm = frameFloor;
        m_noiseFloorBaselineValid = true;
        m_noiseFloorLastSampleMs = nowMs;
        m_noiseFloorCandidateValid = false;
        m_noiseFloorCandidateFrames = 0;
    } else {
        // Asymmetric smoothing with candidate-state transient rejection.
        // Large upward shifts must persist 16 frames / 1.2 s before
        // being adopted (defeats lightning crashes); downward shifts
        // adopt in 2 frames / 70 ms.
        const float baselineDelta = frameFloor - m_noiseFloorBaselineDbm;
        const float baselineDeltaAbs = std::abs(baselineDelta);
        constexpr float kTransientShiftDb = 4.0f;
        if (baselineDeltaAbs > kTransientShiftDb) {
            const bool sameCandidate =
                m_noiseFloorCandidateValid
                && ((frameFloor - m_noiseFloorCandidateDbm) * baselineDelta >= 0.0f
                    || std::abs(frameFloor - m_noiseFloorCandidateDbm) < kTransientShiftDb);
            if (!sameCandidate) {
                m_noiseFloorCandidateValid = true;
                m_noiseFloorCandidateDbm = frameFloor;
                m_noiseFloorCandidateStartMs = nowMs;
                m_noiseFloorCandidateFrames = 1;
                m_noiseFloorLastSampleMs = nowMs;
                return false;
            }
            m_noiseFloorCandidateDbm =
                0.65f * m_noiseFloorCandidateDbm + 0.35f * frameFloor;
            ++m_noiseFloorCandidateFrames;

            const qint64 candidateAgeMs = nowMs - m_noiseFloorCandidateStartMs;
            const bool upward = baselineDelta > 0.0f;
            const int requiredFrames = upward ? 16 : 2;
            const qint64 requiredAgeMs = upward ? 1200 : 70;
            const bool keepWaiting = upward
                ? (m_noiseFloorCandidateFrames < requiredFrames
                   || candidateAgeMs < requiredAgeMs)
                : (m_noiseFloorCandidateFrames < requiredFrames
                   && candidateAgeMs < requiredAgeMs);
            if (keepWaiting) {
                m_noiseFloorLastSampleMs = nowMs;
                return false;
            }
        } else {
            m_noiseFloorCandidateValid = false;
            m_noiseFloorCandidateFrames = 0;
        }

        const float elapsedSec = std::clamp(
            static_cast<float>(nowMs - m_noiseFloorLastSampleMs) / 1000.0f,
            0.001f, 1.0f);
        float tauSec = 2.5f;
        if (baselineDeltaAbs > 20.0f)      tauSec = 0.08f;
        else if (baselineDeltaAbs > 10.0f) tauSec = 0.15f;
        else if (baselineDeltaAbs > 5.0f)  tauSec = 0.35f;
        else if (baselineDeltaAbs < 0.60f) tauSec = 8.0f;
        if (baselineDelta < -1.0f)       tauSec = std::min(tauSec, 0.12f);
        else if (baselineDelta > 1.0f)   tauSec = std::max(tauSec, 1.2f);

        const float alpha = 1.0f - std::exp(-elapsedSec / tauSec);
        m_noiseFloorBaselineDbm =
            (1.0f - alpha) * m_noiseFloorBaselineDbm + alpha * frameFloor;
        m_noiseFloorLastSampleMs = nowMs;
        if (baselineDeltaAbs > kTransientShiftDb) {
            m_noiseFloorCandidateValid = false;
            m_noiseFloorCandidateFrames = 0;
        }
    }

    if (!m_noiseFloorTargetValid) {
        refreshNoiseFloorTarget();
        if (!m_noiseFloorTargetValid) return true;
    }

    if (isDraggingDbmScale() || m_pendingDbmRangeEcho) return true;

    applyNoiseFloorAutoAdjust(nowMs);
    return true;
}

void SpectrumWidget::applyNoiseFloorAutoAdjust(qint64 nowMs)
{
    if (noiseFloorAutoAdjustHeld(nowMs)) {
        return;
    }

    // Pan: keep span fixed, slide refLevel so the smoothed baseline
    // sits at the user-chosen fraction.  (The earlier zoom-based
    // approach changed span every time the floor moved, which made
    // signal heights jump visually.)
    const float desiredRef = m_noiseFloorBaselineDbm
        + m_noiseFloorTargetFrac * m_dynamicRange;
    const float clampedRef = std::max(desiredRef, kMinDisplayDbm + m_dynamicRange);
    if (std::abs(clampedRef - m_refLevel) < 0.45f) {
        if (m_noiseFloorFastLockFrames > 0) {
            --m_noiseFloorFastLockFrames;
        }
        return;
    }

    if (m_noiseFloorFastLockFrames > 0) {
        --m_noiseFloorFastLockFrames;
        m_noiseFloorLastMotionMs = nowMs;
        m_refLevel = clampedRef;
        markOverlayDirty();
        sendNoiseFloorRangeCommand(nowMs, true);
        return;
    }

    moveRefLevelToward(clampedRef, nowMs);
}

bool SpectrumWidget::noiseFloorAutoAdjustHeld(qint64 nowMs)
{
    if (m_noiseFloorAutoAdjustHoldUntilMs > nowMs) {
        return true;
    }
    if (m_noiseFloorAutoAdjustHoldUntilMs > 0) {
        m_noiseFloorAutoAdjustHoldUntilMs = 0;
        resetNoiseFloorBaseline();
        armNoiseFloorFastLock(8, 1);
        return true;
    }
    return false;
}

void SpectrumWidget::armNoiseFloorFastLock(int freshFrames, int snapFrames)
{
    if (!m_noiseFloorEnable) {
        return;
    }

    m_noiseFloorFreshFrameCount = std::max(m_noiseFloorFreshFrameCount, freshFrames);
    m_noiseFloorFastLockFrames = std::max(m_noiseFloorFastLockFrames, snapFrames);
}

void SpectrumWidget::moveRefLevelToward(float targetRef, qint64 nowMs)
{
    if (m_noiseFloorLastMotionMs <= 0) m_noiseFloorLastMotionMs = nowMs;

    const float delta = targetRef - m_refLevel;
    const float deltaAbs = std::abs(delta);
    if (deltaAbs < 0.45f) {
        m_refLevel = targetRef;
        sendNoiseFloorRangeCommand(nowMs, true);
        return;
    }

    const float elapsedSec = std::clamp(
        static_cast<float>(nowMs - m_noiseFloorLastMotionMs) / 1000.0f,
        0.001f, 0.1f);
    m_noiseFloorLastMotionMs = nowMs;

    float tauSec = 0.24f;
    if (deltaAbs > 30.0f)      tauSec = 0.12f;
    else if (deltaAbs > 15.0f) tauSec = 0.16f;
    if (delta < 0.0f) {
        tauSec = 0.10f;
        if (deltaAbs > 30.0f)      tauSec = 0.06f;
        else if (deltaAbs > 15.0f) tauSec = 0.08f;
    }

    const float alpha = 1.0f - std::exp(-elapsedSec / tauSec);
    m_refLevel += delta * alpha;
    if (std::abs(targetRef - m_refLevel) < 0.12f) m_refLevel = targetRef;

    markOverlayDirty();
    sendNoiseFloorRangeCommand(nowMs, m_refLevel == targetRef);
}

void SpectrumWidget::sendNoiseFloorRangeCommand(qint64 nowMs, bool force)
{
    if (!m_noiseFloorEnable || m_dynamicRange <= 0.0f) return;
    if (noiseFloorAutoAdjustHeld(nowMs)) return;

    constexpr qint64 kCommandIntervalMs = 150;
    constexpr float kCommandThresholdDb = 0.75f;
    if (!force) {
        if (m_noiseFloorLastCommandMs > 0
            && nowMs - m_noiseFloorLastCommandMs < kCommandIntervalMs) return;
        if (m_noiseFloorLastCommandRef > -500.0f
            && std::abs(m_refLevel - m_noiseFloorLastCommandRef) < kCommandThresholdDb) return;
    } else if (m_noiseFloorLastCommandRef > -500.0f
               && std::abs(m_refLevel - m_noiseFloorLastCommandRef) < 0.05f) {
        return;
    }

    m_noiseFloorLastCommandMs = nowMs;
    m_noiseFloorLastCommandRef = m_refLevel;
    m_pendingDbmRangeEcho = true;
    m_pendingDbmRangeEchoFromAutoFloor = true;
    m_pendingDbmRangeEchoStartMs = nowMs;
    m_pendingMinDbm = m_refLevel - m_dynamicRange;
    m_pendingMaxDbm = m_refLevel;
    emit dbmRangeChangeRequested(m_refLevel - m_dynamicRange, m_refLevel);
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
    update();
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
void SpectrumWidget::setRadioAutoBlackLevel(quint32 rawLevel) {
    const float v = static_cast<float>(rawLevel);
    if (v == m_radioAutoBlackRaw) return;
    m_radioAutoBlackRaw = v;
    update();
}
void SpectrumWidget::setWfAutoBlackRadioSide(bool radioSide) {
    if (radioSide == m_wfAutoBlackRadioSide) {
        return;
    }
    m_wfAutoBlackRadioSide = radioSide;
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayWfAutoBlackRadioSide"), radioSide ? "True" : "False");
    s.save();
    // Drop any stale radio level when switching back to client-side so the
    // client estimate takes over immediately; a fresh tile repopulates it.
    if (!radioSide) {
        m_radioAutoBlackRaw = 0.0f;
    }
    update();
}
void SpectrumWidget::setWfLineDuration(int ms) {
    const int clamped = std::clamp(ms, kWaterfallLineDurationMinMs, kWaterfallLineDurationMaxMs);
    if (m_wfLineDuration == clamped) {
        if (m_overlayMenu) {
            m_overlayMenu->syncWfLineDuration(m_wfLineDuration);
        }
        return;
    }

    m_wfLineDuration = clamped;
    PerfTelemetry::instance().setWaterfallLineDurationMs(m_wfLineDuration);
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayWfLineDuration"), QString::number(m_wfLineDuration));
    s.save();
    if (m_overlayMenu) {
        m_overlayMenu->syncWfLineDuration(m_wfLineDuration);
    }
    // Re-calibrate the time scale for the new rate
    resetWfTimeScale();
    markOverlayDirty();
}

void SpectrumWidget::setSquelchLine(bool visible, int level)
{
    m_squelchLineVisible = visible;
    m_squelchLevel       = level;
    markOverlayDirty();
    // Manual SQL: 3 s auto-hide; each slider adjustment restarts the timer.
    // Auto SQL: line stays pinned to the tracked floor level — no timer.
    if (m_squelchLineHideTimer) {
        if (visible && !m_autoSquelchEnabled) {
            m_squelchLineHideTimer->start();
        } else {
            m_squelchLineHideTimer->stop();
        }
    }
}

void SpectrumWidget::drawAutoSqlFloor(QPainter& p, const QRect& specRect)
{
    if (!m_autoSquelchEnabled || m_sqlNoiseFloorDbm <= -500.0f) { return; }
    const float norm = (m_refLevel - m_sqlNoiseFloorDbm) / m_dynamicRange;
    const int y = specRect.top()
        + static_cast<int>(std::clamp(norm, 0.0f, 1.0f) * specRect.height());
    p.setPen(QPen(AetherSDR::theme::withAlpha("color.accent.warning", 200), 1, Qt::DashLine));
    p.drawLine(specRect.left(), y, specRect.right(), y);
    QFont f = p.font();
    f.setPointSize(8);
    f.setBold(true);
    p.setFont(f);
    p.setPen(AetherSDR::theme::withAlpha("color.accent.warning", 200));
    const QString lbl = QString("Floor %1 dBm").arg(static_cast<int>(m_sqlNoiseFloorDbm));
    p.drawText(specRect.right() - p.fontMetrics().horizontalAdvance(lbl) - 4, y - 2, lbl);
}

void SpectrumWidget::setAutoSquelchEnable(bool on)
{
    m_autoSquelchEnabled   = on;
    m_sqlNoiseFloorDbm     = -999.0f;  // cold-start the floor EWMA on each enable
    m_lastAutoSquelchLevel = -1;
    if (on && m_squelchLineHideTimer) {
        // Auto pins the line — cancel any pending manual auto-hide.
        m_squelchLineHideTimer->stop();
    }
}

void SpectrumWidget::setAutoSqlMarginDb(int dBm)
{
    m_autoSqlMarginDb      = std::clamp(dBm, 5, 20);
    m_lastAutoSquelchLevel = -1;  // force re-emit with new margin
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
    // Seed the visible scale from the deterministic rate mapping so Ctrl-drag
    // previews move with the mouse.  Real row timestamps update the per-rate
    // cache after enough new rows arrive at this rate.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const int previousFallbackIntervalMs = waterfallFallbackIntervalMs();
    const int lineDuration = std::clamp(m_wfLineDuration,
                                        kWaterfallLineDurationMinMs,
                                        kWaterfallLineDurationMaxMs);
    const float previewMsPerRow = lineDurationToVisualMsPerRow(lineDuration);
    const auto measuredIt = m_wfMeasuredMsPerRowByLineDuration.constFind(lineDuration);
    const bool hasMeasured = measuredIt != m_wfMeasuredMsPerRowByLineDuration.constEnd();
    float estimatedMsPerRow = hasMeasured ? *measuredIt : previewMsPerRow;
    if (!hasMeasured) {
        int lowerRate = kWaterfallLineDurationMinMs - 1;
        int upperRate = kWaterfallLineDurationMaxMs + 1;
        float lowerMsPerRow = 0.0f;
        float upperMsPerRow = 0.0f;
        for (auto it = m_wfMeasuredMsPerRowByLineDuration.cbegin();
             it != m_wfMeasuredMsPerRowByLineDuration.cend(); ++it) {
            const int rate = it.key();
            if (rate < lineDuration && rate > lowerRate) {
                lowerRate = rate;
                lowerMsPerRow = it.value();
            }
            if (rate > lineDuration && rate < upperRate) {
                upperRate = rate;
                upperMsPerRow = it.value();
            }
        }
        if (lowerRate >= kWaterfallLineDurationMinMs
            && upperRate <= kWaterfallLineDurationMaxMs) {
            const float fraction = static_cast<float>(lineDuration - lowerRate)
                / static_cast<float>(upperRate - lowerRate);
            estimatedMsPerRow =
                lowerMsPerRow + (upperMsPerRow - lowerMsPerRow) * fraction;
        }
    }
    m_wfMsPerRow = estimatedMsPerRow;
    m_wfPrevTimecode = 0;
    m_wfPrevTimecodeMs = 0;
    m_wfCalibrationCount = 0;
    m_wfTimeScaleLocked = false;
    m_wfRowsSinceRateChange = 0;
    m_wfCalibrationResumeMs = nowMs + 500;
    m_nextFallbackWaterfallRowMs = 0;
    if (m_hasNativeWaterfall && m_lastNativeTileMs > 0) {
        const int graceMs = nativeWaterfallFallbackHoldMs(previousFallbackIntervalMs);
        m_nativeWaterfallFallbackHoldUntilMs =
            std::max(m_nativeWaterfallFallbackHoldUntilMs, nowMs + graceMs);
        m_waterfallFallbackActive = false;
    } else if (m_lastNativeTileMs <= 0) {
        const int graceMs = nativeWaterfallFallbackHoldMs(waterfallFallbackIntervalMs());
        m_nativeWaterfallFallbackHoldUntilMs =
            std::max(m_nativeWaterfallFallbackHoldUntilMs, nowMs + graceMs);
        m_waterfallFallbackActive = false;
    } else {
        m_nativeWaterfallFallbackHoldUntilMs = 0;
    }
    ensureWaterfallHistory();
}

int SpectrumWidget::waterfallHistoryCapacityRows() const
{
    // Keep history capacity bounded; radio line_duration accepts 1 ms, but
    // allocating 20 minutes at that cadence would be disproportionate.
    const int msPerRow = kWaterfallHistoryCapacityMsPerRow;
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

void SpectrumWidget::updateWaterfallMsPerRowFromHistory()
{
    if (m_waterfallHistory.isNull() || m_wfHistoryTimestamps.isEmpty()
        || m_wfHistoryRowCount < 2) {
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() < m_wfCalibrationResumeMs) {
        return;
    }

    constexpr int kSampleRows = 24;
    constexpr int kMinSampleRows = 8;
    constexpr int kSamplesBeforeVisibleUpdate = 3;
    const int maxAgeRows = std::min({kSampleRows,
                                     m_wfHistoryRowCount - 1,
                                     m_wfRowsSinceRateChange - 1});
    if (maxAgeRows < kMinSampleRows) {
        return;
    }

    const int newestIndex = historyRowIndexForAge(0);
    if (newestIndex < 0 || newestIndex >= m_wfHistoryTimestamps.size()) {
        return;
    }

    const qint64 newestMs = m_wfHistoryTimestamps[newestIndex];
    if (newestMs <= 0) {
        return;
    }

    for (int ageRows = maxAgeRows; ageRows > 0; --ageRows) {
        const int olderIndex = historyRowIndexForAge(ageRows);
        if (olderIndex < 0 || olderIndex >= m_wfHistoryTimestamps.size()) {
            continue;
        }

        const qint64 olderMs = m_wfHistoryTimestamps[olderIndex];
        if (olderMs <= 0 || newestMs <= olderMs) {
            continue;
        }

        const float measured = static_cast<float>(newestMs - olderMs)
            / static_cast<float>(ageRows);
        const int lineDuration = std::clamp(m_wfLineDuration,
                                            kWaterfallLineDurationMinMs,
                                            kWaterfallLineDurationMaxMs);
        const int previousSamples =
            m_wfMeasuredSampleCountByLineDuration.value(lineDuration, 0);
        const float previousMeasured =
            m_wfMeasuredMsPerRowByLineDuration.value(lineDuration, measured);
        const float updatedMeasured = previousSamples > 0
            ? (0.85f * previousMeasured + 0.15f * measured)
            : measured;
        const int updatedSamples = std::min(previousSamples + 1, 1000);

        m_wfMeasuredMsPerRowByLineDuration.insert(lineDuration, updatedMeasured);
        m_wfMeasuredSampleCountByLineDuration.insert(lineDuration, updatedSamples);
        m_wfHasMeasuredMsPerRow = true;
        m_wfLastMeasuredLineDurationMs = lineDuration;
        m_wfLastMeasuredMsPerRow = updatedMeasured;
        if (!m_wfTimeScaleLocked && updatedSamples >= kSamplesBeforeVisibleUpdate) {
            m_wfMsPerRow = updatedMeasured;
            m_wfTimeScaleLocked = true;
            markOverlayDirty();
        }
        return;
    }
}

int SpectrumWidget::waterfallFallbackIntervalMs() const
{
    return std::clamp(static_cast<int>(std::lround(std::max(1.0f, m_wfMsPerRow))),
                      kWaterfallLineDurationMinMs,
                      kNativeWaterfallFallbackMaxTimeoutMs);
}

int SpectrumWidget::waterfallFallbackTimeoutMs() const
{
    const float intervalMs = static_cast<float>(waterfallFallbackIntervalMs());
    return std::clamp(static_cast<int>(std::ceil(intervalMs * 2.5f + 500.0f)),
                      kNativeWaterfallFallbackMinTimeoutMs,
                      kNativeWaterfallFallbackMaxTimeoutMs);
}

int SpectrumWidget::nativeWaterfallFallbackHoldMs(int intervalMs) const
{
    const float clampedIntervalMs = static_cast<float>(
        std::clamp(intervalMs, kWaterfallLineDurationMinMs, kNativeWaterfallFallbackMaxTimeoutMs));
    return std::clamp(static_cast<int>(std::ceil(clampedIntervalMs * 2.0f + 1000.0f)),
                      kNativeWaterfallFallbackMinTimeoutMs,
                      kNativeWaterfallRateChangeGraceMaxMs);
}

void SpectrumWidget::updateNativeWaterfallFallbackState(qint64 nowMs)
{
    if (m_hasNativeWaterfall) {
        if (m_nativeWaterfallFallbackHoldUntilMs > nowMs) {
            m_waterfallFallbackActive = false;
            m_nextFallbackWaterfallRowMs = 0;
            return;
        }
        const bool stillFresh = m_lastNativeTileMs > 0
            && nowMs - m_lastNativeTileMs <= waterfallFallbackTimeoutMs();
        if (stillFresh) {
            m_waterfallFallbackActive = false;
            m_nextFallbackWaterfallRowMs = 0;
            m_nativeWaterfallFallbackHoldUntilMs = 0;
            return;
        }

        m_hasNativeWaterfall = false;
        m_waterfallFallbackActive = true;
        m_nextFallbackWaterfallRowMs = nowMs;
        return;
    }

    if (m_nativeWaterfallFallbackHoldUntilMs > nowMs) {
        m_waterfallFallbackActive = false;
        m_nextFallbackWaterfallRowMs = 0;
        return;
    }

    if (!m_waterfallFallbackActive) {
        m_waterfallFallbackActive = true;
        if (m_nextFallbackWaterfallRowMs <= 0) {
            m_nextFallbackWaterfallRowMs = nowMs;
        }
    }
}

bool SpectrumWidget::pushRxWaterfallFallbackIfDue(const QVector<float>& bins, qint64 nowMs)
{
    if (!m_waterfallFallbackActive || bins.isEmpty()) {
        return false;
    }
    if (m_nextFallbackWaterfallRowMs <= 0) {
        m_nextFallbackWaterfallRowMs = nowMs;
    }
    if (nowMs < m_nextFallbackWaterfallRowMs) {
        return false;
    }

    const bool visibleStream = beginWaterfallStreamWrite(false);
    auto restoreStream = qScopeGuard([&] {
        endWaterfallStreamWrite(false, visibleStream);
    });
    if (m_waterfall.isNull()) {
        return false;
    }
    pushWaterfallRow(bins, m_waterfall.width());
    m_nextFallbackWaterfallRowMs = nowMs + waterfallFallbackIntervalMs();
    return true;
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
        m_wfHistoryRowCenterMhz = QVector<double>(desiredSize.height(), 0.0);
        m_wfHistoryRowBwMhz = QVector<double>(desiredSize.height(), 0.0);
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
    if (PerfTelemetry::instance().enabled())
        PerfTelemetry::instance().recordWaterfallVisibleRows();
}

void SpectrumWidget::appendHistoryRow(const QRgb* rowData, qint64 timestampMs,
                                      double frameCenterMhz,
                                      double frameBandwidthMhz)
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
    // Stamp the frequency frame this row was captured in, so the viewport can
    // remap it later regardless of how the center/bandwidth has since panned.
    const double stampCenterMhz = (frameCenterMhz > 0.0 && frameBandwidthMhz > 0.0)
        ? frameCenterMhz
        : m_centerMhz;
    const double stampBandwidthMhz = frameBandwidthMhz > 0.0
        ? frameBandwidthMhz
        : m_bandwidthMhz;
    if (m_wfHistoryWriteRow >= 0 && m_wfHistoryWriteRow < m_wfHistoryRowCenterMhz.size()) {
        m_wfHistoryRowCenterMhz[m_wfHistoryWriteRow] = stampCenterMhz;
        m_wfHistoryRowBwMhz[m_wfHistoryWriteRow] = stampBandwidthMhz;
    }
    if (m_wfHistoryRowCount < h) {
        ++m_wfHistoryRowCount;
    }
    if (m_wfRowsSinceRateChange < h) {
        ++m_wfRowsSinceRateChange;
    }
    updateWaterfallMsPerRowFromHistory();
    if (!m_wfLive) {
        m_wfHistoryOffsetRows = std::min(m_wfHistoryOffsetRows + 1, maxWaterfallHistoryOffsetRows());
    }
}

// Copy one history scanline into the viewport, remapping its columns from the
// frame it was captured in (rowCenter/rowBw) to the current frame (curCenter/
// curBw). When the frames match (no pan/zoom since capture) this is a plain
// copy; otherwise it is a horizontal resample and newly-exposed columns are
// black. Kiwi rows can be narrower than the current viewport after a zoom-out,
// so its path preserves the brightest source pixel covered by each destination
// column instead of point-sampling carriers out of existence.
static int waterfallPixelScore(QRgb pixel)
{
    const int maxChannel = std::max(qRed(pixel), std::max(qGreen(pixel), qBlue(pixel)));
    return maxChannel * 1024 + qRed(pixel) + qGreen(pixel) + qBlue(pixel);
}

static QRgb peakPreservedWaterfallSample(const QRgb* src, int w,
                                         double srcLeft, double srcRight,
                                         double srcCenter)
{
    if (srcRight <= 0.0 || srcLeft >= static_cast<double>(w)) {
        return qRgb(0, 0, 0);
    }

    const double clampedLeft = std::clamp(srcLeft, 0.0, static_cast<double>(w));
    const double clampedRight = std::clamp(srcRight, 0.0, static_cast<double>(w));
    if (clampedRight - clampedLeft <= 1.0) {
        return (srcCenter < 0.0 || srcCenter >= static_cast<double>(w))
            ? qRgb(0, 0, 0)
            : src[static_cast<int>(srcCenter)];
    }

    const int first = std::clamp(static_cast<int>(std::floor(clampedLeft)), 0, w - 1);
    const int last = std::clamp(static_cast<int>(std::ceil(clampedRight)) - 1, 0, w - 1);
    QRgb best = src[first];
    int bestScore = waterfallPixelScore(best);
    for (int i = first + 1; i <= last; ++i) {
        const int score = waterfallPixelScore(src[i]);
        if (score > bestScore) {
            best = src[i];
            bestScore = score;
        }
    }
    return best;
}

static float interpolatedBinSample(const QVector<float>& bins,
                                   double srcCenter,
                                   float fallback)
{
    const int n = bins.size();
    if (n <= 0) {
        return fallback;
    }
    if (n == 1) {
        return std::isfinite(bins[0]) ? bins[0] : fallback;
    }

    const double clamped = std::clamp(srcCenter, 0.0, static_cast<double>(n - 1));
    const int left = std::clamp(static_cast<int>(std::floor(clamped)), 0, n - 1);
    const int right = std::min(left + 1, n - 1);
    const float leftValue = std::isfinite(bins[left]) ? bins[left] : fallback;
    const float rightValue = std::isfinite(bins[right]) ? bins[right] : leftValue;
    const float frac = static_cast<float>(clamped - static_cast<double>(left));
    return leftValue + frac * (rightValue - leftValue);
}

static float peakPreservedBinSample(const QVector<float>& bins,
                                    double srcLeft, double srcRight,
                                    double srcCenter, float fallback)
{
    const int n = bins.size();
    if (n <= 0 || srcRight <= 0.0 || srcLeft >= static_cast<double>(n)) {
        return fallback;
    }

    const double clampedLeft = std::clamp(srcLeft, 0.0, static_cast<double>(n));
    const double clampedRight = std::clamp(srcRight, 0.0, static_cast<double>(n));
    if (clampedRight - clampedLeft <= 1.0) {
        return interpolatedBinSample(bins, srcCenter, fallback);
    }

    const int first = std::clamp(static_cast<int>(std::floor(clampedLeft)), 0, n - 1);
    const int last = std::clamp(static_cast<int>(std::ceil(clampedRight)) - 1, 0, n - 1);
    float best = fallback;
    bool haveBest = false;
    for (int i = first; i <= last; ++i) {
        const float value = bins[i];
        if (!std::isfinite(value)) {
            continue;
        }
        if (!haveBest || value > best) {
            best = value;
            haveBest = true;
        }
    }
    return haveBest ? best : fallback;
}

static void remapHistoryRowInto(QRgb* dst, const QRgb* src, int w,
                                double rowCenterMhz, double rowBwMhz,
                                double curCenterMhz, double curBwMhz,
                                bool preservePeaks)
{
    if (rowBwMhz <= 0.0 || curBwMhz <= 0.0
        || (rowCenterMhz == curCenterMhz && rowBwMhz == curBwMhz)) {
        std::memcpy(dst, src, w * static_cast<int>(sizeof(QRgb)));
        return;
    }
    const double rowStartMhz = rowCenterMhz - rowBwMhz / 2.0;
    const double rowEndMhz = rowStartMhz + rowBwMhz;
    const double curStartMhz = curCenterMhz - curBwMhz / 2.0;
    for (int x = 0; x < w; ++x) {
        const double freqMhz = curStartMhz + (static_cast<double>(x) + 0.5) / w * curBwMhz;
        const double srcX = (freqMhz - rowStartMhz) / rowBwMhz * w;
        if (!preservePeaks) {
            dst[x] = (srcX < 0.0 || srcX >= w) ? qRgb(0, 0, 0)
                                               : src[static_cast<int>(srcX)];
            continue;
        }

        const double freqLeftMhz = curStartMhz
            + static_cast<double>(x) / w * curBwMhz;
        const double freqRightMhz = curStartMhz
            + static_cast<double>(x + 1) / w * curBwMhz;
        if (freqRightMhz <= rowStartMhz || freqLeftMhz >= rowEndMhz) {
            dst[x] = qRgb(0, 0, 0);
            continue;
        }
        const double srcLeft = (freqLeftMhz - rowStartMhz) / rowBwMhz * w;
        const double srcRight = (freqRightMhz - rowStartMhz) / rowBwMhz * w;
        dst[x] = peakPreservedWaterfallSample(src, w, srcLeft, srcRight, srcX);
    }
}

void SpectrumWidget::rebuildWaterfallViewport()
{
    rebuildWaterfallViewportForFrame(m_centerMhz, m_bandwidthMhz);
}

void SpectrumWidget::rebuildWaterfallViewportForFrame(double centerMhz,
                                                      double bandwidthMhz)
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

    const int w = m_waterfall.width();
    const bool haveFrames = m_wfHistoryRowCenterMhz.size() == m_waterfallHistory.height();
    for (int y = 0; y < m_waterfall.height(); ++y) {
        const int rowIndex = historyRowIndexForAge(m_wfHistoryOffsetRows + y);
        if (rowIndex < 0) {
            break;
        }
        const QRgb* src = reinterpret_cast<const QRgb*>(
            m_waterfallHistory.constScanLine(rowIndex));
        auto* dst = reinterpret_cast<QRgb*>(m_waterfall.scanLine(y));
        const double rowCenter = haveFrames ? m_wfHistoryRowCenterMhz[rowIndex] : m_centerMhz;
        const double rowBw = haveFrames ? m_wfHistoryRowBwMhz[rowIndex] : m_bandwidthMhz;
        remapHistoryRowInto(dst, src, w, rowCenter, rowBw, centerMhz, bandwidthMhz,
                            m_kiwiSdrWaterfallActive);
    }

#ifdef AETHER_GPU_SPECTRUM
    m_wfTexFullUpload = true;
#endif
    if (PerfTelemetry::instance().enabled())
        PerfTelemetry::instance().recordWaterfallRebuild();
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

void SpectrumWidget::handleWaterfallFrequencyFrameChange(double oldCenterMhz,
                                                         double oldBandwidthMhz,
                                                         double newCenterMhz,
                                                         double newBandwidthMhz)
{
    const bool originalKiwiActive = m_kiwiSdrWaterfallActive;

    auto reprojectStream = [this, oldCenterMhz, oldBandwidthMhz,
                            newCenterMhz, newBandwidthMhz](bool kiwiStream) {
        const bool visibleStream = beginWaterfallStreamWrite(kiwiStream);
        auto restoreStream = qScopeGuard([&] {
            endWaterfallStreamWrite(kiwiStream, visibleStream);
        });
        if (kiwiStream) {
            m_kiwiSdrLastWaterfallBins.clear();
        }
        reprojectWaterfall(oldCenterMhz, oldBandwidthMhz,
                           newCenterMhz, newBandwidthMhz);
    };

    // #3668 (KiwiSDR integration) replaced #3578's single per-pan reproject with
    // an unconditional native + kiwi double reproject. The inactive stream is not
    // on screen, so reprojecting it on every pan step is wasted work -- and the
    // stream-state save/restore around it leaves m_waterfall COW-shared, so the
    // next fill() in rebuildWaterfallViewportForFrame deep-copies the whole
    // (potentially very large) waterfall image on every pan step. On high-res
    // displays that cost 60-80 ms of UI-thread stall per pan step -- a visible
    // regression (measured: wfUpdateP95 0.1 ms -> 68 ms with the second pass).
    //
    // Reproject only the active stream. The inactive stream is remapped to the
    // current frequency frame when it next becomes visible (see
    // setKiwiSdrWaterfallActive); each history row carries its own capture frame
    // (#3578), so toggling streams stays seamless. AETHER_WF_KIWI_ALWAYS=1
    // restores the old unconditional double pass for A/B verification.
    static const bool kiwiAlways = qEnvironmentVariableIsSet("AETHER_WF_KIWI_ALWAYS");
    if (kiwiAlways) {
        reprojectStream(false);
        reprojectStream(true);
        if (m_kiwiSdrWaterfallActive != originalKiwiActive) {
            saveCurrentWaterfallStreamState();
            m_kiwiSdrWaterfallActive = originalKiwiActive;
            restoreCurrentWaterfallStreamState();
        }
    } else {
        reprojectStream(m_kiwiSdrWaterfallActive);
    }
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
    const int buttonY = wfRect.top() - freqScaleH() + 2;
    return QRect(buttonX, buttonY, buttonW, buttonH);
}

// ─────────────────────────────────────────────────────────────────────────────

void SpectrumWidget::clearDisplay()
{
    m_bins.clear();
    m_smoothed.clear();
    clearWaterfallRows();
    m_hasNativeWaterfall = false;
    m_lastNativeTileMs = 0;
    m_waterfallFallbackActive = false;
    m_nextFallbackWaterfallRowMs = 0;
    const int graceMs = nativeWaterfallFallbackHoldMs(waterfallFallbackIntervalMs());
    m_nativeWaterfallFallbackHoldUntilMs = QDateTime::currentMSecsSinceEpoch() + graceMs;
    markOverlayDirty();
}

void SpectrumWidget::clearCurrentWaterfallRows()
{
    if (!m_waterfall.isNull()) {
        m_waterfall.fill(Qt::black);
    }
    if (!m_waterfallHistory.isNull()) {
        m_waterfallHistory.fill(Qt::black);
    }
    std::fill(m_wfHistoryTimestamps.begin(), m_wfHistoryTimestamps.end(), 0);
    std::fill(m_wfHistoryRowCenterMhz.begin(), m_wfHistoryRowCenterMhz.end(), 0.0);
    std::fill(m_wfHistoryRowBwMhz.begin(), m_wfHistoryRowBwMhz.end(), 0.0);
    m_wfWriteRow = 0;
    m_wfHistoryWriteRow = 0;
    m_wfHistoryRowCount = 0;
    m_wfHistoryOffsetRows = 0;
    m_wfLive = true;
    m_wfRowsSinceRateChange = 0;
    m_prevTileScanline.clear();
    m_kiwiSdrFftTrace.clear();
    m_kiwiSdrLastWaterfallBins.clear();
    m_kiwiSdrLastWaterfallCenterMhz = 0.0;
    m_kiwiSdrLastWaterfallBandwidthMhz = 0.0;
    m_kiwiSdrLastWaterfallFrameValid = false;
    m_kiwiSdrAutoFloorDbm = kKiwiSdrWaterfallMinDbm;
    m_kiwiSdrAutoCeilDbm = kKiwiSdrWaterfallMaxDbm;
    m_kiwiSdrAutoRangeValid = false;
#ifdef AETHER_GPU_SPECTRUM
    m_wfTexFullUpload = true;
#endif
}

void SpectrumWidget::clearWaterfallRows()
{
    clearCurrentWaterfallRows();
    m_nativeWaterfallState = WaterfallStreamState{};
    m_kiwiWaterfallState = WaterfallStreamState{};
    m_kiwiProfileWaterfallStates.clear();
}

SpectrumWidget::WaterfallStreamState& SpectrumWidget::activeKiwiWaterfallState()
{
    if (!m_kiwiSdrWaterfallProfileId.isEmpty()) {
        return m_kiwiProfileWaterfallStates[m_kiwiSdrWaterfallProfileId];
    }
    return m_kiwiWaterfallState;
}

void SpectrumWidget::saveCurrentWaterfallStreamState()
{
    WaterfallStreamState& state = m_kiwiSdrWaterfallActive
        ? activeKiwiWaterfallState()
        : m_nativeWaterfallState;
    state.waterfall = m_waterfall;
    state.wfWriteRow = m_wfWriteRow;
    state.waterfallHistory = m_waterfallHistory;
    state.historyTimestamps = m_wfHistoryTimestamps;
    state.historyWriteRow = m_wfHistoryWriteRow;
    state.historyRowCount = m_wfHistoryRowCount;
    state.historyOffsetRows = m_wfHistoryOffsetRows;
    state.historyRowCenterMhz = m_wfHistoryRowCenterMhz;
    state.historyRowBwMhz = m_wfHistoryRowBwMhz;
    state.live = m_wfLive;
    state.rowsSinceRateChange = m_wfRowsSinceRateChange;
    state.prevTileScanline = m_prevTileScanline;
    state.kiwiFftTrace = m_kiwiSdrFftTrace;
    state.kiwiLastWaterfallBins = m_kiwiSdrLastWaterfallBins;
    state.kiwiLastWaterfallCenterMhz = m_kiwiSdrLastWaterfallCenterMhz;
    state.kiwiLastWaterfallBandwidthMhz = m_kiwiSdrLastWaterfallBandwidthMhz;
    state.kiwiLastWaterfallFrameValid = m_kiwiSdrLastWaterfallFrameValid;
    state.kiwiAutoFloorDbm = m_kiwiSdrAutoFloorDbm;
    state.kiwiAutoCeilDbm = m_kiwiSdrAutoCeilDbm;
    state.kiwiAutoRangeValid = m_kiwiSdrAutoRangeValid;
    state.valid = true;
}

void SpectrumWidget::restoreCurrentWaterfallStreamState()
{
    WaterfallStreamState& state = m_kiwiSdrWaterfallActive
        ? activeKiwiWaterfallState()
        : m_nativeWaterfallState;
    const QSize currentSize = m_waterfall.size();
    const QSize currentHistorySize = m_waterfallHistory.size();
    if (!state.valid
        || (!currentSize.isEmpty() && !state.waterfall.isNull()
            && state.waterfall.size() != currentSize)
        || (!currentHistorySize.isEmpty() && !state.waterfallHistory.isNull()
            && state.waterfallHistory.size() != currentHistorySize)) {
        clearCurrentWaterfallRows();
        return;
    }

    if (!state.waterfall.isNull()) {
        m_waterfall = state.waterfall;
    }
    m_wfWriteRow = state.wfWriteRow;
    if (!state.waterfallHistory.isNull()) {
        m_waterfallHistory = state.waterfallHistory;
    }
    m_wfHistoryTimestamps = state.historyTimestamps;
    m_wfHistoryWriteRow = state.historyWriteRow;
    m_wfHistoryRowCount = state.historyRowCount;
    m_wfHistoryOffsetRows = state.historyOffsetRows;
    m_wfHistoryRowCenterMhz = state.historyRowCenterMhz;
    m_wfHistoryRowBwMhz = state.historyRowBwMhz;
    m_wfLive = state.live;
    m_wfRowsSinceRateChange = state.rowsSinceRateChange;
    m_prevTileScanline = state.prevTileScanline;
    m_kiwiSdrFftTrace = state.kiwiFftTrace;
    m_kiwiSdrLastWaterfallBins = state.kiwiLastWaterfallBins;
    m_kiwiSdrLastWaterfallCenterMhz = state.kiwiLastWaterfallCenterMhz;
    m_kiwiSdrLastWaterfallBandwidthMhz = state.kiwiLastWaterfallBandwidthMhz;
    m_kiwiSdrLastWaterfallFrameValid = state.kiwiLastWaterfallFrameValid;
    m_kiwiSdrAutoFloorDbm = state.kiwiAutoFloorDbm;
    m_kiwiSdrAutoCeilDbm = state.kiwiAutoCeilDbm;
    m_kiwiSdrAutoRangeValid = state.kiwiAutoRangeValid;
#ifdef AETHER_GPU_SPECTRUM
    m_wfTexFullUpload = true;
#endif
}

bool SpectrumWidget::beginWaterfallStreamWrite(bool kiwiStream)
{
    const bool visibleStream = m_kiwiSdrWaterfallActive == kiwiStream;
    if (visibleStream) {
        return true;
    }

    saveCurrentWaterfallStreamState();
    m_kiwiSdrWaterfallActive = kiwiStream;
    restoreCurrentWaterfallStreamState();
    return false;
}

void SpectrumWidget::endWaterfallStreamWrite(bool kiwiStream,
                                             bool visibleStream)
{
    if (visibleStream) {
        return;
    }

    saveCurrentWaterfallStreamState();
    m_kiwiSdrWaterfallActive = !kiwiStream;
    restoreCurrentWaterfallStreamState();
}

QVector<float> SpectrumWidget::smoothKiwiSdrWaterfallBins(const QVector<float>& bins)
{
    if (bins.isEmpty()) {
        m_kiwiSdrLastWaterfallBins.clear();
        m_kiwiSdrLastWaterfallFrameValid = false;
        return {};
    }

    QVector<float> horizontal(bins.size());
    if (bins.size() == 1) {
        horizontal[0] = bins[0];
    } else {
        horizontal[0] = 0.75f * bins[0] + 0.25f * bins[1];
        for (int i = 1; i < bins.size() - 1; ++i) {
            horizontal[i] = 0.25f * bins[i - 1]
                + 0.50f * bins[i]
                + 0.25f * bins[i + 1];
        }
        horizontal[bins.size() - 1] =
            0.25f * bins[bins.size() - 2] + 0.75f * bins[bins.size() - 1];
    }

    if (m_kiwiSdrLastWaterfallBins.size() == horizontal.size()) {
        for (int i = 0; i < horizontal.size(); ++i) {
            horizontal[i] = 0.65f * horizontal[i]
                + 0.35f * m_kiwiSdrLastWaterfallBins[i];
        }
    }
    m_kiwiSdrLastWaterfallBins = horizontal;
    return horizontal;
}

void SpectrumWidget::updateKiwiSdrAutoColorRange(const QVector<float>& bins)
{
    const KiwiSdrProtocol::WaterfallAperture aperture =
        KiwiSdrProtocol::autoWaterfallAperture(bins);
    if (!aperture.valid) {
        return;
    }

    static constexpr float kMinSpanDb = 32.0f;
    static constexpr float kMaxSpanDb = 120.0f;
    static constexpr float kSmoothing = 0.10f;

    float candidateFloor = qBound(
        kKiwiSdrWaterfallMinDbm,
        aperture.minDbm,
        kKiwiSdrWaterfallMaxDbm - kMinSpanDb);
    float candidateCeil = qBound(
        candidateFloor + kMinSpanDb,
        aperture.maxDbm,
        kKiwiSdrWaterfallMaxDbm);

    float candidateSpan = candidateCeil - candidateFloor;
    if (candidateSpan > kMaxSpanDb) {
        candidateCeil = candidateFloor + kMaxSpanDb;
        candidateSpan = kMaxSpanDb;
    }
    if (candidateCeil > kKiwiSdrWaterfallMaxDbm) {
        candidateCeil = kKiwiSdrWaterfallMaxDbm;
    }
    if (candidateCeil - candidateFloor < kMinSpanDb) {
        candidateFloor = qMax(kKiwiSdrWaterfallMinDbm,
                              candidateCeil - kMinSpanDb);
    }

    if (!m_kiwiSdrAutoRangeValid) {
        m_kiwiSdrAutoFloorDbm = candidateFloor;
        m_kiwiSdrAutoCeilDbm = candidateCeil;
        m_kiwiSdrAutoRangeValid = true;
        return;
    }

    m_kiwiSdrAutoFloorDbm += kSmoothing
        * (candidateFloor - m_kiwiSdrAutoFloorDbm);
    m_kiwiSdrAutoCeilDbm += kSmoothing
        * (candidateCeil - m_kiwiSdrAutoCeilDbm);
    if (m_kiwiSdrAutoCeilDbm - m_kiwiSdrAutoFloorDbm < kMinSpanDb) {
        m_kiwiSdrAutoFloorDbm = qMax(kKiwiSdrWaterfallMinDbm,
                                     m_kiwiSdrAutoCeilDbm - kMinSpanDb);
    }
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

void SpectrumWidget::setKiwiSdrConnectionOverlay(bool visible,
                                                 const QString& detail,
                                                 const QString& title)
{
    const QString trimmedDetail = detail.trimmed();
    const QString trimmedTitle = title.trimmed();
    if (m_kiwiSdrConnectionOverlayVisible == visible
        && m_kiwiSdrConnectionOverlayTitle == trimmedTitle
        && m_kiwiSdrConnectionOverlayDetail == trimmedDetail) {
        return;
    }

    m_kiwiSdrConnectionOverlayVisible = visible;
    m_kiwiSdrConnectionOverlayTitle = trimmedTitle;
    m_kiwiSdrConnectionOverlayDetail = trimmedDetail;
    markOverlayDirty();
}

void SpectrumWidget::showInterlockNotification(const QString& message, int durationMs)
{
    const QString text = message.trimmed();
    if (text.isEmpty())
        return;

    const int availableWidth = qMax(80, width() - 24);
    const int maxTextWidth = qMax(80, qMin(availableWidth - 36, int(width() * 0.78)));
    QFont font = m_interlockNotificationLabel->font();
    font.setPointSize(13);
    font.setBold(true);
    m_interlockNotificationLabel->setFont(font);

    const QFontMetrics fm(font);
    const QRect textBounds = fm.boundingRect(
        QRect(0, 0, maxTextWidth, 1000),
        Qt::AlignCenter | Qt::TextWordWrap,
        text);

    m_interlockNotificationLabel->setText(text);
    m_interlockNotificationLabel->setFixedSize(
        qBound(80, textBounds.width() + 36, availableWidth),
        textBounds.height() + 24);
    positionInterlockNotification();
    m_interlockNotificationLabel->show();
    m_interlockNotificationLabel->raise();
    m_interlockNotificationTimer->start(qMax(1, durationMs));
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
    glow.setColorAt(0.0, withAlpha(kAetherBrandBlue(), 48));
    glow.setColorAt(0.55, withAlpha(kAetherBrandGreen(), 22));
    glow.setColorAt(1.0, AetherSDR::theme::withAlpha("color.background.spectrum", 0));
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
        QColor waveColor = (ring % 2 == 0) ? kAetherBrandBlue() : kAetherBrandGreen();
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
    towerGradient.setColorAt(0.0, kAetherBrandBlue().lighter(115));
    towerGradient.setColorAt(0.55, kAetherBrandBlue());
    towerGradient.setColorAt(1.0, kAetherBrandGreen());

    QPainterPath towerFill;
    towerFill.moveTo(centerX - towerWidth * 0.48, baseY);
    towerFill.lineTo(centerX, topY + towerHeight * 0.08);
    towerFill.lineTo(centerX + towerWidth * 0.48, baseY);
    towerFill.closeSubpath();
    QLinearGradient fillGradient(QPointF(centerX, topY), QPointF(centerX, baseY));
    fillGradient.setColorAt(0.0, withAlpha(kAetherBrandBlue(), 28));
    fillGradient.setColorAt(1.0, withAlpha(kAetherBrandGreen(), 12));
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
    p.setBrush(kAetherBrandBlue());
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
    titleGradient.setColorAt(0.0, kAetherBrandBlue().lighter(108));
    titleGradient.setColorAt(1.0, kAetherBrandGreen().lighter(105));
    p.setFont(titleFont);
    p.setPen(QPen(QBrush(titleGradient), 1.0));
    p.drawText(titleRect, Qt::AlignHCenter | Qt::AlignTop, m_connectionAnimationLabel);

    p.setFont(subtitleFont);
    p.setPen(withAlpha(kConnectionTextColor(), 180));
    p.drawText(subtitleRect, Qt::AlignHCenter | Qt::AlignTop, subtitle);

    p.restore();
}

void SpectrumWidget::drawKiwiSdrConnectionOverlay(QPainter& p,
                                                  const QRect& contentRect)
{
    if (!m_kiwiSdrConnectionOverlayVisible || contentRect.width() < 160
        || contentRect.height() < 80) {
        return;
    }

    const QString title = m_kiwiSdrConnectionOverlayTitle.isEmpty()
        ? QStringLiteral("Not connected to KiwiSDR")
        : m_kiwiSdrConnectionOverlayTitle;
    const QString detail = m_kiwiSdrConnectionOverlayDetail.isEmpty()
        ? QStringLiteral("Disconnected")
        : m_kiwiSdrConnectionOverlayDetail;

    QFont titleFont = p.font();
    titleFont.setPointSize(15);
    titleFont.setBold(true);
    QFont detailFont = titleFont;
    detailFont.setPointSize(10);
    detailFont.setBold(false);

    const QFontMetrics titleFm(titleFont);
    const QFontMetrics detailFm(detailFont);
    const int maxTextWidth =
        qMax(120, qMin(contentRect.width() - 48, 460));
    const QRect titleBounds = titleFm.boundingRect(
        QRect(0, 0, maxTextWidth, 200),
        Qt::AlignCenter | Qt::TextWordWrap,
        title);
    const QRect detailBounds = detailFm.boundingRect(
        QRect(0, 0, maxTextWidth, 240),
        Qt::AlignCenter | Qt::TextWordWrap,
        detail);
    const int boxW =
        qBound(180, qMax(titleBounds.width(), detailBounds.width()) + 40,
               qMax(180, contentRect.width() - 32));
    const int boxH = titleBounds.height() + detailBounds.height() + 34;
    const QRect box(contentRect.center().x() - boxW / 2,
                    contentRect.center().y() - boxH / 2,
                    boxW, boxH);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(AetherSDR::theme::withAlpha("color.accent.warning", 210), 1));
    p.setBrush(AetherSDR::theme::withAlpha("color.background.0", 220));
    p.drawRoundedRect(box, 6, 6);

    p.setFont(titleFont);
    p.setPen(AetherSDR::ThemeManager::instance().color("color.text.primary"));
    QRect textRect = box.adjusted(16, 10, -16, -10);
    const QRect titleRect(textRect.left(), textRect.top(),
                          textRect.width(), titleBounds.height() + 4);
    p.drawText(titleRect, Qt::AlignCenter | Qt::TextWordWrap, title);

    p.setFont(detailFont);
    p.setPen(AetherSDR::ThemeManager::instance().color("color.text.secondary"));
    const QRect detailRect(textRect.left(), titleRect.bottom() + 4,
                           textRect.width(), detailBounds.height() + 6);
    p.drawText(detailRect, Qt::AlignCenter | Qt::TextWordWrap, detail);
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
    if (!m_shutdownPrepared) {
        update();
    }
}

// Horizontally reproject a waterfall image from one frequency frame
// (oldCenter/oldBw) to another (newCenter/newBw). Overlapping spectrum is
// remapped to its new pixel columns; newly-exposed columns become black.
// Shared by the live waterfall (per pan step) and the deferred history flush.
static void reprojectWaterfallImage(QImage& image,
                                    double oldCenterMhz, double oldBandwidthMhz,
                                    double newCenterMhz, double newBandwidthMhz)
{
    if (image.isNull() || oldBandwidthMhz <= 0.0 || newBandwidthMhz <= 0.0) {
        return;
    }

    const int imageWidth = image.width();
    const int imageHeight = image.height();
    if (imageWidth <= 0 || imageHeight <= 0) {
        return;
    }

    const double oldStartMhz = oldCenterMhz - oldBandwidthMhz / 2.0;
    const double newStartMhz = newCenterMhz - newBandwidthMhz / 2.0;
    const double overlapStartMhz = std::max(oldStartMhz, newCenterMhz - newBandwidthMhz / 2.0);
    const double overlapEndMhz = std::min(oldCenterMhz + oldBandwidthMhz / 2.0,
                                          newCenterMhz + newBandwidthMhz / 2.0);

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
}

void SpectrumWidget::reprojectWaterfall(double oldCenterMhz, double oldBandwidthMhz,
                                        double newCenterMhz, double newBandwidthMhz)
{
    if (oldBandwidthMhz <= 0.0 || newBandwidthMhz <= 0.0) {
        return;
    }

    // Prefer the per-row history. Rows can be captured across several pan/zoom
    // frames, especially when switching between native and Kiwi waterfall data;
    // rebuilding from stamped history preserves each row's own frequency frame.
    if (!m_waterfallHistory.isNull() && m_wfHistoryRowCount > 0) {
        rebuildWaterfallViewportForFrame(newCenterMhz, newBandwidthMhz);
    } else {
        reprojectWaterfallImage(m_waterfall, oldCenterMhz, oldBandwidthMhz,
                                newCenterMhz, newBandwidthMhz);
    }
    m_prevTileScanline.clear();
#ifdef AETHER_GPU_SPECTRUM
    m_wfTexFullUpload = true;
#endif

    // History image is intentionally NOT reprojected here: each row carries its
    // own capture frame (m_wfHistoryRowCenterMhz/BwMhz) and is remapped on
    // demand into the current visible viewport.
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
    const bool panAnimationRunning = m_panCenterAnim &&
        m_panCenterAnim->state() != QAbstractAnimation::Stopped;
    const double waterfallFrameCenterMhz =
        panAnimationRunning ? m_panCenterTarget : oldCenterMhz;

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
            handleWaterfallFrequencyFrameChange(waterfallFrameCenterMhz,
                                                oldBandwidthMhz,
                                                centerMhz,
                                                bandwidthMhz);
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
        resetNoiseFloorBaseline();
        markOverlayDirty();
        emit frequencyRangeChanged(m_centerMhz, m_bandwidthMhz);
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

    const bool animAlreadyRunning = m_panCenterAnim &&
        m_panCenterAnim->state() != QAbstractAnimation::Stopped;
    const double waterfallSourceCenterMhz =
        animAlreadyRunning ? m_panCenterTarget : m_centerMhz;

    if (!animAlreadyRunning) {
        // Record the start position so the stale-echo guard above can
        // recognise echo-backs that refer to the pre-animation center.
        m_panCenterStart = m_centerMhz;
    }

    // Scroll waterfall history to align with the new center before the visual
    // center animation lands. During rapid edge-follow retargets the waterfall
    // image is already in the previous target's coordinate frame, so reproject
    // from m_panCenterTarget rather than the mid-animation visual center.
    handleWaterfallFrequencyFrameChange(waterfallSourceCenterMhz,
                                        m_bandwidthMhz,
                                        centerMhz,
                                        m_bandwidthMhz);

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
    emit frequencyRangeChanged(centerMhz, m_bandwidthMhz);
}

void SpectrumWidget::setSpectrumFrac(float f)
{
    m_spectrumFrac = std::clamp(f, 0.10f, 0.90f);
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("SpectrumSplitRatio"), QString::number(m_spectrumFrac, 'f', 3));
    s.save();
    positionFpsMeterLabels();
    if (width() >= 100 && spectrumPixelHeight() >= 20) {
        emit dimensionsChanged(width(), spectrumPixelHeight());
    }
    markOverlayDirty();
}

void SpectrumWidget::setDbmRange(float minDbm, float maxDbm)
{
    if (m_transmitting && txWaterfallAffectsThisPan()) {
        if (!m_txDbmRangeFrozen)
            beginTxDbmRangeFreeze();
        if (m_txDbmRangeFrozen) {
            deferTxDbmRange(minDbm, maxDbm);
            return;
        }
    }

    if (m_pendingDbmRangeEcho) {
        const bool matchesPending = std::abs(minDbm - m_pendingMinDbm) < 0.01f
            && std::abs(maxDbm - m_pendingMaxDbm) < 0.01f;
        const bool pendingFromAutoFloor = m_pendingDbmRangeEchoFromAutoFloor;
        const bool autoFloorMovedPastEcho = pendingFromAutoFloor
            && !isDraggingDbmScale()
            && (std::abs(minDbm - (m_refLevel - m_dynamicRange))
                    > kDbmReleasePreviewChangeThresholdDb
                || std::abs(maxDbm - m_refLevel)
                    > kDbmReleasePreviewChangeThresholdDb);
        if (!matchesPending) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (m_pendingDbmRangeEchoStartMs <= 0
                || nowMs - m_pendingDbmRangeEchoStartMs <= kDbmRangeHandshakeTimeoutMs) {
                return;
            }
        }
        m_pendingDbmRangeEcho = false;
        m_pendingDbmRangeEchoFromAutoFloor = false;
        m_pendingDbmRangeEchoStartMs = 0;
        if (matchesPending && autoFloorMovedPastEcho) {
            clearDbmReleaseRebase();
            sendNoiseFloorRangeCommand(QDateTime::currentMSecsSinceEpoch(), true);
            return;
        }
    }

    applyDbmRangeImmediate(minDbm, maxDbm);
}

void SpectrumWidget::applyDbmRangeImmediate(float minDbm, float maxDbm)
{
    const float clampedMinDbm = std::max(minDbm, kMinDisplayDbm);
    float ref = maxDbm;
    float dyn = std::max(10.0f, maxDbm - clampedMinDbm);
    if (ref == m_refLevel && dyn == m_dynamicRange) {
        clearDbmReleaseRebase();
        return;
    }
    clearDbmReleaseRebase();
    m_refLevel     = ref;
    m_dynamicRange = dyn;
    m_resetFftSmoothingOnNextFrame = true;
    resetNoiseFloorBaseline();
    if (m_noiseFloorEnable) {
        // Do not repaint the radio-owned range as an intermediate state when
        // client-side auto floor is about to place the trace on the next frame.
        armNoiseFloorFastLock(5, 1);
    } else {
        markOverlayDirty();
    }
}

// ─── Slice color table (shared via SliceColors.h) ────────────────────────────

static QColor sliceColor(int sliceId, bool active) {
    if (active) return SliceColorManager::instance().activeColor(sliceId);
    return SliceColorManager::instance().dimColor(sliceId);
}

// Variant that respects the SliceLetterDisplay mode (#2606): when set to
// RadioIndexed, the colour follows the radio-provided per-client letter
// rather than the global slice id so the slice marker / passband colour
// keeps pace with the badge above the VFO.
static QColor sliceColorForOverlay(const SpectrumWidget::SliceOverlay& so) {
    const int colourIdx = SliceLabel::displayColorIndex(so.sliceId, so.perClientLetter);
    if (so.isActive) return SliceColorManager::instance().activeColor(colourIdx);
    return SliceColorManager::instance().dimColor(colourIdx);
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

const SpectrumWidget::SliceOverlay* SpectrumWidget::txOverlay() const
{
    for (const auto& o : m_sliceOverlays)
        if (o.isTxSlice) return &o;
    return nullptr;
}

void SpectrumWidget::beginTxDbmRangeFreeze()
{
    if (!txWaterfallAffectsThisPan()) {
        resetTxDbmRangeFreeze();
        return;
    }

    const float frozenMinDbm = std::max(m_refLevel - m_dynamicRange, kMinDisplayDbm);
    m_txDbmRangeFrozen = true;
    m_txFrozenMinDbm = frozenMinDbm;
    m_txFrozenMaxDbm = m_refLevel;
    m_txSourceMinDbm = frozenMinDbm;
    m_txSourceMaxDbm = m_refLevel;
    m_txDeferredDbmRangeValid = false;
    m_txDeferredMinDbm = 0.0f;
    m_txDeferredMaxDbm = 0.0f;

    m_pendingDbmRangeEcho = false;
    m_pendingDbmRangeEchoFromAutoFloor = false;
    m_pendingDbmRangeEchoStartMs = 0;
    clearDbmReleaseRebase();
}

void SpectrumWidget::endTxDbmRangeFreeze()
{
    const bool applyDeferred = m_txDbmRangeFrozen && m_txDeferredDbmRangeValid;
    const float deferredMinDbm = m_txDeferredMinDbm;
    const float deferredMaxDbm = m_txDeferredMaxDbm;

    resetTxDbmRangeFreeze();

    if (applyDeferred)
        applyDbmRangeImmediate(deferredMinDbm, deferredMaxDbm);
}

void SpectrumWidget::resetTxDbmRangeFreeze()
{
    m_txDbmRangeFrozen = false;
    m_txFrozenMinDbm = 0.0f;
    m_txFrozenMaxDbm = 0.0f;
    m_txSourceMinDbm = 0.0f;
    m_txSourceMaxDbm = 0.0f;
    m_txDeferredDbmRangeValid = false;
    m_txDeferredMinDbm = 0.0f;
    m_txDeferredMaxDbm = 0.0f;
}

void SpectrumWidget::deferTxDbmRange(float minDbm, float maxDbm)
{
    const float clampedMinDbm = std::max(minDbm, kMinDisplayDbm);
    const float clampedMaxDbm = std::max(maxDbm, clampedMinDbm + 10.0f);

    m_txSourceMinDbm = clampedMinDbm;
    m_txSourceMaxDbm = clampedMaxDbm;
    m_txDeferredDbmRangeValid = true;
    m_txDeferredMinDbm = clampedMinDbm;
    m_txDeferredMaxDbm = clampedMaxDbm;
}

void SpectrumWidget::reprojectBinsToFrozenTxDbmRange(QVector<float>& bins) const
{
    if (!m_txDbmRangeFrozen || bins.isEmpty())
        return;

    const float sourceRange = m_txSourceMaxDbm - m_txSourceMinDbm;
    const float targetRange = m_txFrozenMaxDbm - m_txFrozenMinDbm;
    if (sourceRange <= 0.0f || targetRange <= 0.0f)
        return;
    if (std::abs(m_txSourceMinDbm - m_txFrozenMinDbm) < 0.01f &&
        std::abs(m_txSourceMaxDbm - m_txFrozenMaxDbm) < 0.01f) {
        return;
    }

    for (float& bin : bins) {
        if (!std::isfinite(bin))
            continue;
        const float frac = (m_txSourceMaxDbm - bin) / sourceRange;
        bin = m_txFrozenMaxDbm - frac * targetRange;
    }
}

void SpectrumWidget::setTransmitting(bool tx)
{
    if (tx && !m_transmitting) {
        m_preTxAutoBlack = m_autoBlackThresh;  // save before TX
        m_nextFallbackWaterfallRowMs = 0;      // first TX FFT row should render immediately
        beginTxDbmRangeFreeze();
    }
    if (!tx && m_transmitting) {
        m_autoBlackThresh = m_preTxAutoBlack;  // restore after TX
        m_hasNativeWaterfall = false;  // wait for native tiles to resume
        m_wfPrevTimecode   = 0;
        m_wfPrevTimecodeMs = 0;
        m_txEndMs = QDateTime::currentMSecsSinceEpoch(); // post-TX blanking (#2117)
        m_wfBlankerRingCount = 0;                        // reset stale blanker baseline
        m_wfLastGoodRow.clear();                          // forget any TX-era last-good scanline
        endTxDbmRangeFreeze();
    }
    m_transmitting = tx;
}

void SpectrumWidget::setTxWaterfallSlice(double freqMhz, int filterLowHz,
                                         int filterHighHz, bool xitOn,
                                         int xitFreq)
{
    const bool valid = freqMhz > 0.0 && filterHighHz > filterLowHz;
    const bool changed =
        m_txWaterfallSliceValid != valid ||
        m_txWaterfallFreqMhz != freqMhz ||
        m_txWaterfallFilterLowHz != filterLowHz ||
        m_txWaterfallFilterHighHz != filterHighHz ||
        m_txWaterfallXitOn != xitOn ||
        m_txWaterfallXitFreq != xitFreq;

    m_txWaterfallSliceValid = valid;
    m_txWaterfallFreqMhz = freqMhz;
    m_txWaterfallFilterLowHz = filterLowHz;
    m_txWaterfallFilterHighHz = filterHighHz;
    m_txWaterfallXitOn = xitOn;
    m_txWaterfallXitFreq = xitFreq;

    if (changed && m_transmitting) {
        if (txWaterfallAffectsThisPan()) {
            if (!m_txDbmRangeFrozen)
                beginTxDbmRangeFreeze();
        } else if (m_txDbmRangeFrozen) {
            endTxDbmRangeFreeze();
        }
    }
}

void SpectrumWidget::clearTxWaterfallSlice()
{
    if (!m_txWaterfallSliceValid &&
        m_txWaterfallFreqMhz == 0.0 &&
        m_txWaterfallFilterLowHz == 0 &&
        m_txWaterfallFilterHighHz == 0 &&
        !m_txWaterfallXitOn &&
        m_txWaterfallXitFreq == 0) {
        return;
    }

    m_txWaterfallSliceValid = false;
    m_txWaterfallFreqMhz = 0.0;
    m_txWaterfallFilterLowHz = 0;
    m_txWaterfallFilterHighHz = 0;
    m_txWaterfallXitOn = false;
    m_txWaterfallXitFreq = 0;

    if (m_transmitting) {
        if (m_txDbmRangeFrozen)
            endTxDbmRangeFreeze();
    }
}

bool SpectrumWidget::txWaterfallMaskRange(double& lowMhz, double& highMhz) const
{
    if (m_txWaterfallSliceValid &&
        m_txWaterfallFreqMhz > 0.0 &&
        m_txWaterfallFilterHighHz > m_txWaterfallFilterLowHz) {
        const double txCarrierMhz =
            m_txWaterfallFreqMhz +
            (m_txWaterfallXitOn ? m_txWaterfallXitFreq / 1.0e6 : 0.0);
        lowMhz = txCarrierMhz + m_txWaterfallFilterLowHz / 1.0e6;
        highMhz = txCarrierMhz + m_txWaterfallFilterHighHz / 1.0e6;
        if (lowMhz > highMhz)
            std::swap(lowMhz, highMhz);
        return true;
    }

    if (const SliceOverlay* tx = txOverlay()) {
        if (tx->freqMhz > 0.0 && tx->filterHighHz > tx->filterLowHz) {
            const double txCarrierMhz =
                tx->freqMhz + (tx->xitOn ? tx->xitFreq / 1.0e6 : 0.0);
            lowMhz = txCarrierMhz + tx->filterLowHz / 1.0e6;
            highMhz = txCarrierMhz + tx->filterHighHz / 1.0e6;
            if (lowMhz > highMhz)
                std::swap(lowMhz, highMhz);
            return true;
        }
    }

    return false;
}

bool SpectrumWidget::txWaterfallAffectsThisPan() const
{
    double txLowMhz = 0.0;
    double txHighMhz = 0.0;
    if (!txWaterfallMaskRange(txLowMhz, txHighMhz))
        return m_hasTxSlice;

    if (m_bandwidthMhz <= 0.0)
        return m_hasTxSlice;

    const double panLowMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double panHighMhz = m_centerMhz + m_bandwidthMhz / 2.0;
    const double edgePadMhz = width() > 0
        ? m_bandwidthMhz / static_cast<double>(width())
        : 0.0;

    return txHighMhz >= panLowMhz - edgePadMhz &&
           txLowMhz <= panHighMhz + edgePadMhz;
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

void SpectrumWidget::setSliceOverlayLetter(int sliceId, const QString& letter)
{
    for (auto& so : m_sliceOverlays) {
        if (so.sliceId == sliceId) {
            if (so.perClientLetter == letter) return;
            so.perClientLetter = letter;
            markOverlayDirty();
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
    PerfUpdateScope perfScope(PerfUpdateScope::Kind::Panadapter);
    if (!binsDbm.isEmpty()) {
        recordPanadapterFrame();
        if (PerfTelemetry::instance().enabled())
            PerfTelemetry::instance().recordPanFrame();
    }

    QVector<float> txRangeAdjustedBins;
    QVector<float> adjustedBins;
    const QVector<float>* spectrumBins = &binsDbm;
    if (m_txDbmRangeFrozen && m_transmitting && !binsDbm.isEmpty()) {
        txRangeAdjustedBins = binsDbm;
        reprojectBinsToFrozenTxDbmRange(txRangeAdjustedBins);
        spectrumBins = &txRangeAdjustedBins;
    }

    // The stream decoder switches to the requested dBm range immediately, but
    // the radio can still send a few FFT frames encoded with the old range.
    // Rebase those frames so the drag preview does not snap back on release.
    if (m_holdFftUpdatesAfterDbmRelease > 0) {
        const QVector<float>& sourceBins = *spectrumBins;
        const float oldRange =
            m_dbmReleasePreviewOldMaxDbm - m_dbmReleasePreviewOldMinDbm;
        const float newRange =
            m_dbmReleasePreviewNewMaxDbm - m_dbmReleasePreviewNewMinDbm;
        if (oldRange <= 0.0f || newRange <= 0.0f) {
            clearDbmReleaseRebase();
        } else if (!m_bins.isEmpty() && m_bins.size() == sourceBins.size()) {
            --m_holdFftUpdatesAfterDbmRelease;
            QVarLengthArray<float, kDbmReleaseErrorSampleCount> directErrors;
            QVarLengthArray<float, kDbmReleaseErrorSampleCount> rebasedErrors;
            const int step = qMax(1, sourceBins.size() / kDbmReleaseErrorSampleCount);
            const int sampleCount = (sourceBins.size() + step - 1) / step;
            directErrors.reserve(sampleCount);
            rebasedErrors.reserve(sampleCount);
            for (int i = 0; i < sourceBins.size(); i += step) {
                const float directDbm = sourceBins[i];
                const float frac = (m_dbmReleasePreviewNewMaxDbm - directDbm) / newRange;
                const float rebasedDbm = m_dbmReleasePreviewOldMaxDbm - frac * oldRange;
                directErrors.append(std::abs(directDbm - m_bins[i]));
                rebasedErrors.append(std::abs(rebasedDbm - m_bins[i]));
            }
            auto median = [](auto& errors) {
                auto mid = errors.begin() + errors.size() / 2;
                std::nth_element(errors.begin(), mid, errors.end());
                return *mid;
            };
            const float directMedian = median(directErrors);
            const float rebasedMedian = median(rebasedErrors);
            if (rebasedMedian + kDbmReleaseRebaseMinImprovementDb < directMedian) {
                adjustedBins = sourceBins;
                for (float& bin : adjustedBins) {
                    const float frac = (m_dbmReleasePreviewNewMaxDbm - bin) / newRange;
                    bin = m_dbmReleasePreviewOldMaxDbm - frac * oldRange;
                }
                spectrumBins = &adjustedBins;
            }
            if (m_holdFftUpdatesAfterDbmRelease <= 0) {
                clearDbmReleaseRebase();
            }
        }
    }

    if (m_resetFftSmoothingOnNextFrame) {
        m_smoothed = *spectrumBins;
        m_resetFftSmoothingOnNextFrame = false;
    } else if (m_smoothed.size() != spectrumBins->size()) {
        m_smoothed = *spectrumBins;
    } else {
        for (int i = 0; i < spectrumBins->size(); ++i)
            m_smoothed[i] = SMOOTH_ALPHA * (*spectrumBins)[i] + (1.0f - SMOOTH_ALPHA) * m_smoothed[i];
    }
    m_bins = *spectrumBins;

    if (!m_kiwiSdrWaterfallActive) {
        // ── Live noise floor measurement (two-pass trimmed mean) ─────────
        // Same technique as the waterfall auto-black: compute the mean of ALL
        // bins, then average only the bins at-or-below that mean. Signal peaks
        // inflate the first-pass mean and therefore exclude themselves from the
        // second pass, leaving only the flat noise baseline a human eye reads
        // as the noise floor on the scope.
        if (!spectrumBins->isEmpty()) {
            const float frameFloor = estimateNoiseFloorDbm(*spectrumBins);
            constexpr float kAlpha = 0.05f;  // ~20-frame window ≈ 0.8 s at 25 fps
            m_measuredNoiseFloorDbm = (m_measuredNoiseFloorDbm <= -500.0f)
                ? frameFloor
                : m_measuredNoiseFloorDbm * (1.0f - kAlpha)
                    + frameFloor * kAlpha;
        }

        // Noise-floor auto-adjust (Display → Floor slider) follows the visible
        // FFT source. Native Flex FFT frames drive it only while Flex is visible;
        // Kiwi rows update it from the Kiwi-derived FFT trace below.
        const bool useFreshLockFrame =
            m_noiseFloorFreshFrameCount > 0 && !spectrumBins->isEmpty();
        const bool noiseFloorFrameConsumed =
            updateNoiseFloorBaseline(useFreshLockFrame ? *spectrumBins : m_smoothed,
                                     useFreshLockFrame);
        if (useFreshLockFrame && noiseFloorFrameConsumed) {
            --m_noiseFloorFreshFrameCount;
        }
    }

    // ── Auto-squelch: own two-pass trimmed-mean noise floor ───────────────
    // Independent copy of the floor measurement — not borrowed from the
    // display pipeline so it survives any future refactor of the other block.
    // Two-pass trimmed mean: pass 1 gets the overall mean; pass 2 averages
    // only bins at-or-below that mean, excluding signal peaks.
    // EWMA (α=0.1, ~10-frame window at 25 fps) smooths frame-to-frame variation.
    // kSqlMinDbm: FLEX-8600 maps squelch_level 0-100 → -160 to -60 dBm.
    // Empirically verified on fw 4.1.5; not explicitly documented in FlexLib.
    if (m_autoSquelchEnabled && !m_transmitting && !binsDbm.isEmpty()) {
        // Pass 1 — overall mean
        float sum1 = 0.0f; int cnt1 = 0;
        for (int j = 0; j < binsDbm.size(); j += 4) { sum1 += binsDbm[j]; ++cnt1; }
        const float mean1 = sum1 / cnt1;
        // Pass 2 — noise-only bins (≤ mean)
        float sum2 = 0.0f; int cnt2 = 0;
        for (int j = 0; j < binsDbm.size(); j += 4) {
            if (binsDbm[j] <= mean1) { sum2 += binsDbm[j]; ++cnt2; }
        }
        const float frameFloor = (cnt2 > 0) ? sum2 / cnt2 : mean1;
        // EWMA with α=0.1
        m_sqlNoiseFloorDbm = (m_sqlNoiseFloorDbm <= -500.0f)
            ? frameFloor
            : 0.1f * frameFloor + 0.9f * m_sqlNoiseFloorDbm;

        constexpr float kSqlMinDbm = -160.0f;
        const float targetDbm = m_sqlNoiseFloorDbm + static_cast<float>(m_autoSqlMarginDb);
        const int level = std::clamp(
            static_cast<int>(targetDbm - kSqlMinDbm + 0.5f), 1, 100);
        if (level != m_lastAutoSquelchLevel) {
            m_lastAutoSquelchLevel = level;
            emit autoSquelchLevelSuggested(level);
        }
        markOverlayDirty();
    }

    // Native VITA waterfall tiles are the primary RX source. If they stop
    // arriving, fall back to FFT-derived rows, but pace that fallback at the
    // current observed/requested waterfall cadence so slow rates do not look
    // falsely stale and so the rate slider still governs visible scrolling.
    if (m_transmitting) {
        // TX rendering is global, not limited to the pan that owns the TX
        // slice. Any pan whose visible range intersects the TX passband uses
        // the same paced FFT rows and TX filter mask; unrelated pans keep their
        // native waterfall path.
        const bool txAffectsPan = txWaterfallAffectsThisPan();
        if (txAffectsPan && m_showTxInWaterfall) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (m_nextFallbackWaterfallRowMs <= 0) {
                m_nextFallbackWaterfallRowMs = now;
            }
            if (now >= m_nextFallbackWaterfallRowMs) {
                const bool visibleStream = beginWaterfallStreamWrite(false);
                auto restoreStream = qScopeGuard([&] {
                    endWaterfallStreamWrite(false, visibleStream);
                });
                if (!m_waterfall.isNull()) {
                    pushWaterfallRow(*spectrumBins, m_waterfall.width());
                    m_nextFallbackWaterfallRowMs = now + waterfallFallbackIntervalMs();
                }
            }
        } else if (!txAffectsPan) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            updateNativeWaterfallFallbackState(now);
            if (!m_hasNativeWaterfall) {
                pushRxWaterfallFallbackIfDue(*spectrumBins, now);
            }
        }
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
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            updateNativeWaterfallFallbackState(now);
            if (!m_hasNativeWaterfall) {
                pushRxWaterfallFallbackIfDue(*spectrumBins, now);
            }
        }
    }

    leanCappedUpdate();
}

void SpectrumWidget::updateWaterfallRow(const QVector<float>& binsIntensity,
                                        double lowFreqMhz, double highFreqMhz,
                                        quint32 timecode)
{
    PerfUpdateScope perfScope(PerfUpdateScope::Kind::Waterfall);
    // Native waterfall tiles carry intensity values (int16/128.0f, ~96-120 on HF).
    if (binsIntensity.isEmpty()) return;
    const bool visibleStream = beginWaterfallStreamWrite(false);
    auto restoreStream = qScopeGuard([&] {
        endWaterfallStreamWrite(false, visibleStream);
    });

    // Forward to GPU renderer (#502)


    if (m_waterfall.isNull()) return;

    // During TX, any pan that can see the TX passband uses FFT-derived rows as
    // the sole waterfall source. Mixing native tiles with FFT rows double-
    // advances the waterfall and produces timing/row artifacts.
    if (m_transmitting && txWaterfallAffectsThisPan()) return;

    // Suppress post-TX transient noise rows (#2117). The receiver AGC needs
    // ~400 ms to settle after TX→RX; discard waterfall data during that
    // window.
    if (m_txEndMs > 0) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_txEndMs < 400) return;
        m_txEndMs = 0;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    Q_UNUSED(timecode);

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

    m_hasNativeWaterfall = true;
    m_lastNativeTileMs = nowMs;
    m_waterfallFallbackActive = false;
    m_nextFallbackWaterfallRowMs = 0;
    m_nativeWaterfallFallbackHoldUntilMs = 0;

    const int destWidth = m_waterfall.width();
    if (destWidth <= 0) return;

    const int h = m_waterfall.height();
    if (h <= 1) return;

    int rowsToPush = 1;
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

    // NB Waterfall Blanker (#277) — suppress impulse rows.
    // Skip entirely during TX: with show-tx-in-waterfall enabled, TX-era tiles
    // flow through and would otherwise poison the rolling baseline.  Post-TX,
    // real RX rows would then read as huge "impulses" against the suppressed
    // TX-era baseline, causing the blanker to substitute m_wfLastGoodRow (a
    // TX-era scanline) for ~10–18 s while baseline slowly re-converges —
    // visible as a frozen, striped waterfall.  Freezing the ring across TX
    // means post-TX rows are compared against the pre-TX baseline instead.
    if (m_wfBlankerEnabled && !m_transmitting) {
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
    recordWaterfallFrame(rowsToPush);
    if (PerfTelemetry::instance().enabled())
        PerfTelemetry::instance().recordWaterfallNativeRows(rowsToPush);

    if (visibleStream) {
        leanCappedUpdate();
    }
}

void SpectrumWidget::setKiwiSdrWaterfallActive(bool active)
{
    if (m_kiwiSdrWaterfallActive == active) {
        return;
    }

    saveCurrentWaterfallStreamState();
    m_kiwiSdrWaterfallActive = active;
    restoreCurrentWaterfallStreamState();
    // The newly-active stream is no longer reprojected on every pan step
    // (handleWaterfallFrequencyFrameChange now touches only the active stream),
    // so its restored viewport can lag the current frequency frame. Remap it now
    // that it is visible -- exact, since each history row keeps its own capture
    // frame (#3578). This is the lazy counterpart to the per-pan reproject the
    // inactive stream used to receive.
    rebuildWaterfallViewportForFrame(m_centerMhz, m_bandwidthMhz);
    if (!active) {
        m_pendingDbmRangeEcho = false;
        m_pendingDbmRangeEchoFromAutoFloor = false;
        m_pendingDbmRangeEchoStartMs = 0;
        clearDbmReleaseRebase();
        m_resetFftSmoothingOnNextFrame = true;
        resetNoiseFloorBaseline();
    }
    reacquireNoiseFloorLockFromVisibleSource();
    updateFpsMeterLabels();
#ifdef AETHER_GPU_SPECTRUM
    m_wfTexFullUpload = true;
#endif
    leanCappedUpdate();
}

void SpectrumWidget::setKiwiSdrWaterfallAvailable(bool available)
{
    if (m_kiwiSdrWaterfallAvailable == available) {
        return;
    }

    m_kiwiSdrWaterfallAvailable = available;
    if (!available) {
        setKiwiSdrWaterfallActive(false);
    }
}

void SpectrumWidget::setKiwiSdrWaterfallProfile(const QString& profileId)
{
    const QString normalized = profileId.trimmed();
    if (m_kiwiSdrWaterfallProfileId == normalized) {
        return;
    }

    if (m_kiwiSdrWaterfallActive) {
        saveCurrentWaterfallStreamState();
    }
    m_kiwiSdrWaterfallProfileId = normalized;
    if (m_kiwiSdrWaterfallActive) {
        restoreCurrentWaterfallStreamState();
        reacquireNoiseFloorLockFromVisibleSource();
        leanCappedUpdate();
    }
}

void SpectrumWidget::clearKiwiSdrWaterfallRows()
{
    m_kiwiWaterfallState = WaterfallStreamState{};
    m_kiwiProfileWaterfallStates.clear();
    m_kiwiSdrFftTrace.clear();
    if (m_kiwiSdrWaterfallActive) {
        clearCurrentWaterfallRows();
        leanCappedUpdate();
    }
}

void SpectrumWidget::clearKiwiSdrWaterfallRowsForProfile(const QString& profileId)
{
    const QString normalized = profileId.trimmed();
    if (normalized.isEmpty()) {
        return;
    }

    m_kiwiProfileWaterfallStates.remove(normalized);
    if (m_kiwiSdrWaterfallActive
        && m_kiwiSdrWaterfallProfileId == normalized) {
        clearCurrentWaterfallRows();
        leanCappedUpdate();
    }
}

const QVector<float>& SpectrumWidget::displaySpectrumBins() const
{
    return m_kiwiSdrWaterfallActive ? m_kiwiSdrFftTrace : m_smoothed;
}

const QVector<float>& SpectrumWidget::noiseFloorAutoLevelBins() const
{
    if (m_kiwiSdrWaterfallActive) {
        return m_kiwiSdrFftTrace;
    }
    return !m_smoothed.isEmpty() ? m_smoothed : m_bins;
}

void SpectrumWidget::setKiwiSdrWaterfallAdjustments(int cellDb, int floorDb)
{
    const int clampedCell = std::clamp(cellDb, -30, 30);
    const int clampedFloor = std::clamp(floorDb, -30, 30);
    if (m_kiwiSdrWaterfallCellDb == clampedCell
        && m_kiwiSdrWaterfallFloorDb == clampedFloor) {
        return;
    }

    m_kiwiSdrWaterfallCellDb = clampedCell;
    m_kiwiSdrWaterfallFloorDb = clampedFloor;
    if (m_kiwiSdrWaterfallActive) {
        leanCappedUpdate();
    }
}

void SpectrumWidget::updateKiwiSdrWaterfallRow(const QVector<float>& binsDbm,
                                               double lowFreqMhz,
                                               double highFreqMhz,
                                               quint32 timecode)
{
    Q_UNUSED(timecode);
    if (binsDbm.isEmpty()) {
        return;
    }

    const bool visibleStream = beginWaterfallStreamWrite(true);
    auto restoreStream = qScopeGuard([&] {
        endWaterfallStreamWrite(true, visibleStream);
    });

    const int destWidth = m_waterfall.width();
    if (destWidth <= 0) {
        return;
    }

    double rowLowMhz = lowFreqMhz;
    double rowHighMhz = highFreqMhz;
    if (rowHighMhz <= rowLowMhz || rowLowMhz <= 0.0 || m_bandwidthMhz <= 0.0) {
        return;
    }
    const double rowCenterMhz = (rowLowMhz + rowHighMhz) * 0.5;
    const double rowBandwidthMhz = rowHighMhz - rowLowMhz;
    const double centerToleranceMhz = std::max(1.0e-9, rowBandwidthMhz * 1.0e-6);
    const double bandwidthToleranceMhz = std::max(1.0e-9, rowBandwidthMhz * 1.0e-6);
    const bool sameWaterfallFrame = m_kiwiSdrLastWaterfallFrameValid
        && std::abs(rowCenterMhz - m_kiwiSdrLastWaterfallCenterMhz) <= centerToleranceMhz
        && std::abs(rowBandwidthMhz - m_kiwiSdrLastWaterfallBandwidthMhz) <= bandwidthToleranceMhz;
    if (!sameWaterfallFrame) {
        m_kiwiSdrLastWaterfallBins.clear();
        m_kiwiSdrLastWaterfallCenterMhz = rowCenterMhz;
        m_kiwiSdrLastWaterfallBandwidthMhz = rowBandwidthMhz;
        m_kiwiSdrLastWaterfallFrameValid = true;
    }

    const QVector<float> smoothedBins = smoothKiwiSdrWaterfallBins(binsDbm);
    if (m_kiwiSdrWaterfallActive && destWidth > 0) {
        QVector<float> kiwiTrace(destWidth, kKiwiSdrWaterfallMinDbm);
        const double rowLow = rowCenterMhz - rowBandwidthMhz * 0.5;
        const double viewLow = m_centerMhz - m_bandwidthMhz * 0.5;
        const int srcSize = smoothedBins.size();
        for (int x = 0; x < destWidth; ++x) {
            const double freqMhz = viewLow
                + (static_cast<double>(x) + 0.5)
                    * m_bandwidthMhz / static_cast<double>(destWidth);
            const double srcCenter =
                ((freqMhz - rowLow) / rowBandwidthMhz)
                    * static_cast<double>(srcSize) - 0.5;
            if (srcCenter < -0.5
                || srcCenter > static_cast<double>(srcSize) - 0.5) {
                continue;
            }
            const double srcLeft = std::max(0.0, srcCenter - 0.5);
            const double srcRight =
                std::min(static_cast<double>(srcSize), srcCenter + 0.5);
            kiwiTrace[x] = peakPreservedBinSample(
                smoothedBins, srcLeft, srcRight, srcCenter,
                kKiwiSdrWaterfallMinDbm);
        }
        if (m_kiwiSdrFftTrace.size() != kiwiTrace.size()) {
            m_kiwiSdrFftTrace = kiwiTrace;
        } else {
            for (int i = 0; i < kiwiTrace.size(); ++i) {
                m_kiwiSdrFftTrace[i] = SMOOTH_ALPHA * kiwiTrace[i]
                    + (1.0f - SMOOTH_ALPHA) * m_kiwiSdrFftTrace[i];
            }
        }
        if (!m_kiwiSdrFftTrace.isEmpty()) {
            const float frameFloor = estimateNoiseFloorDbm(m_kiwiSdrFftTrace);
            constexpr float kAlpha = 0.05f;
            m_measuredNoiseFloorDbm = (m_measuredNoiseFloorDbm <= -500.0f)
                ? frameFloor
                : m_measuredNoiseFloorDbm * (1.0f - kAlpha)
                    + frameFloor * kAlpha;

            const bool useFreshLockFrame = m_noiseFloorFreshFrameCount > 0;
            const bool noiseFloorFrameConsumed =
                updateNoiseFloorBaseline(m_kiwiSdrFftTrace, useFreshLockFrame);
            if (useFreshLockFrame && noiseFloorFrameConsumed) {
                --m_noiseFloorFreshFrameCount;
            }
        }
    }
    pushKiwiSdrWaterfallRow(smoothedBins, destWidth,
                            rowCenterMhz, rowBandwidthMhz);
    if (visibleStream) {
        leanCappedUpdate();
    }
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
        const int chromeH = freqScaleH() + DIVIDER_H;
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
    shape = normalizedSpectrumCursorShape(shape);
    if (cursor().shape() == shape) {
        return;
    }
    setCursor(shape);
}

bool SpectrumWidget::sliceCursorShapeAt(const QPoint& localPos,
                                        Qt::CursorShape& shape) const
{
    const int chromeH = freqScaleH() + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int mx = localPos.x();
    const int y = localPos.y();
    if (y < 0 || y >= specH || mx < 0 || mx >= width() - DBM_STRIP_W) {
        return false;
    }

    for (const auto& so : m_sliceOverlays) {
        if (so.isActive) {
            continue;
        }
        const int sliceX = mhzToX(so.freqMhz);
        const int loX = mhzToX(so.freqMhz + so.filterLowHz / 1.0e6);
        const int hiX = mhzToX(so.freqMhz + so.filterHighHz / 1.0e6);
        if (filterEdgeHitAtPixel(mx, loX, hiX, kFilterEdgeGrabPx) != 0) {
            shape = Qt::SizeHorCursor;
            return true;
        }

        if ((mx >= sliceX - 8 && mx <= sliceX + 35 && y <= 25)
            || std::abs(mx - sliceX) <= 8) {
            shape = Qt::PointingHandCursor;
            return true;
        }

        const int left = std::min(loX, hiX);
        const int right = std::max(loX, hiX);
        if (mx >= left && mx <= right) {
            shape = Qt::OpenHandCursor;
            return true;
        }
    }

    if (const auto* ao = activeOverlay()) {
        const int loX = mhzToX(ao->freqMhz + ao->filterLowHz / 1.0e6);
        const int hiX = mhzToX(ao->freqMhz + ao->filterHighHz / 1.0e6);
        if (filterEdgeHitAtPixel(mx, loX, hiX, kFilterEdgeGrabPx) != 0) {
            shape = Qt::SizeHorCursor;
            return true;
        }
        if (filterPassbandBodyHitAtPixel(mx, loX, hiX, kFilterEdgeGrabPx)) {
            shape = Qt::OpenHandCursor;
            return true;
        }
    }

    return false;
}

bool SpectrumWidget::spectrumDefaultsToCrosshairAt(const QPoint& localPos) const
{
    const int chromeH = freqScaleH() + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int mx = localPos.x();
    const int y = localPos.y();
    if (y < 0 || y >= specH || mx < 0 || mx >= width() - DBM_STRIP_W) {
        return false;
    }

    for (const QRect& rect : m_offScreenRects) {
        if (!rect.isNull() && rect.contains(localPos)) {
            return false;
        }
    }

    if (tnfAtPixel(mx, m_hoveredTnfId) >= 0) {
        return false;
    }

    if (m_showSpots) {
        for (const auto& hitRect : m_spotClickRects) {
            if (hitRect.rect.contains(localPos)) {
                return false;
            }
        }
        for (const auto& cluster : m_spotClusters) {
            if (cluster.rect.contains(localPos)) {
                return false;
            }
        }
    }

    if (!m_propClickRect.isNull() && m_propClickRect.contains(localPos)) {
        return false;
    }

    return true;
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
    PerfInputScope perfScope("mousePress");
    const auto dragStatePublisher = makeScopeExit([this] { publishPerfDragState(); });
    (void)dragStatePublisher;

    const int chromeH  = freqScaleH() + DIVIDER_H;
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
                    // SHistory / QRM marker — markerIndex is past the regular
                    // spot range.  Optionally snap to the slice's step size to
                    // counter detector edge-bin imprecision.
                    const double tuneMhz = m_sHistorySnapToStep
                        ? snapToStep(hr.freqMhz, m_stepHz)
                        : hr.freqMhz;
                    emit frequencyClicked(tuneMhz);
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
    if (y >= scaleY && y < scaleY + freqScaleH()) {
        const QRect wfRect(0, scaleY + freqScaleH(), width(), height() - (scaleY + freqScaleH()));
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

    // Left-click in waterfall area -> start pan drag (tune on double-click only)
    const int wfY = scaleY + freqScaleH();
    if (y >= wfY) {
        const QRect wfRect(0, wfY, width(), height() - wfY);
        const QRect timeScaleRect = waterfallTimeScaleRect(wfRect);
        const QPoint pos = ev->position().toPoint();
        const Qt::KeyboardModifiers modifiers =
            ev->modifiers() | QGuiApplication::keyboardModifiers();
#ifdef Q_OS_MAC
        const bool rateModifier = modifiers.testFlag(Qt::ControlModifier)
            || modifiers.testFlag(Qt::MetaModifier);
        const bool rateClick = rateModifier
            && (ev->button() == Qt::LeftButton || ev->button() == Qt::RightButton);
#else
        const bool rateModifier = modifiers.testFlag(Qt::ControlModifier);
        const bool rateClick = rateModifier && ev->button() == Qt::LeftButton;
#endif
        if (rateClick && timeScaleRect.contains(pos)) {
            m_draggingTimeScaleRate = true;
            m_timeScaleDragStartY = y;
            m_timeScaleDragStartRatePercent = lineDurationToRatePercent(m_wfLineDuration);
            setSpectrumCursor(Qt::SizeVerCursor);
            ev->accept();
            return;
        }

        if (ev->button() == Qt::LeftButton) {
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
            const Qt::KeyboardModifiers modifiers =
                ev->modifiers() | QGuiApplication::keyboardModifiers();
            const bool primaryClick = ev->button() == Qt::LeftButton;
#ifdef Q_OS_MAC
            const bool rangeDrag = modifiers.testFlag(Qt::ControlModifier)
                || modifiers.testFlag(Qt::MetaModifier);
            const bool controlClick = rangeDrag
                && (primaryClick || ev->button() == Qt::RightButton);
#else
            const bool rangeDrag = modifiers.testFlag(Qt::ControlModifier);
            const bool controlClick = rangeDrag && primaryClick;
#endif
            if (controlClick) {
                m_draggingDbmRange = true;
                m_dbmDragStartY = y;
                m_dbmDragStartRef = m_refLevel;
                m_dbmDragStartRange = m_dynamicRange;
                m_dbmDragStartBottom = std::max(m_refLevel - m_dynamicRange, kMinDisplayDbm);
                setSpectrumCursor(Qt::SizeVerCursor);
                ev->accept();
                return;
            }

            if (primaryClick) {
                // Arrow row (side by side: left = up, right = down)
                if (y < DBM_ARROW_H) {
                    const float bottom = std::max(m_refLevel - m_dynamicRange, kMinDisplayDbm);
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
                    refreshNoiseFloorTarget(true, true);
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
                // Follow display mode so menu labels match the pill above (#2606).
                const QString letter =
                    SliceLabel::unicodeForm(so.sliceId, so.perClientLetter);
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
            menu.addAction(QString("Add Slice at %1 MHz").arg(freqStr), this,
                [this, snappedMhz]{ emit sliceCreateRequested(snappedMhz); });
        }

        if (hitTnf < 0) {
            // Close Slice option (only when multiple slices exist)
            if (m_sliceOverlays.size() > 1) {
                menu.addSeparator();
                int closestSlice = -1;
                int closestDist = INT_MAX;
                QString closestLetter;
                for (const auto& so : m_sliceOverlays) {
                    int dist = std::abs(mx - mhzToX(so.freqMhz));
                    if (dist < closestDist) {
                        closestDist = dist;
                        closestSlice = so.sliceId;
                        closestLetter = so.perClientLetter;
                    }
                }
                if (closestSlice >= 0) {
                    const QString letter =
                        SliceLabel::unicodeForm(closestSlice, closestLetter);
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
            const int edgeHit = filterEdgeHitAtPixel(mx, loX, hiX, kFilterEdgeGrabPx);
            if (edgeHit != 0) {
                const int edgeHz = edgeHit < 0 ? so.filterLowHz : so.filterHighHz;
                emit sliceClicked(so.sliceId);
                m_draggingFilter = edgeHit < 0 ? FilterEdge::Low : FilterEdge::High;
                m_filterDragStartX = mx;
                m_filterDragStartHz = edgeHz;
                setSpectrumCursor(Qt::SizeHorCursor);
                ev->accept();
                return;
            }
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
                m_vfoDragLastX = mx;
                m_vfoDragOffsetHz = static_cast<int>(
                    std::round((xToMhz(mx) - so.freqMhz) * 1.0e6));
                setSpectrumCursor(Qt::ClosedHandCursor);
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

        const int edgeHit = filterEdgeHitAtPixel(mx, loX, hiX, kFilterEdgeGrabPx);
        if (edgeHit != 0) {
            m_draggingFilter = edgeHit < 0 ? FilterEdge::Low : FilterEdge::High;
            // Store anchor offset so the edge doesn't snap to cursor (#764)
            const int edgeHz = (m_draggingFilter == FilterEdge::Low) ? ao->filterLowHz : ao->filterHighHz;
            m_filterDragStartX = mx;
            m_filterDragStartHz = edgeHz;

            setSpectrumCursor(Qt::SizeHorCursor);
            ev->accept();
            return;
        }

        // Click inside the filter passband → start VFO drag (#404)
        if (filterPassbandBodyHitAtPixel(mx, loX, hiX, kFilterEdgeGrabPx)) {
            m_draggingVfo = true;
            m_vfoDragLastX = mx;
            m_vfoDragOffsetHz = static_cast<int>(std::round((xToMhz(mx) - ao->freqMhz) * 1.0e6));
            setSpectrumCursor(Qt::ClosedHandCursor);
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

static QString spotMarkerTooltip(const SpectrumWidget::SpotMarker& sm);

// Compute the slice target frequency under cursor X (offset-anchored, snapped)
// Tune the slice for the live, in-window drag path (cursor not at the edge).
// Deliberately routed through edgePanTuneRequested with the center UNCHANGED so
// the whole drag bypasses pan-follow/reveal: reveal is a position controller
// whose flag-extended trigger (#2761) fires asymmetrically a little inside the
// edge — inside our velocity zone on the flag side — and fights the edge-pan,
// producing a one-sided "rubber band" stutter.  With reveal out of the drag
// path, in-window moves only tune; the velocity zone owns all panning.  The
// MainWindow handler skips the redundant pan command when the center is
// unchanged.  (user-reported)
void SpectrumWidget::driveVfoDragTune(int mx, const char* phase)
{
    const double mhz = snapToStep(xToMhz(mx) - m_vfoDragOffsetHz / 1.0e6, m_stepHz);
    qCDebug(lcPerf).nospace()
        << "SliceDrag phase=" << phase
        << " mx=" << mx << " w=" << width()
        << " center=" << m_centerMhz << " bw=" << m_bandwidthMhz
        << " tuneMhz=" << mhz;
    emit edgePanTuneRequested(m_centerMhz, mhz);
}

// Start the edge-pan velocity timer while the cursor sits in the edge zone
// (within kVfoDragEdgeZoneFrac of either border), stop it otherwise.  Returns
// whether the cursor is currently in the edge zone, so the caller can skip the
// normal in-window tune and let the timer own panning.  Restarts the hold ramp
// each time the zone is (re-)entered.  (user-reported)
bool SpectrumWidget::updateVfoDragEdgePan(int mx)
{
    const int zonePx = std::max(1, static_cast<int>(width() * kVfoDragEdgeZoneFrac));
    const bool inEdgeZone = !m_vfoDragEdgePanDisabled
        && ((mx <= zonePx) || (mx >= width() - zonePx));
    if (!m_vfoDragEdgePanTimer) {
        return inEdgeZone;
    }
    if (inEdgeZone) {
        if (!m_vfoDragEdgePanTimer->isActive()) {
            m_vfoDragEdgeHoldTicks = 0;     // fresh ramp on (re-)entry
            m_vfoDragEdgePanTimer->start();
        }
    } else if (m_vfoDragEdgePanTimer->isActive()) {
        m_vfoDragEdgePanTimer->stop();
    }
    return inEdgeZone;
}

// One edge-pan velocity tick: pan the view at a speed that scales with how deep
// the cursor is in the edge zone (depth) and ramps up the longer it is held,
// keeping the dragged slice parked just inside the leading edge.  Unlike the
// reveal-follow
// (a position controller whose nudge is bounded by the cursor's bounded
// overshoot — the old "rubber band" creep), this is a velocity controller, so
// the band sweeps continuously and reaches across the whole range.  Pans the
// waterfall/overlay locally for an immediate response, then hands the
// pan+tune to MainWindow via edgePanTuneRequested (no reveal).  (user-reported)
void SpectrumWidget::edgePanVelocityStep()
{
    const int w = width();
    const int zonePx = std::max(1, static_cast<int>(w * kVfoDragEdgeZoneFrac));
    int borderDist;
    double dir;
    if (m_vfoDragLastX <= zonePx) {
        borderDist = m_vfoDragLastX;
        dir = -1.0;                          // pan toward lower frequencies
    } else if (m_vfoDragLastX >= w - zonePx) {
        borderDist = w - m_vfoDragLastX;
        dir = 1.0;                           // pan toward higher frequencies
    } else {
        m_vfoDragEdgePanTimer->stop();         // cursor left the zone
        return;
    }

    const double depth = std::clamp(
        static_cast<double>(zonePx - borderDist) / static_cast<double>(zonePx), 0.0, 1.0);
    ++m_vfoDragEdgeHoldTicks;
    const double rampFactor = std::min(1.0,
        static_cast<double>(m_vfoDragEdgeHoldTicks * m_edgePanIntervalMs)
            / static_cast<double>(std::max(1, m_edgePanRampMs)));
    const double vmaxBwPerSec = m_edgePanVmaxPctBw / 100.0;
    const double deltaMhz = depth * rampFactor * vmaxBwPerSec * m_bandwidthMhz
                          * (m_edgePanIntervalMs / 1000.0);
    if (deltaMhz <= 0.0) {
        return;
    }

    const double newCenter = std::max(m_centerMhz + dir * deltaMhz, m_bandwidthMhz / 2.0);
    if (qFuzzyCompare(newCenter, m_centerMhz)) {
        return;
    }

    reprojectWaterfall(m_centerMhz, m_bandwidthMhz, newCenter, m_bandwidthMhz);
    m_centerMhz = newCenter;
    markOverlayDirty();

    // Park the slice a comfortable margin inside the leading edge rather than
    // exactly under the cursor: the cursor is jammed against the window border,
    // so following it literally pushes the slice (and its flag) off-screen.
    // Clamping the reference X to the zone boundary keeps the slice visibly
    // parked at the edge while the band scrolls under it, and stays continuous
    // with the in-window path at the boundary.  (user-reported)
    const int sliceX = std::clamp(m_vfoDragLastX, zonePx, w - zonePx);
    const double sliceFreq = snapToStep(xToMhz(sliceX) - m_vfoDragOffsetHz / 1.0e6, m_stepHz);
    qCDebug(lcPerf).nospace()
        << "SliceDrag phase=edgepan dir=" << dir
        << " depth=" << depth << " ramp=" << rampFactor
        << " dMhz=" << deltaMhz << " center=" << m_centerMhz
        << " sliceX=" << sliceX << " sliceMhz=" << sliceFreq;
    emit edgePanTuneRequested(newCenter, sliceFreq);
}

void SpectrumWidget::mouseMoveEvent(QMouseEvent* ev)
{
    PerfInputScope perfScope("mouseMove");
    if (PerfTelemetry::instance().enabled()) {
        const qint64 nowNs = PerfTelemetry::nowNs();
        if (m_lastMouseMoveNs > 0) {
            PerfTelemetry::instance().recordMouseMoveGap(
                static_cast<double>(nowNs - m_lastMouseMoveNs) / 1000000.0);
        }
        m_lastMouseMoveNs = nowNs;
    }
    const auto dragStatePublisher = makeScopeExit([this] { publishPerfDragState(); });
    (void)dragStatePublisher;

    const int chromeH  = freqScaleH() + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int y = static_cast<int>(ev->position().y());
    const int mx = static_cast<int>(ev->position().x());
    const QPoint mousePos = ev->position().toPoint();
    Qt::CursorShape sliceCursorShape = Qt::ArrowCursor;
    const bool overSliceCursorTarget = sliceCursorShapeAt(mousePos, sliceCursorShape);
    if (!anyDragActive()) {
        if (overSliceCursorTarget) {
            setVfoCursorOverride(sliceCursorShape);
        } else {
            clearVfoCursorOverride();
        }
    }

    // Let child widgets (overlay menu buttons) own the pointer while hovered so
    // their tooltips are not killed by SpectrumWidget's QToolTip::hideText() calls.
    // Slice passband/edge zones are still owned by the spectrum because their
    // cursor advertises the drag action before click. (#2355)
    if (!anyDragActive() && childAt(mousePos) && !overSliceCursorTarget) {
        ev->ignore();
        return;
    }

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
        positionFpsMeterLabels();
        markOverlayDirty();
        ev->accept();
        return;
    }

    if (m_draggingDbmRange) {
        const int dragHeight = std::max(1, specH);
        const int dy = m_dbmDragStartY - y;
        const float deltaDb = (static_cast<float>(dy) / dragHeight) * m_dbmDragStartRange;
        m_dynamicRange = std::max(10.0f, m_dbmDragStartRange + deltaDb);
        m_refLevel = m_dbmDragStartBottom + m_dynamicRange;
        markOverlayDirty();
        ev->accept();
        return;
    }

    if (m_draggingDbm) {
        const int dragHeight = std::max(1, specH);
        const int dy = y - m_dbmDragStartY;
        // Convert pixel drag to dB: full FFT height = full dynamic range
        const float deltaDb = (static_cast<float>(dy) / dragHeight) * m_dynamicRange;
        m_refLevel = m_dbmDragStartRef + deltaDb;
        m_refLevel = std::max(m_refLevel, kMinDisplayDbm + m_dynamicRange);
        markOverlayDirty();
        ev->accept();
        return;
    }

    if (m_draggingTimeScaleRate) {
        const int wfY = specH + DIVIDER_H + freqScaleH();
        const QRect wfRect(0, wfY, width(), height() - wfY);
        const QRect timeScaleRect = waterfallTimeScaleRect(wfRect);
        const int dragHeight = std::max(1, timeScaleRect.height());
        const int dy = m_timeScaleDragStartY - y;
        const int rangePct = kWaterfallRatePercentMax - kWaterfallRatePercentMin;
        const int deltaPct = static_cast<int>(
            std::round((static_cast<double>(dy) / dragHeight) * rangePct));
        // Screen Y decreases while dragging up. On the time scale, dragging up
        // should slow the waterfall, so reduce the rate percent.
        const int newRatePct = std::clamp(m_timeScaleDragStartRatePercent - deltaPct,
                                          kWaterfallRatePercentMin,
                                          kWaterfallRatePercentMax);
        const int newMs = ratePercentToLineDuration(newRatePct);

        if (newMs != m_wfLineDuration) {
            emit waterfallLineDurationChangeRequested(newMs);
        }

        setSpectrumCursor(Qt::SizeVerCursor);
        ev->accept();
        return;
    }

    if (m_draggingTimeScale) {
        const int wfY = specH + DIVIDER_H + freqScaleH();
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
        const double zoomCenter = std::max(m_bwDragAnchorMhz - mouseXFrac * newBw,
                                           newBw / 2.0);
        handleWaterfallFrequencyFrameChange(m_centerMhz, m_bandwidthMhz,
                                            zoomCenter, newBw);
        if (!reprojectSpectrum(m_centerMhz, m_bandwidthMhz, zoomCenter, newBw)) {
            m_bins.clear();
            m_smoothed.clear();
        }
        m_bandwidthMhz = newBw;
        m_centerMhz = zoomCenter;
        resetNoiseFloorBaseline();
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
        m_vfoDragLastX = mx;
        // In the edge zone the velocity timer owns panning (keeps the slice
        // pinned under the cursor); only tune directly when in-window so a
        // normal drag still tunes live.
        if (!updateVfoDragEdgePan(mx)) {
            driveVfoDragTune(mx, "move");
        }
        ev->accept();
        return;
    }

    if (m_draggingPan) {
        const int dx = static_cast<int>(ev->position().x()) - m_panDragStartX;
        // Dragging right moves the view right → center shifts left
        const double deltaMhz = -(static_cast<double>(dx) / width()) * m_bandwidthMhz;
        const double newCenter = std::max(m_panDragStartCenter + deltaMhz,
                                          m_bandwidthMhz / 2.0);
        handleWaterfallFrequencyFrameChange(m_centerMhz, m_bandwidthMhz,
                                            newCenter, m_bandwidthMhz);
        m_centerMhz = newCenter;
        markOverlayDirty();
        emit centerChangeRequested(newCenter);
        if (s_starstruckMode && s_starstruckSound
            && s_starstruckSound->isLoaded() && !s_starstruckSound->isPlaying()) {
            s_starstruckSound->play();
        }
        ev->accept();
        return;
    }

    // Update cursor based on hover position
    const int wfY = specH + DIVIDER_H + freqScaleH();

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
                // Surface the Ctrl-drag span-zoom affordance (#2724).
                // Use a plain string literal (not QStringLiteral) so the
                // platform-conditional #ifdef block sits at the preprocessor
                // level rather than inside a macro call — MSVC strictly
                // rejects preprocessor directives inside macro arguments.
                const QRect stripRect(stripX, 0, DBM_STRIP_W, specH);
                static const QString tip =
                    "<b>dBm scale</b><br>"
                    "Drag &mdash; pan reference level<br>"
#ifdef Q_OS_MAC
                    "Ctrl-drag or &#8984;-drag &mdash; zoom span (anchor at bottom)<br>"
#else
                    "Ctrl-drag &mdash; zoom span (anchor at bottom)<br>"
#endif
                    "&#9650; / &#9660; &mdash; &plusmn;10 dB steps";
                QToolTip::showText(ev->globalPosition().toPoint() + QPoint(0, 20),
                                   tip, this, stripRect);
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
                if (!foundCursor) {
                    Qt::CursorShape cursorShape = Qt::ArrowCursor;
                    if (sliceCursorShapeAt(pos, cursorShape)) {
                        setSpectrumCursor(cursorShape);
                        foundCursor = true;
                    }
                }
                if (!foundCursor && m_showSpots) {
                    bool spotHover = false;
                    for (const auto& hr : m_spotClickRects) {
                        if (hr.rect.contains(pos)) {
                            setSpectrumCursor(Qt::PointingHandCursor);
                            foundCursor = true;
                            if (hr.markerIndex >= 0 && hr.markerIndex < m_spotMarkers.size()) {
                                m_hoveredSpotKey = hr.callsign + QChar('@')
                                    + QString::number(qRound(hr.freqMhz * 1000.0));
                                QToolTip::showText(ev->globalPosition().toPoint() + QPoint(0, 20),
                                                   spotMarkerTooltip(m_spotMarkers[hr.markerIndex]),
                                                   this, hr.rect);
                            }
                            spotHover = true;
                            break;
                        }
                    }
                    if (!spotHover)
                        m_hoveredSpotKey.clear();
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
    const int specH2 = static_cast<int>((height() - freqScaleH() - DIVIDER_H) * m_spectrumFrac);
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
                QToolTip::showText(ev->globalPosition().toPoint() + QPoint(0, 20),
                    QString("%1 MHz — %2")
                        .arg(spot.freqMhz, 0, 'f', 3)
                        .arg(spot.label),
                    this,
                    QRect(sx - 5, bandBarTop, 11, specH2 - bandBarTop));
                return;
            }
        }
        QToolTip::hideText();
    }
}

void SpectrumWidget::mouseReleaseEvent(QMouseEvent* ev)
{
    PerfInputScope perfScope("mouseRelease");
    const auto dragStatePublisher = makeScopeExit([this] { publishPerfDragState(); });
    (void)dragStatePublisher;
    clearVfoCursorOverride();

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
        if (width() >= 100 && spectrumPixelHeight() >= 20) {
            emit dimensionsChanged(width(), spectrumPixelHeight());
        }
        ev->accept();
        return;
    }
    if (m_draggingDbm || m_draggingDbmRange) {
        const float oldMinDbm = m_draggingDbmRange
            ? m_dbmDragStartBottom
            : std::max(m_dbmDragStartRef - m_dynamicRange, kMinDisplayDbm);
        const float oldMaxDbm = m_dbmDragStartRef;
        m_pendingDbmRangeEcho = true;
        m_pendingDbmRangeEchoFromAutoFloor = false;
        m_pendingDbmRangeEchoStartMs = QDateTime::currentMSecsSinceEpoch();
        m_pendingMinDbm = m_refLevel - m_dynamicRange;
        m_pendingMaxDbm = m_refLevel;
        m_dbmReleasePreviewOldMinDbm = oldMinDbm;
        m_dbmReleasePreviewOldMaxDbm = oldMaxDbm;
        m_dbmReleasePreviewNewMinDbm = m_pendingMinDbm;
        m_dbmReleasePreviewNewMaxDbm = m_pendingMaxDbm;
        m_holdFftUpdatesAfterDbmRelease =
            (std::abs(oldMinDbm - m_pendingMinDbm) > kDbmReleasePreviewChangeThresholdDb
             || std::abs(oldMaxDbm - m_pendingMaxDbm)
                 > kDbmReleasePreviewChangeThresholdDb) ? kDbmReleaseHoldFrames : 0;
        m_draggingDbm = false;
        m_draggingDbmRange = false;
        setSpectrumCursor(Qt::CrossCursor);
        m_resetFftSmoothingOnNextFrame = true;
        refreshNoiseFloorTarget(true, true);
        emit dbmRangeDragFinished(m_pendingMinDbm, m_pendingMaxDbm);
        ev->accept();
        return;
    }
    if (m_draggingTimeScale) {
        m_draggingTimeScale = false;
        setSpectrumCursor(Qt::CrossCursor);
        ev->accept();
        return;
    }
    if (m_draggingTimeScaleRate) {
        m_draggingTimeScaleRate = false;
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
        if (m_vfoDragEdgePanTimer)
            m_vfoDragEdgePanTimer->stop();
        m_vfoDragEdgeHoldTicks = 0;
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
        if (s_starstruckSound) s_starstruckSound->stop();

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
    AetherSDR::ThemeManager::instance().applyStyleSheet(&dlg, "QDialog { background: {{color.background.0}}; color: {{color.text.primary}}; }"
                      "QLineEdit { background: {{color.background.0}}; color: {{color.text.primary}}; border: 1px solid #304060; padding: 4px; }"
                      "QDoubleSpinBox { background: {{color.background.0}}; color: {{color.text.primary}}; border: 1px solid #304060; padding: 4px; }"
                      "QDoubleSpinBox::up-button { background: #304060; width: 16px; }"
                      "QDoubleSpinBox::down-button { background: #304060; width: 16px; }"
                      "QComboBox { background: {{color.background.0}}; color: {{color.text.primary}}; border: 1px solid #304060; padding: 4px; }"
                      "QLabel { color: {{color.text.primary}}; }"
                      "QCheckBox { color: {{color.text.primary}}; }");

    auto* layout = new QFormLayout(&dlg);

    auto* freqSpin = new QDoubleSpinBox;
    freqSpin->setRange(0.030, 50000.000);  // 50 GHz cap matches VfoWidget convention
    freqSpin->setDecimals(6);
    freqSpin->setSingleStep(m_stepHz > 0 ? m_stepHz / 1e6 : 0.001);
    freqSpin->setSuffix(" MHz");
    freqSpin->setValue(freqMhz);
    freqSpin->setAlignment(Qt::AlignRight);
    layout->addRow("Frequency (MHz):", freqSpin);

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

    freqSpin->setFocus();
    freqSpin->selectAll();

    if (dlg.exec() != QDialog::Accepted) return;

    const double finalFreqMhz = freqSpin->value();
    const QString callsign = callEdit->text().trimmed().toUpper();
    if (callsign.isEmpty()) return;

    const QString comment = commentEdit->text().trimmed();
    const int lifetimeSec = lifetimeCombo->currentData().toInt();
    const bool forward = forwardCheck->isChecked();

    // Remember preferences
    as.setValue("ManualSpotLifetime", QString::number(lifetimeSec));
    as.setValue("SpotForwardToCluster", forward ? "True" : "False");

    emit spotAddRequested(finalFreqMhz, callsign, comment, lifetimeSec, forward);
}

void SpectrumWidget::mouseDoubleClickEvent(QMouseEvent* ev)
{
    const int chromeH  = freqScaleH() + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int wfY = specH + DIVIDER_H + freqScaleH();
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
    clearVfoCursorOverride();
    m_hoveredSpotKey.clear();
    m_lastTooltipRect = {};
    updateTrackedCursorState(QPoint(-1, -1), false);
}

void SpectrumWidget::setLeanMode(bool on)
{
    if (m_leanMode == on)
        return;
    m_leanMode = on;
    m_leanRepaintClock.invalidate();
    markOverlayDirty();  // re-render with/without wallpaper + fill
}

void SpectrumWidget::leanCappedUpdate()
{
    if (m_leanMode) {
        if (m_leanRepaintClock.isValid()
            && m_leanRepaintClock.elapsed() < kLeanFrameMs) {
            return;  // drop frames above ~30 Hz (kLeanFrameMs = 33)
        }
        m_leanRepaintClock.restart();
    }
    update();
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

void SpectrumWidget::setBackgroundFillColor(const QColor& c)
{
    if (!c.isValid() || c == m_bgFillColor) return;
    m_bgFillColor = c;
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("BackgroundFillColor"), m_bgFillColor.name());
    s.save();
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
            const double newCenter = std::max(anchorMhz - mouseXFrac * newBw,
                                              newBw / 2.0);
            handleWaterfallFrequencyFrameChange(m_centerMhz, m_bandwidthMhz,
                                                newCenter, newBw);
            if (!reprojectSpectrum(m_centerMhz, m_bandwidthMhz, newCenter, newBw)) {
                m_bins.clear();
                m_smoothed.clear();
            }
            m_bandwidthMhz = newBw;
            m_centerMhz = newCenter;
            resetNoiseFloorBaseline();
            markOverlayDirty();
            emit frequencyRangeChangeRequested(newCenter, newBw);
            return true;
        }
    }
    return SPECTRUM_BASE_CLASS::event(ev);
}

bool SpectrumWidget::eventFilter(QObject* watched, QEvent* event)
{
    QWidget* widget = qobject_cast<QWidget*>(watched);
    if (!widget || anyDragActive()) {
        return SPECTRUM_BASE_CLASS::eventFilter(watched, event);
    }

    bool vfoDescendant = false;
    for (QWidget* current = widget; current && current != this;
         current = current->parentWidget()) {
        if (qobject_cast<VfoWidget*>(current)) {
            vfoDescendant = true;
            break;
        }
    }

    if (!vfoDescendant) {
        return SPECTRUM_BASE_CLASS::eventFilter(watched, event);
    }

    QPoint localPos;
    bool hasPosition = false;
    if (event->type() == QEvent::MouseMove) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        localPos = mapFromGlobal(mouseEvent->globalPosition().toPoint());
        hasPosition = true;
    } else if (event->type() == QEvent::Enter) {
        auto* enterEvent = static_cast<QEnterEvent*>(event);
        localPos = mapFromGlobal(enterEvent->globalPosition().toPoint());
        hasPosition = true;
    }

    if (hasPosition) {
        Qt::CursorShape cursorShape = Qt::ArrowCursor;
        if (sliceCursorShapeAt(localPos, cursorShape)) {
            setSpectrumCursor(cursorShape);
            setVfoCursorOverride(cursorShape);
        } else {
            clearVfoCursorOverride();
            setSpectrumCursor(Qt::CrossCursor);
        }
    }

    return SPECTRUM_BASE_CLASS::eventFilter(watched, event);
}

// ─── Starstruck easter egg ────────────────────────────────────────────────────

void SpectrumWidget::ensureStarstruckSoundLoaded()
{
    if (s_starstruckSound) return;
    s_starstruckSound = new QSoundEffect(qApp);
    s_starstruckSound->setSource(QUrl("qrc:/sounds/aetherial.wav"));
    s_starstruckSound->setVolume(0.03f);
    // The WAV is a forward+reverse palindrome, so infinite looping while
    // the drag is held produces a seamless back-and-forth effect.
    s_starstruckSound->setLoopCount(QSoundEffect::Infinite);
}

void SpectrumWidget::toggleStarstruckMode()
{
    s_starstruckMode = !s_starstruckMode;
    if (s_starstruckMode) {
        // Preload so the first drag plays immediately (loading is async).
        ensureStarstruckSoundLoaded();
    } else if (s_starstruckSound) {
        s_starstruckSound->stop();
    }
}

// ─── Wheel ────────────────────────────────────────────────────────────────────

void SpectrumWidget::wheelEvent(QWheelEvent* ev)
{
    PerfInputScope perfScope("wheel");

    // Skip scroll on the divider + freq scale bar.
    const int chromeH  = freqScaleH() + DIVIDER_H;
    const int contentH2 = height() - chromeH;
    const int specH2 = static_cast<int>(contentH2 * m_spectrumFrac);
    const int wfY = specH2 + DIVIDER_H + freqScaleH();
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

    // Ctrl+wheel → zoom bandwidth anchored on the frequency under the cursor.
    // Mirrors the pinch-to-zoom gesture and the Ctrl-drag convention used on
    // the dBm scale strip (PR #2717) and waterfall time scale (PR #2783).
    if (ev->modifiers() & Qt::ControlModifier) {
        const double factor  = (steps > 0) ? (1.0 / 1.5) : 1.5;
        const double newBw   = std::clamp(m_bandwidthMhz * factor, m_minBwMhz, m_maxBwMhz);
        if (qFuzzyCompare(newBw, m_bandwidthMhz)) { ev->accept(); return; }
        const double mouseXFrac = ev->position().x() / width() - 0.5;
        const double anchorMhz  = m_centerMhz + mouseXFrac * m_bandwidthMhz;
        const double newCenter  = std::max(anchorMhz - mouseXFrac * newBw, newBw / 2.0);
        handleWaterfallFrequencyFrameChange(m_centerMhz, m_bandwidthMhz,
                                            newCenter, newBw);
        if (!reprojectSpectrum(m_centerMhz, m_bandwidthMhz, newCenter, newBw)) {
            m_bins.clear();
            m_smoothed.clear();
        }
        m_centerMhz    = newCenter;
        m_bandwidthMhz = newBw;
        resetNoiseFloorBaseline();
        markOverlayDirty();
        emit frequencyRangeChangeRequested(newCenter, newBw);
        ev->accept();
        return;
    }

    // Reverse-wheel option (#3302) — applied AFTER the Ctrl+wheel zoom branch
    // above so zoom direction stays natural; only frequency tuning is flipped.
    if (reverseMouseWheel()) steps = -steps;

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

    const int chromeH  = freqScaleH() + DIVIDER_H;
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
    positionFpsMeterLabels();
    positionInterlockNotification();

    // Notify MainWindow so it can re-push xpixels/ypixels to the radio (#1511)
    if (width() >= 100 && spectrumPixelHeight() >= 20) {
        emit dimensionsChanged(width(), spectrumPixelHeight());
    }
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

void SpectrumWidget::positionInterlockNotification()
{
    if (!m_interlockNotificationLabel)
        return;

    const int x = qMax(0, (width() - m_interlockNotificationLabel->width()) / 2);
    const int y = qMax(0, (height() - m_interlockNotificationLabel->height()) / 2);
    m_interlockNotificationLabel->move(x, y);
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

QRgb SpectrumWidget::kiwiSdrLevelToRgb(float level) const
{
    // Kiwi W/F bytes are display/bin intensity values, not calibrated dBm.
    // They are mapped into a Kiwi-only pseudo-dB display range so the
    // existing waterfall color machinery can be reused without touching Flex.
    const float floorDbm = m_kiwiSdrAutoRangeValid
        ? m_kiwiSdrAutoFloorDbm
        : kKiwiSdrWaterfallMinDbm;
    const float ceilDbm = m_kiwiSdrAutoRangeValid
        ? m_kiwiSdrAutoCeilDbm
        : kKiwiSdrWaterfallMaxDbm;
    const float adjustedFloorDbm = floorDbm
        + static_cast<float>(m_kiwiSdrWaterfallFloorDb);
    const float adjustedCeilDbm = qMax(
        adjustedFloorDbm + 1.0f,
        ceilDbm + static_cast<float>(m_kiwiSdrWaterfallCellDb));
    const float t = KiwiSdrProtocol::waterfallColorIndex(
        level, adjustedFloorDbm, adjustedCeilDbm);

    int n = 0;
    const auto* stops = wfSchemeStops(m_wfColorScheme, n);
    return interpolateGradient(t, stops, n);
}

// Cubic colour-gain curve mapping the radio's black point (low) to a white
// point (high):
//   num  = (100 − colorGain)/100 · cbrt(65535 − low)
//   high = low + num³        (floored at low + 100)
// colorGain 0 → full range (dim); 100 → narrow range (max contrast).
static float wfHighThresholdRaw(float lowRaw, int colorGain)
{
    const float low = qBound(0.0f, lowRaw, 65535.0f);
    const double num = (100.0 - colorGain) / 100.0 * std::cbrt(65535.0 - low);
    double high = low + num * num * num;
    if (high < low + 100.0) {
        high = low + 100.0;
    }
    return static_cast<float>(high);
}

// Map native waterfall tile intensity to RGB.
// Intensity is int16(raw)/128.0f — observed range ~96-120 on HF.
// m_wfBlackLevel and m_wfColorGain control the mapping independently from FFT.
QRgb SpectrumWidget::intensityToRgb(float intensity) const
{
    // Two auto-black paths (intensity arrives as raw_uint16 / 128):
    //  • Radio-authoritative: the radio's per-tile black level is the low/black
    //    point; the white point follows the cubic colour-gain curve
    //    (wfHighThresholdRaw). Reproduces the radio's evenly-levelled floor.
    //  • Fallback (no radio auto-black yet, or auto-black off): the prior
    //    client-side noise-floor estimate / manual black level.
    // The auto-black offset slider biases the black point: 50 = no bias,
    // <50 darker, >50 lighter.
    float blackThresh;   // low point  (intensity domain)
    float rangeWidth;    // high − low (intensity domain)
    if (m_wfAutoBlack && m_wfAutoBlackRadioSide && m_radioAutoBlackRaw > 0.0f) {
        // Clamp once so the black point, white point, and range all derive from
        // the same low value — the offset can push lowRaw out of [0, 65535].
        const float lowRaw = qBound(
            0.0f,
            m_radioAutoBlackRaw + (50 - m_wfAutoBlackOffset) * 0.5f * 128.0f,
            65535.0f);
        const float highRaw = wfHighThresholdRaw(lowRaw, m_wfColorGain);
        blackThresh = lowRaw / 128.0f;
        rangeWidth  = std::max(1.0f, (highRaw - lowRaw) / 128.0f);
    } else if (m_wfAutoBlack) {
        blackThresh = m_autoBlackThresh + (50 - m_wfAutoBlackOffset) * 0.5f;
        rangeWidth  = std::max(1.0f, 120.0f - m_wfColorGain * 0.91f);
    } else {
        // Manual: slider 0 → thresh 160 (well above noise), slider 100 → thresh 60.
        blackThresh = 160.0f - m_wfBlackLevel * 1.0f;
        rangeWidth  = std::max(1.0f, 120.0f - m_wfColorGain * 0.91f);
    }

    const float t = qBound(0.0f, (intensity - blackThresh) / rangeWidth, 1.0f);

    int n = 0;
    const auto* stops = wfSchemeStops(m_wfColorScheme, n);
    return interpolateGradient(t, stops, n);
}

// ─── Waterfall update ─────────────────────────────────────────────────────────

void SpectrumWidget::pushWaterfallRow(const QVector<float>& bins, int destWidth,
                                      double tileLowMhz, double tileHighMhz)
{
    // Callers own cadence: TX and RX stale-native fallback pace FFT-derived
    // rows from the requested waterfall interval before appending here.
    // Time-axis labelling uses m_wfMsPerRow, seeded from the requested rate and
    // corrected from appended-row timestamps, so the scale follows whichever
    // path is currently producing visible rows.

    if (m_waterfall.isNull() || destWidth <= 0) return;

    const int h = m_waterfall.height();
    if (h <= 1) return;

    if (bins.isEmpty()) return;

    Q_UNUSED(tileLowMhz);
    Q_UNUSED(tileHighMhz);

    bool useTxFilterMask = false;
    double txMaskLowMhz = 0.0;
    double txMaskHighMhz = 0.0;
    if (m_transmitting && txWaterfallAffectsThisPan())
        useTxFilterMask = txWaterfallMaskRange(txMaskLowMhz, txMaskHighMhz);

    const int srcSize = bins.size();
    const double panStartMhz = m_centerMhz - m_bandwidthMhz / 2.0;

    QVector<QRgb> scanline(destWidth, qRgb(0, 0, 0));
    for (int x = 0; x < destWidth; ++x) {
        if (useTxFilterMask) {
            const double freqMhz = panStartMhz
                + (static_cast<double>(x) / static_cast<double>(destWidth))
                    * m_bandwidthMhz;
            if (freqMhz < txMaskLowMhz || freqMhz > txMaskHighMhz) {
                scanline[x] = qRgb(0, 0, 0);
                continue;
            }
        }

        float dbm = m_wfMinDbm;
        if (srcSize == 1) {
            dbm = std::max(bins[0], kMinDisplayDbm);
        } else if (srcSize > 1) {
            const float binF = (destWidth > 1)
                ? static_cast<float>(x) * static_cast<float>(srcSize - 1)
                      / static_cast<float>(destWidth - 1)
                : 0.0f;
            const int binIdx = std::clamp(static_cast<int>(binF), 0, srcSize - 1);
            const int nextIdx = std::min(binIdx + 1, srcSize - 1);
            const float frac = binF - static_cast<float>(binIdx);
            const float b0 = std::max(bins[binIdx], kMinDisplayDbm);
            const float b1 = std::max(bins[nextIdx], kMinDisplayDbm);
            dbm = b0 + frac * (b1 - b0);
        }
        scanline[x] = dbmToRgb(dbm);
    }

    appendHistoryRow(scanline.constData(), QDateTime::currentMSecsSinceEpoch());
    if (m_wfLive) {
        appendVisibleRow(scanline.constData());
    } else {
        rebuildWaterfallViewport();
    }
    recordWaterfallFrame();
    if (PerfTelemetry::instance().enabled())
        PerfTelemetry::instance().recordWaterfallFallbackRows(1);
}

void SpectrumWidget::pushKiwiSdrWaterfallRow(const QVector<float>& bins,
                                             int destWidth,
                                             double rowCenterMhz,
                                             double rowBandwidthMhz)
{
    if (m_waterfall.isNull() || destWidth <= 0 || bins.isEmpty()) {
        return;
    }

    const int h = m_waterfall.height();
    if (h <= 1) {
        return;
    }

    const int srcSize = bins.size();
    updateKiwiSdrAutoColorRange(bins);
    if (rowCenterMhz <= 0.0 || rowBandwidthMhz <= 0.0) {
        rowCenterMhz = m_centerMhz;
        rowBandwidthMhz = m_bandwidthMhz;
    }

    QVector<QRgb> scanline(destWidth, qRgb(0, 0, 0));
    for (int x = 0; x < destWidth; ++x) {
        const double srcLeft = static_cast<double>(x)
            * static_cast<double>(srcSize) / static_cast<double>(destWidth);
        const double srcRight = static_cast<double>(x + 1)
            * static_cast<double>(srcSize) / static_cast<double>(destWidth);
        const double srcCenter = (static_cast<double>(x) + 0.5)
            * static_cast<double>(srcSize) / static_cast<double>(destWidth) - 0.5;
        const float level = peakPreservedBinSample(
            bins, srcLeft, srcRight, srcCenter, kKiwiSdrWaterfallMinDbm);
        scanline[x] = kiwiSdrLevelToRgb(level);
    }

    appendHistoryRow(scanline.constData(), QDateTime::currentMSecsSinceEpoch(),
                     rowCenterMhz, rowBandwidthMhz);
    if (m_wfLive) {
        QVector<QRgb> visibleLine(destWidth, qRgb(0, 0, 0));
        remapHistoryRowInto(visibleLine.data(), scanline.constData(), destWidth,
                            rowCenterMhz, rowBandwidthMhz,
                            m_centerMhz, m_bandwidthMhz,
                            true);
        appendVisibleRow(visibleLine.constData());
    } else {
        rebuildWaterfallViewport();
    }
    recordKiwiSdrWaterfallFrame();
    if (PerfTelemetry::instance().enabled()) {
        PerfTelemetry::instance().recordWaterfallFallbackRows(1);
    }
}

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

    // Enable alpha blending for overlay compositing.
    // The overlay textures (m_overlayStatic / m_overlayBg) are painted into
    // QImage::Format_RGBA8888_Premultiplied and the overlay.frag shader emits
    // the texel as-is, so the source is PREMULTIPLIED. Premultiplied
    // compositing wants srcColor = One (the RGB already carries × alpha);
    // using SrcAlpha here double-multiplies the texel by alpha (color × α²),
    // which crushed the passband fill (α=35/255 → 1.9% instead of 13.7%) below
    // the visible floor while the brighter edge lines (α=130/255) survived.
    // See issue #3294. NOTE: keep SrcAlpha on the FFT fill/line pipelines —
    // those source from non-premultiplied baked vertex colors.
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;   // premultiplied source
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

    // Background-image layer — parallel texture + SRB so the same overlay
    // pipeline can paint a separate quad BEFORE the FFT pass.  The image
    // itself is built by the static-overlay paint pass below.
    m_bgGpuTex = r->newTexture(QRhiTexture::RGBA8, QSize(pw, ph));
    m_bgGpuTex->create();
    m_bgSrb = r->newShaderResourceBindings();
    m_bgSrb->setBindings({
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_bgGpuTex, m_ovSampler),
    });
    m_bgSrb->create();
    m_overlayBg = QImage(pw, ph, QImage::Format_RGBA8888_Premultiplied);
    m_overlayBg.setDevicePixelRatio(dpr);
    m_overlayBg.fill(Qt::transparent);

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
        if (PerfTelemetry::instance().enabled())
            PerfTelemetry::instance().recordGpuUpload(PerfTelemetry::GpuUploadKind::WaterfallFull);
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
            if (!anyDragActive()) {
                Qt::CursorShape cursorShape = Qt::ArrowCursor;
                if (sliceCursorShapeAt(localPos, cursorShape)) {
                    setSpectrumCursor(cursorShape);
                    setVfoCursorOverride(cursorShape);
                } else if (spectrumDefaultsToCrosshairAt(localPos)) {
                    clearVfoCursorOverride();
                    setSpectrumCursor(Qt::CrossCursor);
                }
            }
        } else if (m_cursorPos.x() >= 0 || m_hoveredTnfId >= 0 || m_tuneGuideVisible) {
            // Mouse left the widget without a leaveEvent
            updateTrackedCursorState(QPoint(-1, -1), false);
        }
    }

    QRhi* r = rhi();
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= freqScaleH() + DIVIDER_H + 2) return;
    const bool perfEnabled = PerfTelemetry::instance().enabled();
    const qint64 perfStartNs = perfEnabled ? PerfTelemetry::nowNs() : 0;

    const int chromeH = freqScaleH() + DIVIDER_H;
    const int contentH = h - chromeH;
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int wfY = specH + DIVIDER_H + freqScaleH();
    const int wfH = h - wfY;
    const QRect specRect(0, 0, w, specH);
    const QRect wfRect(0, wfY, w, wfH);

    // Detect display state changes that may bypass markOverlayDirty()
    {
        if (m_centerMhz != m_lastDetectCenter || m_bandwidthMhz != m_lastDetectBw ||
            m_refLevel != m_lastDetectRef || m_dynamicRange != m_lastDetectDyn ||
            m_spectrumFrac != m_lastDetectFrac ||
            m_wnbActive != m_lastDetectWnb ||
            m_wnbUpdating != m_lastDetectWnbUpdating ||
            m_rfGainValue != m_lastDetectRfGain ||
            m_wideActive != m_lastDetectWide) {
            markOverlayDirty();
            m_lastDetectCenter = m_centerMhz; m_lastDetectBw = m_bandwidthMhz;
            m_lastDetectRef = m_refLevel; m_lastDetectDyn = m_dynamicRange;
            m_lastDetectFrac = m_spectrumFrac;
            m_lastDetectWnb = m_wnbActive;
            m_lastDetectWnbUpdating = m_wnbUpdating;
            m_lastDetectRfGain = m_rfGainValue;
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
            if (perfEnabled)
                PerfTelemetry::instance().recordGpuUpload(PerfTelemetry::GpuUploadKind::WaterfallFull);
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
                if (perfEnabled) {
                    PerfTelemetry::instance().recordGpuUpload(
                        PerfTelemetry::GpuUploadKind::WaterfallIncremental);
                }
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
            // Background layer mirrors the resize so its texture stays the
            // same pixel size as the framebuffer.
            m_overlayBg = QImage(pw, ph, QImage::Format_RGBA8888_Premultiplied);
            m_overlayBg.setDevicePixelRatio(dpr);
            m_overlayBg.fill(Qt::transparent);
            m_bgGpuTex->setPixelSize(QSize(pw, ph));
            m_bgGpuTex->create();
            m_bgSrb->setBindings({
                QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_bgGpuTex, m_ovSampler),
            });
            m_bgSrb->create();
            m_overlayStaticDirty = true;
        }

        // Background-image layer — kept separate from the static overlay so
        // it can render BELOW the FFT trace (parity with software paint).
        // Rebuilt whenever the static overlay is rebuilt, since markOverlayDirty
        // also fires when m_bgImage, m_bgOpacity, or m_bgFillColor change.
        //
        // Composition (bottom → top):
        //     m_bgFillColor (full opacity)
        //     m_bgImage     (opacity = 1 - m_bgOpacity/100, lets fill bleed through)
        if (m_overlayStaticDirty) {
            m_overlayBg.fill(Qt::transparent);
            QPainter bp(&m_overlayBg);
            bp.setRenderHint(QPainter::Antialiasing, false);
            bp.fillRect(specRect, m_bgFillColor);
            if (!m_leanMode && !m_bgImage.isNull()) {
                if (m_bgScaledSize != specRect.size()) {
                    QImage expanded = m_bgImage.scaled(specRect.size(),
                        Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                    int cx = (expanded.width()  - specRect.width())  / 2;
                    int cy = (expanded.height() - specRect.height()) / 2;
                    m_bgScaled = expanded.copy(cx, cy, specRect.width(), specRect.height());
                    m_bgScaledSize = specRect.size();
                }
                bp.setOpacity(1.0 - m_bgOpacity / 100.0);
                bp.drawImage(specRect.topLeft(), m_bgScaled);
            }
            // #3606: the grid lives in the background layer so it composites
            // BELOW the FFT trace (GPU/software parity) -- the rectangular grid
            // cells must sit behind the signal peaks, not paint over them.
            // Reset opacity first: the bg-image branch above leaves bp at
            // (1 - m_bgOpacity/100), and drawGrid sets its own pen but not
            // opacity, so the grid must not inherit the image opacity (review @NF0T).
            bp.setOpacity(1.0);
            drawGrid(bp, specRect);
            m_overlayBgNeedsUpload = true;
        }

        // Static overlay: only repaint when state changes (markOverlayDirty).
        if (m_overlayStaticDirty) {
            m_overlayStatic.fill(Qt::transparent);
            QPainter p(&m_overlayStatic);
            p.setRenderHint(QPainter::Antialiasing, false);

            // #3606: grid moved to the background layer (composites below the
            // FFT trace). The static overlay composites ABOVE the trace, so only
            // markers/scales/band-plan that belong on top of the peaks stay here.
            if (m_bandPlanFontSize > 0)
                drawBandPlan(p, specRect);

            // Divider bar
            p.fillRect(0, specH, w, DIVIDER_H, AetherSDR::ThemeManager::instance().color("color.background.2"));

            drawFreqScale(p, QRect(0, specH + DIVIDER_H, w, freqScaleH()));
            drawTnfMarkers(p, specRect);
            if (m_showSpots || m_showSHistory)
                drawSpotMarkers(p, specRect);
            drawSwrSweep(p, specRect);
            drawSliceMarkers(p, specRect, wfRect);
            drawOffScreenSlices(p, specRect);

            drawAutoSqlFloor(p, specRect);

            // ── Squelch threshold line (solid yellow) ────────────────────
            // Drawn at the radio's actual gate position using the fixed absolute
            // dBm scale: dBm = -160 + squelch_level. Empirically verified on
            // FLEX-8600 fw 4.1.5; independent of refLevel/dynamicRange so the
            // line stays correct regardless of zoom or display range changes.
            if (m_squelchLineVisible && m_squelchLevel > 0) {
                constexpr float kSqlMinDbm = -160.0f;
                const float squelchDbm = kSqlMinDbm + static_cast<float>(m_squelchLevel);
                const float norm = (m_refLevel - squelchDbm) / m_dynamicRange;
                const int sy = specRect.top()
                    + static_cast<int>(std::clamp(norm, 0.0f, 1.0f) * specRect.height());
                p.setPen(QPen(AetherSDR::theme::withAlpha("color.accent.warning", 220), 1));
                p.drawLine(specRect.left(), sy, specRect.right(), sy);
                QFont f = p.font();
                f.setPointSize(8);
                f.setBold(true);
                p.setFont(f);
                p.setPen(AetherSDR::theme::withAlpha("color.accent.warning", 220));
                const QString lbl = QString("SQL %1").arg(m_squelchLevel);
                p.drawText(4, sy - 2, lbl);
            }

            // WNB / RF gain / Prop forecast indicators (top-right of spectrum)
            {
                const bool showProp = m_propForecastVisible
                    && m_propKIndex >= 0
                    && m_propAIndex >= 0
                    && m_propSfi > 0;
                if (m_wnbActive || m_rfGainValue != 0 || showProp || m_wideActive) {
                    QFont indFont(p.font().family(), 14, QFont::Bold);
                    p.setFont(indFont);
                    const QColor indicatorColor(0xc8, 0xd8, 0xe8, 180);
                    const QColor wnbDimColor(0xc8, 0xd8, 0xe8, 84);
                    const QFontMetrics fm(indFont);
                    const int y = specRect.top() + fm.ascent() + 4;
                    const int rightEdge = specRect.right() - DBM_STRIP_W - 8;
                    int x = rightEdge;
                    int leftEdge = rightEdge;
                    auto drawSegment = [&](const QString& text, const QColor& color) {
                        const int textWidth = fm.horizontalAdvance(text);
                        x -= textWidth;
                        leftEdge = x;
                        p.setPen(color);
                        p.drawText(x, y, text);
                        x -= 10;
                    };

                    if (m_wideActive) {
                        drawSegment(QStringLiteral("WIDE"), indicatorColor);
                    }
                    if (m_rfGainValue != 0) {
                        drawSegment(
                            QStringLiteral("%1%2 dB")
                                .arg(m_rfGainValue > 0 ? "+" : "")
                                .arg(m_rfGainValue),
                            indicatorColor);
                    }
                    if (m_wnbActive) {
                        drawSegment(QStringLiteral("WNB"),
                                    m_wnbUpdating ? wnbDimColor : indicatorColor);
                    }
                    if (showProp) {
                        const QString propText = QString("K%1  A%2  SFI %3")
                            .arg(m_propKIndex, 0, 'f', 2)
                            .arg(m_propAIndex)
                            .arg(m_propSfi);
                        const int propW = fm.horizontalAdvance(propText);
                        x -= propW;
                        leftEdge = x;
                        p.setPen(indicatorColor);
                        p.drawText(x, y, propText);
                        m_propClickRect = QRect(x, y - fm.ascent(), propW, fm.height());
                    } else {
                        m_propClickRect = QRect();
                    }

                    // Bounding rect of the full strip — used to suppress
                    // single-click-to-tune when clicking on these indicators (#1564).
                    m_indicatorStripRect = QRect(leftEdge, y - fm.ascent(),
                                                 rightEdge - leftEdge,
                                                 fm.height());
                } else {
                    m_indicatorStripRect = QRect();
                }

                // MQTT device status overlay (#699)
                if (!m_mqttDisplayValues.isEmpty()) {
                    QFont mqttFont(p.font().family(), 12, QFont::Bold);
                    p.setFont(mqttFont);
                    p.setPen(AetherSDR::theme::withAlpha("color.accent.bright", 200));
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
                p.fillRect(lx, ly, tw, th, AetherSDR::theme::withAlpha("color.background.0", 200));
                p.setPen(AetherSDR::ThemeManager::instance().color("color.text.primary"));
                p.drawText(lx + 4, ly + fm.ascent() + 2, label);
            }

            // Tune guide overlay (vertical line + frequency label)
            if (m_showTuneGuides && m_hoveredTnfId < 0 && m_tuneGuideVisible
                && m_cursorPos.x() >= 0 && m_cursorPos.y() >= 0) {
                const int cx = m_cursorPos.x();
                p.setPen(QPen(AetherSDR::ThemeManager::instance().color("color.text.label"), 1));
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
                p.fillRect(lx, ly, tw, th, AetherSDR::theme::withAlpha("color.background.0", 200));
                p.setPen(AetherSDR::ThemeManager::instance().color("color.text.primary"));
                p.drawText(lx + 4, ly + fm.ascent() + 2, label);
            }

            drawConnectionAnimation(p, specRect);
            drawKiwiSdrConnectionOverlay(
                p, QRect(0, 0, qMax(0, w - DBM_STRIP_W), h));
            drawDbmScale(p, specRect);
            drawTimeScale(p, wfRect);

            m_overlayStaticDirty = false;
            m_overlayNeedsUpload = true;
        }

        // Upload overlay texture only when content changed
        if (m_overlayNeedsUpload) {
            QRhiTextureSubresourceUploadDescription ovDesc(m_overlayStatic);
            batch->uploadTexture(m_ovGpuTex, QRhiTextureUploadEntry(0, 0, ovDesc));
            if (perfEnabled)
                PerfTelemetry::instance().recordGpuUpload(PerfTelemetry::GpuUploadKind::Overlay);
            m_overlayNeedsUpload = false;
        }
        if (m_overlayBgNeedsUpload) {
            QRhiTextureSubresourceUploadDescription bgDesc(m_overlayBg);
            batch->uploadTexture(m_bgGpuTex, QRhiTextureUploadEntry(0, 0, bgDesc));
            if (perfEnabled)
                PerfTelemetry::instance().recordGpuUpload(PerfTelemetry::GpuUploadKind::Overlay);
            m_overlayBgNeedsUpload = false;
        }

        // Generate FFT spectrum vertices with baked colors
        const QVector<float>& fftBins = displaySpectrumBins();
        if (!fftBins.isEmpty() && m_fftLineVbo && m_fftFillVbo) {
            const int n = qMin(fftBins.size(), kMaxFftBins);
            const float minDbm = m_refLevel - m_dynamicRange;
            const float maxDbm = m_refLevel;
            const float range = maxDbm - minDbm;
            const float yBot = -1.0f;
            const float yTop = 1.0f;

            // Colors from settings
            const float fr = m_fftFillColor.redF();
            const float fg = m_fftFillColor.greenF();
            const float fb = m_fftFillColor.blueF();
            const float fa = m_leanMode ? 0.0f : m_fftFillAlpha;

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
                float t = qBound(0.0f, (fftBins[i] - minDbm) / range, 1.0f);
                pts[i].y = yBot + t * (yTop - yBot);
            }

            // Compute the perpendicular normal in pixel space so the line
            // width is a true pixel measurement regardless of the spectrum
            // viewport's aspect ratio. The trace is predominantly horizontal,
            // so the normal almost always points along Y; using a single
            // NDC half-width based on width() collapses the offset to a
            // sub-pixel value once specH is much smaller than width().
            const float wPx = static_cast<float>(qMax(1, width()));
            const float hPx = static_cast<float>(qMax(1, specH));

            for (int i = 0; i < n; ++i) {
                float t = qBound(0.0f, (fftBins[i] - minDbm) / range, 1.0f);

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
                const float dxPx = dx * wPx * 0.5f;
                const float dyPx = dy * hPx * 0.5f;
                float lenPx = std::sqrt(dxPx * dxPx + dyPx * dyPx);
                if (lenPx < 1e-8f) lenPx = 1e-8f;
                const float nxPx = -dyPx / lenPx * m_fftLineWidth;
                const float nyPx =  dxPx / lenPx * m_fftLineWidth;
                const float nx = nxPx * 2.0f / wPx;
                const float ny = nyPx * 2.0f / hPx;

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

    // Draw background-image quad BELOW the FFT — same overlay pipeline,
    // different SRB.  Sits between the waterfall pass and the FFT pass so
    // the FFT trace ends up on top of any user-set bg image, matching the
    // software paint path's ordering.
    if (m_ovPipeline && m_bgSrb) {
        cb->setGraphicsPipeline(m_ovPipeline);
        cb->setShaderResources(m_bgSrb);
        cb->setViewport({0, 0,
            static_cast<float>(outputSize.width()),
            static_cast<float>(outputSize.height())});
        const QRhiCommandBuffer::VertexInput vbuf(m_ovVbo, 0);
        cb->setVertexInput(0, 1, &vbuf);
        cb->draw(4);
    }

    // Draw FFT spectrum — viewport restricted to spectrum rect
    const QVector<float>& fftDrawBins = displaySpectrumBins();
    if (m_fftFillPipeline && m_fftLinePipeline && !fftDrawBins.isEmpty()) {
        const int n = qMin(fftDrawBins.size(), kMaxFftBins);
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
            // Split partners stay locked to opposite sides regardless of
            // edge proximity (#2663).  Flipping a partner when near an
            // edge would collapse both panels onto the same side and the
            // RX/TX panels would visually overlap; the panadapter is the
            // user's spatial frame and the side-locking is the whole point
            // of the split affordance.  The outward-facing panel may clip
            // the pan edge — the user pans toward center to read it.
            int leftIdx  = (vfos[i].x <= vfos[pi].x) ? i : pi;
            int rightIdx = (leftIdx == i) ? pi : i;
            dirMap[vfos[leftIdx].sliceId]  = VfoWidget::LockLeft;
            dirMap[vfos[rightIdx].sliceId] = VfoWidget::LockRight;
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

    if (m_interlockNotificationLabel && m_interlockNotificationLabel->isVisible())
        m_interlockNotificationLabel->raise();

    if (perfEnabled) {
        PerfTelemetry::instance().recordRender(
            static_cast<double>(PerfTelemetry::nowNs() - perfStartNs) / 1000000.0);
    }
}

void SpectrumWidget::render(QRhiCommandBuffer* cb)
{
    if (m_shutdownPrepared) {
        return;
    }
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

    // Bg-image layer (added with the FFT-above-bg fix) — same lifecycle
    // as the overlay scaffolding it lives alongside; release in the same
    // teardown sweep.
    delete m_bgSrb;          m_bgSrb = nullptr;
    delete m_bgGpuTex;       m_bgGpuTex = nullptr;

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
    if (width() <= 0 || height() <= freqScaleH() + DIVIDER_H + 2) return;

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

    const int chromeH  = freqScaleH() + DIVIDER_H;
    const int contentH = height() - chromeH;
    const int specH    = static_cast<int>(contentH * m_spectrumFrac);
    const int wfH      = contentH - specH;

    const int divY     = specH;
    const int scaleY   = specH + DIVIDER_H;
    const int wfY      = scaleY + freqScaleH();

    const QRect specRect (0, 0,       width(), specH);
    const QRect divRect  (0, divY,    width(), DIVIDER_H);
    const QRect scaleRect(0, scaleY,  width(), freqScaleH());
    const QRect wfRect   (0, wfY,     width(), wfH);

    {
        // Software fallback: full QPainter rendering.  Composition z-order:
        //   bottom: m_bgFillColor (user-pickable, default #0a0a14)
        //   middle: bg image at opacity (1 - m_bgOpacity/100) so the fill
        //           bleeds through as the slider moves toward 100
        //   above:  grid → FFT trace → band plan → markers
        p.fillRect(specRect, m_bgFillColor);
        if (!m_leanMode && !m_bgImage.isNull()) {
            if (m_bgScaledSize != specRect.size()) {
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
        drawSpectrum(p, specRect);
        if (m_bandPlanFontSize > 0) drawBandPlan(p, specRect);

        p.fillRect(divRect, AetherSDR::ThemeManager::instance().color("color.background.1"));
        p.setPen(QColor(m_draggingDivider ? 0x00b4d8 : 0x304050));
        p.drawLine(divRect.left(), divRect.center().y(), divRect.right(), divRect.center().y());

        drawFreqScale(p, scaleRect);
        drawWaterfall(p, wfRect);
        drawTnfMarkers(p, specRect);
        if (m_showSpots || m_showSHistory) drawSpotMarkers(p, specRect);
        drawSwrSweep(p, specRect);
        drawSliceMarkers(p, specRect, wfRect);
        drawOffScreenSlices(p, specRect);

        drawAutoSqlFloor(p, specRect);

        // ── Squelch threshold line (solid yellow) ────────────────────
        if (m_squelchLineVisible && m_squelchLevel > 0) {
            constexpr float kSqlMinDbm = -160.0f;
            const float squelchDbm = kSqlMinDbm + static_cast<float>(m_squelchLevel);
            const float norm = (m_refLevel - squelchDbm) / m_dynamicRange;
            const int sy = specRect.top()
                + static_cast<int>(std::clamp(norm, 0.0f, 1.0f) * specRect.height());
            p.setPen(QPen(AetherSDR::theme::withAlpha("color.accent.warning", 220), 1));
            p.drawLine(specRect.left(), sy, specRect.right(), sy);
            QFont f = p.font(); f.setPointSize(8); f.setBold(true); p.setFont(f);
            p.setPen(AetherSDR::theme::withAlpha("color.accent.warning", 220));
            p.drawText(4, sy - 2, QString("SQL %1").arg(m_squelchLevel));
        }

        drawConnectionAnimation(p, specRect);
        drawKiwiSdrConnectionOverlay(
            p, QRect(0, 0, qMax(0, width() - DBM_STRIP_W), height()));
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

            // Left partner flies left, right partner flies right.
            // Split partners stay locked to opposite sides regardless of
            // edge proximity (#2663).  Flipping a partner when near an
            // edge would collapse both panels onto the same side and the
            // RX/TX panels would visually overlap.  The outward-facing
            // panel may clip the pan edge — the user pans toward center
            // to read it.  Mirrors the GPU path block above.
            int leftIdx  = (vfos[i].x <= vfos[pi].x) ? i : pi;
            int rightIdx = (leftIdx == i) ? pi : i;
            dirMap[vfos[leftIdx].sliceId]  = VfoWidget::LockLeft;
            dirMap[vfos[rightIdx].sliceId] = VfoWidget::LockRight;
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
        if (m_interlockNotificationLabel && m_interlockNotificationLabel->isVisible())
            m_interlockNotificationLabel->raise();
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
            const QColor indicatorColor(255, 255, 255, 84);
            const QColor wnbActiveColor(0xc8, 0xd8, 0xe8, 180);
            const QColor wnbDimColor(0xc8, 0xd8, 0xe8, 84);

            const QFontMetrics fm(indFont);
            const int rightEdge = specRect.right() - DBM_STRIP_W - 6;
            const int topY = specRect.top() + fm.ascent() + 2;

            int x = rightEdge;
            int leftEdge = rightEdge;
            auto drawSegment = [&](const QString& text, const QColor& color) {
                const int textWidth = fm.horizontalAdvance(text);
                x -= textWidth;
                leftEdge = x;
                p.setPen(color);
                p.drawText(x, topY, text);
                x -= 10;
            };

            // WIDE (rightmost)
            if (m_wideActive) {
                drawSegment(QStringLiteral("WIDE"), indicatorColor);
            }

            // RF Gain (to the left of WIDE)
            if (m_rfGainValue != 0) {
                const QString gainStr = (m_rfGainValue > 0)
                    ? QString("+%1dB").arg(m_rfGainValue)
                    : QString("%1dB").arg(m_rfGainValue);
                drawSegment(gainStr, indicatorColor);
            }

            // WNB (to the left of RF Gain)
            if (m_wnbActive) {
                drawSegment(QStringLiteral("WNB"),
                            m_wnbUpdating ? wnbDimColor : wnbActiveColor);
            }

            // Prop forecast (leftmost: "K3  A12  SFI 110")
            if (showProp) {
                const QString propStr = QString("K%1  A%2  SFI %3")
                    .arg(m_propKIndex, 0, 'f', 2)
                    .arg(m_propAIndex)
                    .arg(m_propSfi);
                const int pw = fm.horizontalAdvance(propStr);
                x -= pw;
                leftEdge = x;
                p.setPen(indicatorColor);
                p.drawText(x, topY, propStr);
                m_propClickRect = QRect(x, topY - fm.ascent(), pw, fm.height());
            } else {
                m_propClickRect = QRect();
            }

            // Bounding rect of the full strip (prop + WNB + RF Gain + WIDE) —
            // used to suppress single-click-to-tune within (#1564).
            m_indicatorStripRect = QRect(leftEdge, topY - fm.ascent(),
                                         rightEdge - leftEdge, fm.height());
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
        p.fillRect(lx, ly, tw, th, AetherSDR::theme::withAlpha("color.background.0", 200));
        p.setPen(AetherSDR::ThemeManager::instance().color("color.text.primary"));
        p.drawText(lx + 4, ly + fm.ascent() + 2, label);
    }

    // ── Tune guide overlay (vertical line + frequency label) ──────────────
    if (m_showTuneGuides && m_hoveredTnfId < 0 && m_tuneGuideVisible
        && m_cursorPos.x() >= 0 && m_cursorPos.y() >= 0) {
        const int cx = m_cursorPos.x();
        p.setPen(QPen(AetherSDR::ThemeManager::instance().color("color.text.label"), 1));
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
        p.fillRect(lx, ly, tw, th, AetherSDR::theme::withAlpha("color.background.0", 200));
        p.setPen(AetherSDR::ThemeManager::instance().color("color.text.primary"));
        p.drawText(lx + 4, ly + fm.ascent() + 2, label);
    }

    drawDbmScale(p, specRect);
    drawTimeScale(p, wfRect);

    if (PerfTelemetry::instance().enabled()) {
        PerfTelemetry::instance().recordRender(
            static_cast<double>(frameTimer.nsecsElapsed()) / 1000000.0);
    }
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
    p.setPen(QPen(AetherSDR::ThemeManager::instance().color("color.background.1"), 1, Qt::DotLine));
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

    p.setPen(QPen(AetherSDR::ThemeManager::instance().color("color.background.1"), 1, Qt::DotLine));
    for (double f = firstLine; f <= endMhz; f += gridStep)
        p.drawLine(mhzToX(f), r.top(), mhzToX(f), r.bottom());
}

// ─── Spectrum line ────────────────────────────────────────────────────────────

void SpectrumWidget::drawSpectrum(QPainter& p, const QRect& r)
{
    const QVector<float>& fftBins = displaySpectrumBins();
    if (fftBins.isEmpty()) {
        p.setPen(AetherSDR::ThemeManager::instance().color("color.accent.dim"));
        p.drawText(r, Qt::AlignCenter, "No panadapter data — waiting for radio stream");
        return;
    }

    const int w = r.width();
    const int h = r.height();
    const int n = fftBins.size();

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
        const float dbm  = fftBins[i];
        const float norm = qBound(0.0f, (m_refLevel - dbm) / m_dynamicRange, 1.0f);
        pts[i].x = r.left() + static_cast<int>(static_cast<float>(i) / n * w);
        pts[i].y = r.top()  + qMin(static_cast<int>(norm * h), h - 1);
        pts[i].t = 1.0f - norm;  // 0=noise floor, 1=strong signal
    }

    p.setRenderHint(QPainter::Antialiasing, true);

    // Mirror GPU-path semantics (renderGpuFrame, ~L5065): width 0 = "Off"
    // (line draw skipped); otherwise honor the slider with a cosmetic pen so
    // the requested pixel width survives high-DPI on Windows.
    const bool drawLine = m_fftLineWidth > 0.0f;
    QPen linePen;
    linePen.setCosmetic(true);
    linePen.setWidthF(m_fftLineWidth);

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
            const float swFillAlpha = m_leanMode ? 0.0f : m_fftFillAlpha;
            top.setAlphaF(swFillAlpha * 0.3f);
            QColor bot(0, 0, 77, static_cast<int>(255 * swFillAlpha));
            QLinearGradient grad(0, qMin(pts[i].y, pts[i+1].y), 0, bottom);
            grad.setColorAt(0.0, top);
            grad.setColorAt(1.0, bot);
            p.setPen(Qt::NoPen);
            p.setBrush(grad);
            p.drawPolygon(trapezoid);
        }

        // Heat map line: per-segment coloring
        if (drawLine) {
            for (int i = 0; i < n - 1; ++i) {
                float avgT = (pts[i].t + pts[i+1].t) * 0.5f;
                linePen.setColor(heatColor(avgT));
                p.setPen(linePen);
                p.drawLine(pts[i].x, pts[i].y, pts[i+1].x, pts[i+1].y);
            }
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
        if (drawLine) {
            linePen.setColor(m_fftFillColor);
            p.setPen(linePen);
            p.drawPath(linePath);
        }
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
        p.setPen(AetherSDR::theme::withAlpha("color.background.0", 200));
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

    // Draw single-frequency spot markers (white circles) — issue #3339
    if (m_bandPlanShowSpots) {
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
}

// ─── TNF markers ────────────────────────────────────────────────────────────

void SpectrumWidget::setTnfMarkers(const QVector<TnfMarker>& markers)
{
    m_tnfMarkers = markers;
    markOverlayDirty();
}

void SpectrumWidget::setSpotMarkers(const QVector<SpotMarker>& markers)
{
    // Prune confirmed-spot cache to only entries still present in the new list.
    if (!m_spotConfirmedMs.isEmpty()) {
        QHash<QString, qint64> pruned;
        pruned.reserve(markers.size());
        for (const auto& m : markers) {
            const QString key = m.callsign + QChar('@') +
                                QString::number(qRound(m.freqMhz * 1000.0));
            auto it = m_spotConfirmedMs.find(key);
            if (it != m_spotConfirmedMs.end())
                pruned.insert(key, it.value());
        }
        m_spotConfirmedMs = std::move(pruned);
    }

    const bool visualChange = !spotMarkersVisuallyEqual(m_spotMarkers, markers);
    m_spotMarkers = markers;
    if (!visualChange) {
        return;
    }
    markOverlayDirty();
}

void SpectrumWidget::setSHistoryMarkers(const QVector<SpotMarker>& markers)
{
    const bool visualChange = !spotMarkersVisuallyEqual(m_sHistoryMarkers, markers);
    m_sHistoryMarkers = markers;
    if (!visualChange) {
        return;
    }
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
    return tnf.permanent ? AetherSDR::ThemeManager::instance().color("color.accent.success") : AetherSDR::ThemeManager::instance().color("color.accent.warning");
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

static QString spotMarkerTooltip(const SpectrumWidget::SpotMarker& sm)
{
    QString tip = QString("<b>%1</b>  %2 MHz").arg(sm.callsign).arg(sm.freqMhz, 0, 'f', 4);
    if (!sm.source.isEmpty())
        tip += QString("<br>Source: %1").arg(sm.source);
    if (!sm.spotterCallsign.isEmpty())
        tip += QString("<br>Spotter: %1").arg(sm.spotterCallsign);
    if (!sm.comment.isEmpty())
        tip += QString("<br>%1").arg(sm.comment);
    if (sm.timestampMs > 0)
        tip += QString("<br>Spotted: %1 UTC").arg(
            QDateTime::fromMSecsSinceEpoch(sm.timestampMs, QTimeZone::utc()).toString("yyyy-MM-dd HH:mm:ss"));
    return tip;
}

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

    // Smart Spot Filter: cache S-History voice frequencies for O(n) per spot.
    // Hoist the current timestamp once — used for warmup check and per-spot
    // confirmation cache, avoiding repeated syscalls at frame rate.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 smartFilterWarmupMs = static_cast<qint64>(m_smartSpotFilterDelayS) * 1000;
    const bool smartFilterReady = m_smartSpotFilter &&
        (nowMs - m_smartSpotFilterEnabledMs) >= smartFilterWarmupMs;
    QVector<double> sHistVoiceFreqs;
    if (smartFilterReady) {
        for (const auto& sh : m_sHistoryMarkers) {
            if (sh.source != QStringLiteral("QRM"))
                sHistVoiceFreqs.append(sh.freqMhz);
        }
    }

    QFont spotFont = p.font();
    spotFont.setPixelSize(m_spotFontSize);
    spotFont.setBold(true);
    p.setFont(spotFont);
    const QFontMetrics fm(spotFont);

    // Label-geometry constants.  Extracted from inline literals so the
    // padding and inter-label spacing can be tuned in one place (#2622).
    constexpr int kSpotLabelHPad = 6;   // horizontal padding inside each label
    constexpr int kSpotLabelVPad = 2;   // vertical padding inside each label
    constexpr int kSpotLabelGap  = 2;   // inter-label gap (collision only — not painted)

    // Starting Y position based on percentage setting
    const int startY = specRect.top() + specRect.height() * m_spotStartPct / 100;
    const int th = fm.height() + kSpotLabelVPad;
    // Each label occupies th + kSpotLabelGap of vertical space once the
    // collision-nudge gap is included, so m_spotMaxLevels keeps meaning
    // "this many labels stack" rather than shrinking by the gap factor.
    const int maxBottom = startY + (th + kSpotLabelGap) * m_spotMaxLevels;

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
        const int tw = fm.horizontalAdvance(label) + kSpotLabelHPad;

        // Start at configured position, nudge down to avoid overlap.
        // Re-scan from the start after each nudge to handle cases where
        // nudging past label A lands on top of label B.
        QRect labelRect(x - tw / 2, startY, tw, th);
        bool collision = true;
        while (collision) {
            collision = false;
            for (const auto& r : placed) {
                if (labelRect.intersects(r.adjusted(0, -kSpotLabelGap, 0, kSpotLabelGap))) {
                    labelRect.moveTop(r.bottom() + kSpotLabelGap + 1);
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

        // Determine draw opacity.
        // QRM markers: always 30%.  Smart-filter unmatched/expired spots: user-configurable (default 20%).  Normal: 100%.
        const bool isQrm    = (spot.source == QStringLiteral("QRM"));
        const bool isMemory = (spot.source == QStringLiteral("Memory"));
        bool dimForFilter = false;
        if (!isQrm && !isMemory && smartFilterReady && !sHistVoiceFreqs.isEmpty()) {
            // Only apply to voice/SSB spots; leave CW/digital at full opacity.
            const QString mu = spot.mode.toUpper();
            const bool isDigital = mu.contains("CW")   || mu.contains("FT")  ||
                                   mu.contains("JS8")  || mu.contains("PSK") ||
                                   mu.contains("RTTY") || mu.contains("WSPR");
            if (!isDigital) {
                // ±N Hz of detected signal start — user-configurable via the
                // "Filter Match Window" slider in SpotHub Display. (#2609)
                const double matchMhz = m_smartSpotFilterMatchHz / 1.0e6;
                constexpr qint64 kConfirmGraceMs = 120000; // 2 min grace after last confirmation
                const QString spotKey = spot.callsign + QChar('@') +
                                        QString::number(qRound(spot.freqMhz * 1000.0));
                bool matched = false;
                for (double shFreq : sHistVoiceFreqs) {
                    if (std::abs(spot.freqMhz - shFreq) <= matchMhz) {
                        matched = true;
                        break;
                    }
                }
                if (matched) {
                    m_spotConfirmedMs[spotKey] = nowMs;
                } else {
                    const qint64 lastConfirmed = m_spotConfirmedMs.value(spotKey, 0);
                    if (nowMs - lastConfirmed < kConfirmGraceMs)
                        matched = true; // still within grace period
                }
                dimForFilter = !matched;
            }
        }
        const double dimOpacity = 1.0 - m_smartSpotFilterOpacity / 100.0;
        const double drawOpacity = isQrm ? 0.3 : (dimForFilter ? dimOpacity : 1.0);
        if (drawOpacity < 1.0) p.setOpacity(drawOpacity);

        // Draw vertical tick line from bottom of spectrum up to the label
        if (m_spotShowLines) {
            p.setPen(QPen(QColor(col.red(), col.green(), col.blue(), 120), 1, Qt::DotLine));
            p.drawLine(x, specRect.bottom(), x, labelRect.bottom());
        }

        placed.append(labelRect);
        m_spotClickRects.append({labelRect, spot.freqMhz, mIdx, spot.callsign});

        // Background pill — local Override Background color wins when on
        // (#768).  When off, fall through to the protocol-supplied
        // `background_color` field if the spot carries one (#2550).  The
        // #AARRGGBB form encodes alpha directly so no separate opacity
        // multiplier is applied — the upstream tool's choice is honored.
        // Opacity for QRM markers / smart-filter dimming is set above
        // (drawOpacity block) so the pill participates in the same
        // opacity as the label.
        if (m_spotOverrideBg) {
            int bgAlpha = m_spotBgOpacity * 255 / 100;
            QColor bgCol = m_spotBgColor;
            bgCol.setAlpha(bgAlpha);
            p.setPen(Qt::NoPen);
            p.setBrush(bgCol);
            p.drawRoundedRect(labelRect, 3, 3);
        } else if (!spot.backgroundColor.isEmpty()
                   && spot.backgroundColor.startsWith('#')) {
            QColor bgCol(spot.backgroundColor);
            if (bgCol.isValid()) {
                p.setPen(Qt::NoPen);
                p.setBrush(bgCol);
                p.drawRoundedRect(labelRect, 3, 3);
            }
        }

        // Text
        p.setPen(col);
        p.drawText(labelRect, Qt::AlignCenter, label);

        if (drawOpacity < 1.0) p.setOpacity(1.0);
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
            p.setBrush(AetherSDR::theme::withAlpha("color.background.2", 200));
            p.drawRoundedRect(badgeRect, 3, 3);

            p.setPen(AetherSDR::ThemeManager::instance().color("color.accent.warning"));  // amber text
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

    // Re-show tooltip for the hovered spot after every rect rebuild. Without
    // this, the jitter in spot label positions (collision-nudge cascade) causes
    // Qt to hide the tooltip each repaint because the registered rect shifts
    // slightly. Called via QueuedConnection because drawSpotMarkers runs on the
    // render thread; QToolTip must be touched on the main thread. (#2553)
    if (!m_hoveredSpotKey.isEmpty() && !m_tooltipRefreshPending) {
        m_tooltipRefreshPending = true;
        QMetaObject::invokeMethod(this, [this]() {
            m_tooltipRefreshPending = false;
            if (m_hoveredSpotKey.isEmpty()) return;
            const QPoint cursorPos = QCursor::pos();
            for (const auto& hr : m_spotClickRects) {
                const QString key = hr.callsign + QChar('@')
                    + QString::number(qRound(hr.freqMhz * 1000.0));
                if (key != m_hoveredSpotKey) continue;
                if (!hr.rect.contains(mapFromGlobal(cursorPos))) continue;
                if (hr.markerIndex >= 0 && hr.markerIndex < m_spotMarkers.size()) {
                    if (hr.rect != m_lastTooltipRect) {
                        m_lastTooltipRect = hr.rect;
                        QToolTip::showText(cursorPos + QPoint(0, 20),
                                           spotMarkerTooltip(m_spotMarkers[hr.markerIndex]),
                                           this, hr.rect);
                    }
                }
                break;
            }
        }, Qt::QueuedConnection);
    }
}

QRect SpectrumWidget::leftOccludedRect() const
{
    if (m_overlayMenu && m_overlayMenu->isVisible())
        return m_overlayMenu->geometry();
    return {};
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
    p.setBrush(AetherSDR::theme::withAlpha("color.background.0", 190));
    p.drawRoundedRect(plotRect, 3, 3);

    auto fillThresholdBand = [&](float lowSwr, float highSwr, const QColor& color) {
        const float high = qMin(highSwr, maxSwr);
        if (high <= lowSwr)
            return;
        const qreal top = yForSwr(high);
        const qreal bottom = yForSwr(lowSwr);
        p.fillRect(QRectF(plotRect.left(), top, plotRect.width(), bottom - top), color);
    };
    fillThresholdBand(1.0f, 1.5f, AetherSDR::theme::withAlpha("color.accent.success", 28));
    fillThresholdBand(1.5f, 2.0f, AetherSDR::theme::withAlpha("color.accent.warning", 22));
    fillThresholdBand(2.0f, maxSwr, AetherSDR::theme::withAlpha("color.accent.danger", 16));

    p.setPen(QPen(AetherSDR::theme::withAlpha("color.text.secondary", 120), 1, Qt::DotLine));
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
        QPen sweepPen(AetherSDR::theme::withAlpha("color.accent.warning", 235), 2);
        sweepPen.setCapStyle(Qt::RoundCap);
        sweepPen.setJoinStyle(Qt::RoundJoin);
        p.setPen(sweepPen);
        p.drawPolyline(poly);
    }
    p.setPen(QPen(AetherSDR::theme::withAlpha("color.background.0", 220), 1));
    p.setBrush(AetherSDR::theme::withAlpha("color.accent.warning", 240));
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

        QPen endpointPen(AetherSDR::theme::withAlpha("color.accent.warning", 245), 2);
        endpointPen.setCapStyle(Qt::RoundCap);
        p.setPen(endpointPen);
        drawEndpointNotch(visiblePoints.first().pos);
        if (visiblePoints.size() > 1)
            drawEndpointNotch(visiblePoints.last().pos);
    }

    if (bestVisibleIndex >= 0 && bestVisibleIndex < visiblePoints.size()) {
        const QPointF bestPos = visiblePoints[bestVisibleIndex].pos;
        QPen resonancePen(AetherSDR::theme::withAlpha("color.accent.success", 245), 2);
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
    drawBandwidthSpan(bw20, 0, AetherSDR::theme::withAlpha("color.accent.warning", 205));
    drawBandwidthSpan(bw15, 1, AetherSDR::theme::withAlpha("color.accent.success", 225));

    if (m_swrSweepCurrentFreqMhz > 0.0) {
        const int cx = mhzToX(m_swrSweepCurrentFreqMhz);
        if (cx >= plotRect.left() && cx <= plotRect.right()) {
            p.setPen(QPen(AetherSDR::theme::withAlpha("color.accent", 210), 1, Qt::DashLine));
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
    const QRect occluded = leftOccludedRect();
    if (!occluded.isNull()
        && labelRect.intersects(occluded.adjusted(-4, -4, 8, 4))) {
        labelX = occluded.right() + 8;
        const int maxLabelX = qMax(plotRect.left() + 4, plotRect.right() - labelW - 4);
        labelX = qBound(plotRect.left() + 4, labelX, maxLabelX);
        labelRect.moveLeft(labelX);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(AetherSDR::theme::withAlpha("color.background.0", 220));
    p.drawRoundedRect(labelRect, 2, 2);
    p.setPen(AetherSDR::ThemeManager::instance().color("color.accent.warning"));
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

            p.setPen(QPen(AetherSDR::theme::withAlpha("color.accent.warning", 130), 1));
            const qreal notchY = std::clamp(point.pos.y(),
                                            qreal(plotRect.top() + 2),
                                            qreal(plotRect.bottom()));
            const QPointF notchTop(point.pos.x(), notchY);
            const QPointF notchBottom(point.pos.x(),
                                      placeBelow ? valueRect.top() : valueRect.bottom());
            p.drawLine(notchTop, notchBottom);

            p.setPen(Qt::NoPen);
            p.setBrush(AetherSDR::theme::withAlpha("color.background.0", 230));
            p.drawRoundedRect(valueRect, 2, 2);
            p.setPen(AetherSDR::ThemeManager::instance().color("color.accent.warning"));
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
    p.setPen(AetherSDR::theme::withAlpha("color.text.secondary", 180));
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
    AetherSDR::ThemeManager::instance().applyStyleSheet(menu, "QMenu {"
        "  background: {{color.background.0}};"
        "  border: 1px solid #305070;"
        "  padding: 4px;"
        "}"
        "QMenu::item {"
        "  color: {{color.text.primary}};"
        "  padding: 4px 12px;"
        "  font-size: 12px;"
        "}"
        "QMenu::item:selected {"
        "  background: {{color.background.2}};"
        "  color: {{color.accent}};"
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
                // Mirror direct-click path so the radio sees the spot click (#2680, #341)
                emit spotTriggered(spot.index);
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

        const QColor col = sliceColorForOverlay(so);
        // Bandwidth affordances (passband fill + filter edges) render at full
        // brightness so they stay visible on non-active slices (#3484) — but an
        // inactive slice uses the neutral secondary colour instead of the
        // slice's own colour, so it is COLOUR (not brightness) that signals
        // inactive. That keeps the panadapter from making an inactive slice look
        // TX-selectable (the #2389 confusion concern) while fixing the
        // near-invisible passband. The VFO centre line, triangle, and RIT/XIT
        // lines keep `col` (dimmed when inactive) to preserve the focus cue.
        const int colourIdx = SliceLabel::displayColorIndex(so.sliceId, so.perClientLetter);
        const QColor bandCol = so.isActive
            ? SliceColorManager::instance().activeColor(colourIdx)
            : AetherSDR::ThemeManager::instance().color("color.text.secondary");
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
                   QColor(bandCol.red(), bandCol.green(), bandCol.blue(), 35));

        // Filter edge lines — user-hidden via per-slice VFO flag toggle (#1526)
        if (!so.filterEdgesHidden) {
            p.setPen(QPen(QColor(bandCol.red(), bandCol.green(), bandCol.blue(), 130), 1));
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
            p.setPen(QPen(AetherSDR::theme::withAlpha("color.accent.success", 200), 1, Qt::DashLine));
            p.drawLine(markX, specRect.top(), markX, freqLineBottom);

            // Space line — red, dashed
            p.setPen(QPen(AetherSDR::theme::withAlpha("color.accent.danger", 200), 1, Qt::DashLine));
            p.drawLine(spaceX, specRect.top(), spaceX, freqLineBottom);

            // Labels at top
            QFont f = p.font();
            f.setPixelSize(10);
            f.setBold(true);
            p.setFont(f);
            p.setPen(AetherSDR::theme::withAlpha("color.accent.success", 240));
            p.drawText(markX + 2, specRect.top() + 12, "M");
            p.setPen(AetherSDR::theme::withAlpha("color.accent.danger", 240));
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
            QPen xitPen(AetherSDR::theme::withAlpha("color.accent.danger", 180), 1, Qt::DashLine);
            p.setPen(xitPen);
            p.drawLine(xitX, specRect.top(), xitX, wfRect.bottom());
            QFont f = p.font();
            f.setPixelSize(10);
            f.setBold(true);
            p.setFont(f);
            p.setPen(AetherSDR::theme::withAlpha("color.accent.danger", 220));
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

int SpectrumWidget::freqScaleH() const
{
    if (m_freqScaleFontPt <= 8)
        return FREQ_SCALE_H;
    QFont f = font();
    f.setPointSize(m_freqScaleFontPt);
    return std::max(FREQ_SCALE_H, QFontMetrics(f).height() + 6);
}

void SpectrumWidget::drawFreqScale(QPainter& p, const QRect& r)
{
    p.fillRect(r, AetherSDR::ThemeManager::instance().color("color.background.0"));

    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double endMhz   = m_centerMhz + m_bandwidthMhz / 2.0;

    // Grid step — honours user spacing override (#1390)
    const double stepMhz = effectiveGridStepMhz(width());
    const double firstLine = std::ceil(startMhz / stepMhz) * stepMhz;

    QFont f = p.font();
    f.setPointSize(m_freqScaleFontPt);
    p.setFont(f);
    const QFontMetrics fm(f);

    // Decimal places: enough to distinguish labels at this step size
    int decimals;
    if      (stepMhz < 0.0001) decimals = 6;
    else if (stepMhz < 0.001)  decimals = 5;
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
        p.setPen(AetherSDR::ThemeManager::instance().color("color.background.3"));
        p.drawLine(x, r.top(), x, r.top() + 4);

        // Label only every Nth line to prevent overlap
        if (stepIdx % labelEvery != 0) continue;

        const QString label = formatFreqScaleLabel(freq, decimals);
        const int tw = fm.horizontalAdvance(label);
        // Keep labels clear of the right-edge dBm / waterfall-time scale
        // column — at larger label sizes (#3501) a label clamped to the
        // widget edge collides with those figures.
        const int labelMaxRight = width() - DBM_STRIP_W - 2;
        if (x > labelMaxRight) continue;
        const int lx = qBound(0, x - tw / 2, labelMaxRight - tw);

        p.setPen(AetherSDR::ThemeManager::instance().color("color.text.secondary"));
        p.drawText(lx, r.bottom() - 2, label);
    }
}

// ─── dBm scale strip (right edge of FFT area) ────────────────────────────────

void SpectrumWidget::drawDbmScale(QPainter& p, const QRect& specRect)
{
    const int stripX = specRect.right() - DBM_STRIP_W + 1;
    const QRect strip(stripX, specRect.top(), DBM_STRIP_W, specRect.height());

    // Semi-opaque background
    p.fillRect(strip, AetherSDR::theme::withAlpha("color.background.0", 220));

    // Left border line
    p.setPen(AetherSDR::ThemeManager::instance().color("color.background.2"));
    p.drawLine(stripX, specRect.top(), stripX, specRect.bottom());

    // ── Up/Down arrows side by side at top ─────────────────────────────
    const int halfW = DBM_STRIP_W / 2;
    const int upCx  = stripX + halfW / 2;       // left half center
    const int dnCx  = stripX + halfW + halfW / 2; // right half center
    const int arrowTop = specRect.top() + 2;
    const int arrowBot = specRect.top() + DBM_ARROW_H - 2;

    p.setPen(Qt::NoPen);
    p.setBrush(AetherSDR::ThemeManager::instance().color("color.text.secondary"));

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

    auto drawTickLabel = [&](float dbm, int y, int textBaseline) {
        p.setPen(AetherSDR::ThemeManager::instance().color("color.background.3"));
        p.drawLine(stripX, y, stripX + 4, y);

        const QString label = QString::number(static_cast<int>(std::lround(dbm)));
        p.setPen(AetherSDR::ThemeManager::instance().color("color.text.secondary"));
        p.drawText(stripX + 6, textBaseline, label);
    };

    for (float dbm = firstLabel; dbm <= m_refLevel; dbm += stepDb) {
        const float frac = (m_refLevel - dbm) / m_dynamicRange;
        const int y = specRect.top() + static_cast<int>(frac * specRect.height());
        if (y < labelTop || y > specRect.bottom() - 5) continue;

        drawTickLabel(dbm, y, y + fm.ascent() / 2);
    }

    const int bottomY = specRect.bottom();
    if (bottomY >= labelTop) {
        drawTickLabel(bottomDbm, bottomY, bottomY - 2);
    }
}

// ─── Time scale (right edge of waterfall) ─────────────────────────────────────

void SpectrumWidget::drawTimeScale(QPainter& p, const QRect& wfRect)
{
    const QRect strip = waterfallTimeScaleRect(wfRect);
    const int stripX = strip.x();

    // Semi-opaque background
    p.fillRect(strip, AetherSDR::theme::withAlpha("color.background.0", 220));

    // Left border line
    p.setPen(AetherSDR::ThemeManager::instance().color("color.background.2"));
    p.drawLine(stripX, wfRect.top(), stripX, wfRect.bottom());

    const QRect liveRect = waterfallLiveButtonRect(wfRect);
    p.setPen(AetherSDR::ThemeManager::instance().color("color.meter.bar.fill"));
    p.setBrush(m_wfLive ? AetherSDR::ThemeManager::instance().color("color.text.label") : AetherSDR::ThemeManager::instance().color("color.accent.danger"));
    p.drawRoundedRect(liveRect, 3, 3);

    QFont liveFont = p.font();
    liveFont.setPointSize(7);
    liveFont.setBold(true);
    p.setFont(liveFont);
    p.setPen(m_wfLive ? AetherSDR::ThemeManager::instance().color("color.text.primary") : Qt::white);
    p.drawText(liveRect, Qt::AlignCenter, "LIVE");

    const float msPerRow = std::max(1.0f, m_wfMsPerRow);
    const QRect labelRect = strip.adjusted(0, 4, 0, 0);
    const float totalSec = labelRect.height() * msPerRow / 1000.0f;
    if (totalSec <= 0) return;

    QFont f = p.font();
    f.setPointSize(7);
    f.setBold(false);
    p.setFont(f);
    const QFontMetrics fm(f);

    const int minLabelSpacingPx = std::max(18, fm.height() + 4);
    const int maxVisibleLabels =
        std::max(1, labelRect.height() / minLabelSpacingPx);
    const float rawStepSec = totalSec / static_cast<float>(maxVisibleLabels);
    static constexpr float kNiceStepsSec[] = {
        1.0f, 2.0f, 5.0f, 10.0f, 15.0f, 30.0f,
        60.0f, 120.0f, 300.0f, 600.0f, 900.0f, 1800.0f,
        3600.0f, 7200.0f, 14400.0f, 28800.0f
    };
    float stepSec = kNiceStepsSec[sizeof(kNiceStepsSec) / sizeof(kNiceStepsSec[0]) - 1];
    for (const float candidate : kNiceStepsSec) {
        if (candidate >= rawStepSec) {
            stepSec = candidate;
            break;
        }
    }

    const auto liveLabelForSeconds = [](float seconds) {
        const int roundedSec = static_cast<int>(std::lround(seconds));
        if (roundedSec < 60) {
            return QString("%1s").arg(roundedSec);
        }
        if (roundedSec < 3600) {
            return QString("%1m").arg(static_cast<int>(std::lround(roundedSec / 60.0)));
        }
        const double hours = roundedSec / 3600.0;
        if (hours < 10.0 && roundedSec % 3600 != 0) {
            return QString("%1h").arg(QString::number(hours, 'f', 1));
        }
        return QString("%1h").arg(static_cast<int>(std::lround(hours)));
    };

    for (float sec = 0; sec <= totalSec; sec += stepSec) {
        const float frac = sec / totalSec;
        const int y = labelRect.top() + static_cast<int>(frac * labelRect.height());
        if (y > wfRect.bottom() - 5) continue;

        // Tick mark
        p.setPen(AetherSDR::ThemeManager::instance().color("color.background.3"));
        p.drawLine(stripX, y, stripX + 4, y);

        const QString label = m_wfLive
            ? liveLabelForSeconds(sec)
            : pausedTimeLabelForAge(m_wfHistoryOffsetRows
                                    + static_cast<int>(std::round(sec * 1000.0f / msPerRow)));

        p.setPen(AetherSDR::ThemeManager::instance().color("color.text.secondary"));
        const QRect textRect(stripX + 6, y - fm.height() / 2,
                             strip.width() - 10, fm.height());
        if (m_wfLive) {
            p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, label);
        } else {
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
        const QColor col = sliceColorForOverlay(so);
        // Letter on the off-screen pill follows the same display mode as
        // the marker colour: per-client letter (with Unicode subscript)
        // in RadioIndexed mode, global letter in Global mode (#2606).
        QString letter = SliceLabel::unicodeForm(so.sliceId, so.perClientLetter);
        if (letter.isEmpty())
            letter = QString(QChar('A' + (so.sliceId % kSliceColorCount)));

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
            const QRect occluded = leftOccludedRect();
            if (!occluded.isNull())
                leftMargin = occluded.width() + 2;
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
