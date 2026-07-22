#include "SpectrumWidget.h"
#include "DbmRangeTransition.h"
#include "KiwiSdrTraceMath.h"
#include "NativeWidgetTopology.h"
#include "PanadapterRenderScheduler.h"
#include "PanadapterMessageOverlay.h"
#include "SpectrumOverlayMenu.h"
#include "VfoWidget.h"
#include "DisplaySettings.h"
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QApplication>
#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QCoreApplication>
#include <QCursor>
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
#include <QtCore/qfloat16.h>
#include "core/LogManager.h"
#include "core/PerfTelemetry.h"
#include <QSoundEffect>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include "core/ThemeManager.h"

namespace AetherSDR {

bool SpectrumWidget::s_starstruckMode = false;
QSoundEffect* SpectrumWidget::s_starstruckSound = nullptr;

namespace {

constexpr int kDssMaxIncrementalUploadRows = 8;

#ifdef AETHER_GPU_SPECTRUM
bool waterfallGpuFrequencyPreviewRequested()
{
    if (!qEnvironmentVariableIsSet("AETHER_WF_GPU_PREVIEW")) {
        return true;
    }
    return qEnvironmentVariableIntValue("AETHER_WF_GPU_PREVIEW") != 0;
}

bool waterfallFrameTextureFailureForced()
{
    return qEnvironmentVariableIsSet("AETHER_AUTOMATION")
        && qEnvironmentVariableIntValue(
               "AETHER_AUTOMATION_FORCE_WF_FRAME_TEXTURE_FAILURE") != 0;
}

bool waterfallFrameResizeFailureForced()
{
    return qEnvironmentVariableIsSet("AETHER_AUTOMATION")
        && qEnvironmentVariableIntValue(
               "AETHER_AUTOMATION_FORCE_WF_FRAME_RESIZE_FAILURE") != 0;
}
#endif

constexpr int dssFillVerticesPerRow()
{
    return (DssRenderer::kCols - 1) * 6;
}

constexpr int dssLineVerticesPerRow()
{
    return (DssRenderer::kCols - 1) * 2;
}

void appendDssVertex(QVector<float>& vertices, float u, float v, float edge)
{
    vertices << u << v << edge;
}

struct VfoPos {
    int sliceId;
    int x;
    VfoWidget* w;
    int splitPartner;
    const SpectrumWidget::SliceOverlay* overlay;
};

VfoWidget::FlagDir singleVfoFlagDirectionForOverlay(
    const SpectrumWidget::SliceOverlay& overlay,
    VfoWidget* widget,
    int markerX,
    int spectrumWidth)
{
    if (overlay.mode == "RTTY" || overlay.mode == "DIGL") {
        return VfoWidget::ForceRight;
    }

    const bool defaultOnLeft = VfoWidget::defaultFlagOnLeftForMode(overlay.mode);
    const bool previousOnLeft = widget ? widget->onLeft() : defaultOnLeft;
    return VfoWidget::autoDirectionForSingleFlag(
        markerX, widget ? widget->width() : 0, spectrumWidth,
        defaultOnLeft, previousOnLeft);
}

VfoWidget::FlagDir deconflictedVfoFlagDirection(
    const QVector<VfoPos>& vfos, int index, int panelWidth, int spectrumWidth)
{
    const int previousMarkerX = index > 0 ? vfos[index - 1].x : 0;
    const int nextMarkerX = index + 1 < vfos.size() ? vfos[index + 1].x : 0;
    const bool previousOnLeft = vfos[index].w ? vfos[index].w->onLeft() : true;
    return VfoWidget::autoDirectionForDeconflictedFlag(
        index, vfos.size(), vfos[index].x, previousMarkerX, nextMarkerX,
        panelWidth, spectrumWidth, previousOnLeft);
}

bool flagDirectionOnLeft(VfoWidget::FlagDir dir)
{
    return dir == VfoWidget::ForceLeft || dir == VfoWidget::LockLeft;
}

bool overlayIsDiversityPairCandidate(const SpectrumWidget::SliceOverlay& overlay)
{
    return overlay.diversity;
}

bool overlaysAreAttachedDiversityPair(const SpectrumWidget::SliceOverlay* active,
                                      const SpectrumWidget::SliceOverlay& overlay)
{
    if (!active || active->sliceId == overlay.sliceId
        || !active->diversity || !overlay.diversity) {
        return false;
    }

    const bool parentChildPair =
        (active->diversityParent && overlay.diversityChild)
        || (active->diversityChild && overlay.diversityParent);
    if (parentChildPair) {
        return true;
    }

    if (active->diversityIndex >= 0 && overlay.diversityIndex >= 0) {
        return active->diversityIndex != overlay.diversityIndex;
    }

    return true;
}

int diversityOrderKeyForVfo(const VfoPos& vfo)
{
    if (!vfo.overlay) {
        return 1000 + std::max(vfo.sliceId, 0);
    }

    return VfoWidget::diversityPairOrderKey(
        vfo.overlay->diversityParent,
        vfo.overlay->diversityChild,
        vfo.overlay->diversityIndex,
        vfo.sliceId);
}

void assignSplitPairDirections(const QVector<VfoPos>& vfos,
                               QMap<int, VfoWidget::FlagDir>& dirMap)
{
    for (int i = 0; i < vfos.size(); ++i) {
        if (vfos[i].splitPartner < 0) {
            continue;
        }
        if (dirMap.contains(vfos[i].sliceId)) {
            continue;
        }

        int partnerIndex = -1;
        for (int j = 0; j < vfos.size(); ++j) {
            if (vfos[j].sliceId == vfos[i].splitPartner) {
                partnerIndex = j;
                break;
            }
        }
        if (partnerIndex < 0) {
            continue;
        }
        if (dirMap.contains(vfos[partnerIndex].sliceId)) {
            continue;
        }

        // Split partners stay locked to opposite sides regardless of edge
        // proximity (#2663).  Flipping a partner near an edge collapses both
        // panels onto the same side and makes the RX/TX pair overlap.
        const int leftIndex = (vfos[i].x <= vfos[partnerIndex].x) ? i : partnerIndex;
        const int rightIndex = (leftIndex == i) ? partnerIndex : i;
        dirMap[vfos[leftIndex].sliceId] = VfoWidget::LockLeft;
        dirMap[vfos[rightIndex].sliceId] = VfoWidget::LockRight;
    }
}

void assignDiversityPairDirections(const QVector<VfoPos>& vfos,
                                   QMap<int, VfoWidget::FlagDir>& dirMap)
{
    QVector<int> diversityIndices;
    for (int i = 0; i < vfos.size(); ++i) {
        if (!vfos[i].overlay || dirMap.contains(vfos[i].sliceId)) {
            continue;
        }
        if (!overlayIsDiversityPairCandidate(*vfos[i].overlay)) {
            continue;
        }
        diversityIndices.append(i);
    }

    if (diversityIndices.size() != 2) {
        return;
    }

    std::sort(diversityIndices.begin(), diversityIndices.end(),
              [&vfos](int lhs, int rhs) {
        const int lhsKey = diversityOrderKeyForVfo(vfos[lhs]);
        const int rhsKey = diversityOrderKeyForVfo(vfos[rhs]);
        if (lhsKey != rhsKey) {
            return lhsKey < rhsKey;
        }
        return vfos[lhs].sliceId < vfos[rhs].sliceId;
    });

    dirMap[vfos[diversityIndices[0]].sliceId] = VfoWidget::LockLeft;
    dirMap[vfos[diversityIndices[1]].sliceId] = VfoWidget::LockRight;
}

void assignModeForcedDirections(const QVector<SpectrumWidget::SliceOverlay>& overlays,
                                QMap<int, VfoWidget::FlagDir>& dirMap)
{
    for (const SpectrumWidget::SliceOverlay& overlay : overlays) {
        if (overlay.mode == "RTTY" || overlay.mode == "DIGL") {
            dirMap[overlay.sliceId] = VfoWidget::ForceRight;
        }
    }
}

} // namespace

inline QColor kAetherBrandBlue() { return AetherSDR::ThemeManager::instance().color("color.accent"); }
inline QColor kAetherBrandGreen() { return AetherSDR::ThemeManager::instance().color("color.accent.success"); }
inline QColor kConnectionTextColor() { return AetherSDR::ThemeManager::instance().color("color.text.primary"); }
static constexpr float kMinDisplayDbm = -180.0f;
static constexpr float kMaxDisplayDbm = 80.0f;
static constexpr float kMinDisplayRangeDb = 10.0f;
static constexpr float kMaxDisplayRangeDb = 180.0f;
static constexpr int kWaterfallLineDurationMinMs = 1;
static constexpr int kWaterfallLineDurationMaxMs = 100;
static constexpr int kWaterfallHistoryCapacityMsPerRow = 50;
static constexpr int kWaterfallRatePercentMin = 1;
static constexpr int kWaterfallRatePercentMax = 100;
static constexpr float kKiwiSdrWaterfallMinDbm = -255.0f;
static constexpr float kKiwiSdrWaterfallMaxDbm = 0.0f;
static constexpr int kNativeWaterfallFallbackMinTimeoutMs = 2000;
static constexpr int kNativeWaterfallFallbackMaxTimeoutMs = 20000;
static constexpr int kNativeWaterfallRateChangeGraceMaxMs = 14000;
static constexpr int kPanDragFrameMs = 33;
static constexpr int kPanDragCommandMs = 33;
static constexpr int kPanDragSettleMs = 160;
static constexpr int kFrequencyRangeCommandMs = 33;
static constexpr int kFrequencyRangeSettleMs = 300;
static constexpr int kResizeBufferSettleMs = 120;
static constexpr float kDbmReleasePreviewChangeThresholdDb = 0.05f;
static constexpr float kDbmReleaseRebaseMinImprovementDb = 0.75f;
static constexpr int kFilterEdgeGrabPx = 8;
static constexpr int kFilterPassbandMinBodyPx = 6;
static constexpr int kMaxFftDisplayTracePoints = 8192;
static constexpr int kFftDisplayOversample = 4;
static constexpr float kFftDisplaySpatialSmoothBlend = 0.75f;
static constexpr float kFftLineFeatherPx = 1.0f;
static constexpr float kFftLineFeatherAlpha = 0.22f;
static constexpr float kFftLineCoreAlpha = 0.90f;
static constexpr const char* kSliceCursorOverrideActiveProperty =
    "_aetherSliceCursorOverrideActive";
static constexpr const char* kSliceCursorOverrideHadCursorProperty =
    "_aetherSliceCursorOverrideHadCursor";
static constexpr const char* kSliceCursorOverrideShapeProperty =
    "_aetherSliceCursorOverrideShape";

static bool mhzNearlyEqual(double a, double b)
{
    return std::abs(a - b) <= 1.0e-6;
}

static float clampDbmBottom(float bottomDbm)
{
    if (!std::isfinite(bottomDbm)) {
        return kMinDisplayDbm;
    }
    return std::clamp(bottomDbm,
                      kMinDisplayDbm,
                      kMaxDisplayDbm - kMinDisplayRangeDb);
}

static float clampDbmRangeForBottom(float bottomDbm, float rangeDb)
{
    bottomDbm = clampDbmBottom(bottomDbm);
    if (!std::isfinite(rangeDb)) {
        rangeDb = kMinDisplayRangeDb;
    }
    const float maxRangeForBottom =
        std::min(kMaxDisplayRangeDb, kMaxDisplayDbm - bottomDbm);
    return std::clamp(rangeDb,
                      kMinDisplayRangeDb,
                      std::max(kMinDisplayRangeDb, maxRangeForBottom));
}

static float clampDbmRefForRange(float refDbm, float rangeDb)
{
    if (!std::isfinite(rangeDb)) {
        rangeDb = kMinDisplayRangeDb;
    }
    rangeDb = std::clamp(rangeDb, kMinDisplayRangeDb, kMaxDisplayRangeDb);
    if (!std::isfinite(refDbm)) {
        return kMinDisplayDbm + rangeDb;
    }
    return std::clamp(refDbm,
                      kMinDisplayDbm + rangeDb,
                      kMaxDisplayDbm);
}

static bool clampDbmRange(float& minDbm, float& maxDbm)
{
    if (!std::isfinite(minDbm) || !std::isfinite(maxDbm)) {
        return false;
    }

    minDbm = std::max(minDbm, kMinDisplayDbm);
    maxDbm = std::min(maxDbm, kMaxDisplayDbm);
    if (maxDbm - minDbm > kMaxDisplayRangeDb) {
        minDbm = maxDbm - kMaxDisplayRangeDb;
    }
    if (maxDbm - minDbm < kMinDisplayRangeDb) {
        maxDbm = std::min(kMaxDisplayDbm, minDbm + kMinDisplayRangeDb);
        minDbm = std::min(minDbm, maxDbm - kMinDisplayRangeDb);
        minDbm = std::max(minDbm, kMinDisplayDbm);
    }
    return true;
}

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

static float clampedCatmullRom(float p0, float p1, float p2, float p3, float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float value = 0.5f * ((2.0f * p1)
        + (-p0 + p2) * t
        + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
        + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    const float lo = std::min(p1, p2);
    const float hi = std::max(p1, p2);
    return std::clamp(value, lo, hi);
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

void SpectrumWidget::setRenderScheduler(PanadapterRenderScheduler* scheduler)
{
    if (m_renderScheduler == scheduler) {
        return;
    }

    m_renderScheduler = scheduler;
    m_presentPending = false;
    m_presentCoalesceClock.invalidate();
}

QVariantMap SpectrumWidget::automationRhiSnapshot() const
{
    QVariantMap m;
    m[QStringLiteral("panIndex")] = m_panIndex;
    m[QStringLiteral("name")] = objectName();
    m[QStringLiteral("visible")] = isVisible();
    const qreal dpr = devicePixelRatioF();
    m[QStringLiteral("widthPx")] = width();
    m[QStringLiteral("heightPx")] = height();
    m[QStringLiteral("dpr")] = dpr;
#ifdef Q_OS_MAC
    appendNativeWidgetTopology(m, *this);
#endif
#ifdef AETHER_GPU_SPECTRUM
    m[QStringLiteral("gpu")] = true;
    m[QStringLiteral("renderer")] = rendererDescription();
    const QSize fixed = fixedColorBufferSize();
    // Unset fixedColorBufferSize() is the null QSize(-1,-1) — isEmpty()
    // covers it (and any degenerate size) → QRhiWidget auto-sizes.
    const bool autoSized = fixed.isEmpty();
    m[QStringLiteral("colorBufferAutoSized")] = autoSized;
    m[QStringLiteral("colorBufferW")] = fixed.width();
    m[QStringLiteral("colorBufferH")] = fixed.height();
    // What an even-aligned pin *should* be for the current size — lets a test
    // assert the #4091 alignment without recomputing the formula itself.
    const int expW = static_cast<int>(std::ceil(width() * dpr));
    const int expH = static_cast<int>(std::ceil(height() * dpr));
    m[QStringLiteral("expectedEvenW")] = expW + (expW & 1);
    m[QStringLiteral("expectedEvenH")] = expH + (expH & 1);
    m[QStringLiteral("evenAligned")] =
        !autoSized && (fixed.width() % 2 == 0) && (fixed.height() % 2 == 0);
#else
    m[QStringLiteral("gpu")] = false;
#endif
    return m;
}

QVariantMap SpectrumWidget::panstatsSnapshot(bool reset)
{
    const double secs = std::max(0.001, m_panStats.sinceMs() / 1000.0);
    const auto msPerSec = [secs](quint64 us) { return (us / 1000.0) / secs; };

    QVariantMap m;
    m[QStringLiteral("panIndex")] = m_panIndex;
    m[QStringLiteral("name")] = objectName();
    m[QStringLiteral("visible")] = isVisible();
    m[QStringLiteral("widthPx")] = width();
    m[QStringLiteral("heightPx")] = height();
    m[QStringLiteral("dpr")] = devicePixelRatioF();
    m[QStringLiteral("renderMode")] =
        m_spectrumRenderMode == SpectrumRenderMode::Mode3D
            ? QStringLiteral("3D") : QStringLiteral("2D");
    m[QStringLiteral("renderer")] = rendererDescription();
    m[QStringLiteral("sinceMs")] = static_cast<qlonglong>(m_panStats.sinceMs());
    m[QStringLiteral("fftAverage")] = m_fftAverage;
    m[QStringLiteral("fftFps")] = m_fftFps;
    m[QStringLiteral("fftWeightedAverage")] = m_fftWeightedAvg;
    m[QStringLiteral("waterfallLineDurationMs")] = m_wfLineDuration;
    m[QStringLiteral("waterfallSource")] = m_kiwiSdrWaterfallActive
        ? QStringLiteral("kiwi") : QStringLiteral("flex");

    m[QStringLiteral("fftFramesPerSec")] = m_panStats.updateSpectrumCalls / secs;
    m[QStringLiteral("ingestMsPerSec")] = msPerSec(m_panStats.updateSpectrumUs);
    m[QStringLiteral("gpuFramesPerSec")] = m_panStats.gpuFrames / secs;
    // Main-thread budget consumed preparing + encoding GPU frames, in
    // "ms per wall second" — the single number to compare before/after.
    m[QStringLiteral("gpuFrameMsPerSec")] = msPerSec(m_panStats.gpuFrameUs);
    m[QStringLiteral("avgGpuFrameUs")] = m_panStats.gpuFrames
        ? static_cast<double>(m_panStats.gpuFrameUs) / m_panStats.gpuFrames : 0.0;
    m[QStringLiteral("fftBuildMsPerSec")] = msPerSec(m_panStats.fftBuildUs);
    m[QStringLiteral("fftVboBytesPerSec")] =
        static_cast<double>(m_panStats.fftVboBytes) / secs;
    m[QStringLiteral("overlayRebuildsPerSec")] = m_panStats.overlayRebuilds / secs;
    m[QStringLiteral("overlayRebuildMsPerSec")] = msPerSec(m_panStats.overlayRebuildUs);
    m[QStringLiteral("overlayUploadBytesPerSec")] =
        static_cast<double>(m_panStats.overlayUploadBytes) / secs;
    m[QStringLiteral("previewOverlayTransformsPerSec")] =
        m_panStats.previewOverlayTransforms / secs;
    m[QStringLiteral("previewOverlayCommitRefreshes")] =
        static_cast<qulonglong>(m_panStats.previewOverlayCommitRefreshes);
    m[QStringLiteral("previewScaleRefreshesPerSec")] =
        m_panStats.previewScaleRefreshes / secs;
    m[QStringLiteral("previewScalePaintMsPerSec")] =
        msPerSec(m_panStats.previewScalePaintUs);
    m[QStringLiteral("previewScaleUploadBytesPerSec")] =
        static_cast<double>(m_panStats.previewScaleUploadBytes) / secs;
    m[QStringLiteral("wfUploadBytesPerSec")] =
        static_cast<double>(m_panStats.wfUploadBytes) / secs;
    m[QStringLiteral("nativeWaterfallUpdatesPerSec")] =
        m_panStats.nativeWaterfallCalls / secs;
    m[QStringLiteral("nativeWaterfallUpdateMsPerSec")] =
        msPerSec(m_panStats.nativeWaterfallUs);
    m[QStringLiteral("nativeWaterfallHiddenUpdatesPerSec")] =
        m_panStats.nativeWaterfallHiddenCalls / secs;
    m[QStringLiteral("kiwiWaterfallUpdatesPerSec")] =
        m_panStats.kiwiWaterfallCalls / secs;
    m[QStringLiteral("kiwiWaterfallUpdateMsPerSec")] =
        msPerSec(m_panStats.kiwiWaterfallUs);
    m[QStringLiteral("kiwiWaterfallHiddenUpdatesPerSec")] =
        m_panStats.kiwiWaterfallHiddenCalls / secs;
    m[QStringLiteral("waterfallVisibleRowsPerSec")] =
        m_panStats.waterfallVisibleRows / secs;
    m[QStringLiteral("waterfallVisibleRowMsPerSec")] =
        msPerSec(m_panStats.waterfallVisibleRowUs);
    m[QStringLiteral("waterfallHistoryRowsPerSec")] =
        m_panStats.waterfallHistoryRows / secs;
    m[QStringLiteral("waterfallHistoryRowMsPerSec")] =
        msPerSec(m_panStats.waterfallHistoryRowUs);
    m[QStringLiteral("dssLiveRowsPerSec")] = m_panStats.dssLiveRows / secs;
    m[QStringLiteral("dssLiveMsPerSec")] = msPerSec(m_panStats.dssLiveUs);
    m[QStringLiteral("dssHiddenLiveRowsPerSec")] =
        m_panStats.dssHiddenLiveRows / secs;
    m[QStringLiteral("dssHistoryRowsPerSec")] = m_panStats.dssHistoryRows / secs;
    m[QStringLiteral("dssHistoryMsPerSec")] = msPerSec(m_panStats.dssHistoryUs);
    m[QStringLiteral("paintsPerSec")] = m_panStats.paintEvents / secs;
    m[QStringLiteral("paintMsPerSec")] = msPerSec(m_panStats.paintUs);

    quint64 cachedWaterfallVisibleBytes = 0;
    quint64 cachedWaterfallHistoryBytes = 0;
    quint64 dssFixedBytes = m_dss.fixedStorageBytes();
    quint64 dssHistoryBytes = m_dss.historyStorageBytes();
    quint64 dssCacheBytes = m_dss.cacheStorageBytes();
    int dssRendererCount = 1;
    auto accountState = [&](const WaterfallStreamState& state) {
        cachedWaterfallVisibleBytes += state.waterfall.isNull()
            ? 0 : static_cast<quint64>(state.waterfall.sizeInBytes());
        cachedWaterfallHistoryBytes += static_cast<quint64>(
            state.waterfallHistory.allocatedBytes());
        dssFixedBytes += state.dss.fixedStorageBytes();
        dssHistoryBytes += state.dss.historyStorageBytes();
        dssCacheBytes += state.dss.cacheStorageBytes();
        ++dssRendererCount;
    };
    accountState(m_nativeWaterfallState);
    accountState(m_kiwiWaterfallState);
    for (auto it = m_kiwiProfileWaterfallStates.cbegin();
         it != m_kiwiProfileWaterfallStates.cend(); ++it) {
        accountState(it.value());
    }
    const quint64 currentWaterfallVisibleBytes = m_waterfall.isNull()
        ? 0 : static_cast<quint64>(m_waterfall.sizeInBytes());
    const quint64 currentWaterfallHistoryBytes = static_cast<quint64>(
        m_waterfallHistory.allocatedBytes());
    m[QStringLiteral("currentWaterfallVisibleBytes")] =
        static_cast<qulonglong>(currentWaterfallVisibleBytes);
    m[QStringLiteral("currentWaterfallHistoryBytes")] =
        static_cast<qulonglong>(currentWaterfallHistoryBytes);
    m[QStringLiteral("cachedWaterfallVisibleBytes")] =
        static_cast<qulonglong>(cachedWaterfallVisibleBytes);
    m[QStringLiteral("cachedWaterfallHistoryBytes")] =
        static_cast<qulonglong>(cachedWaterfallHistoryBytes);
    m[QStringLiteral("waterfallAllocatedBytes")] = static_cast<qulonglong>(
        currentWaterfallVisibleBytes + currentWaterfallHistoryBytes
        + cachedWaterfallVisibleBytes + cachedWaterfallHistoryBytes);
    m[QStringLiteral("dssRendererCount")] = dssRendererCount;
    m[QStringLiteral("dssFixedBytes")] = static_cast<qulonglong>(dssFixedBytes);
    m[QStringLiteral("dssHistoryBytes")] = static_cast<qulonglong>(dssHistoryBytes);
    m[QStringLiteral("dssCacheBytes")] = static_cast<qulonglong>(dssCacheBytes);
    m[QStringLiteral("dssAllocatedBytes")] = static_cast<qulonglong>(
        dssFixedBytes + dssHistoryBytes + dssCacheBytes);

    QVariantMap causes;
    for (auto it = m_panStats.dirtyCauses.cbegin();
         it != m_panStats.dirtyCauses.cend(); ++it)
        causes[QString::fromLatin1(it.key())] = static_cast<qulonglong>(it.value());
    m[QStringLiteral("overlayDirtyCauses")] = causes;

    if (reset)
        m_panStats.reset();
    return m;
}

QVariantMap SpectrumWidget::renderSchedulerStatsSnapshot(bool reset)
{
    if (!m_renderScheduler) {
        QVariantMap stats;
        stats[QStringLiteral("enabled")] = false;
        return stats;
    }

    return m_renderScheduler->statsSnapshot(reset);
}

QVariantMap SpectrumWidget::automationDssSnapshot() const
{
    QVariantMap m;
    m[QStringLiteral("ok")] = true;
    m[QStringLiteral("panIndex")] = m_panIndex;
    m[QStringLiteral("visible")] = isVisible();
    m[QStringLiteral("kiwiActive")] = m_kiwiSdrWaterfallActive;
    m[QStringLiteral("live")] = m_wfLive;
    m[QStringLiteral("centerMhz")] = m_centerMhz;
    m[QStringLiteral("bandwidthMhz")] = m_bandwidthMhz;
    m[QStringLiteral("frequencyPreviewAvailable")] = frequencyPreviewAvailable();
    m[QStringLiteral("frequencyPreviewActive")] = m_frequencyPreviewActive;
    m[QStringLiteral("frequencyPreviewBaseCenterMhz")] =
        m_frequencyPreviewBaseCenterMhz;
    m[QStringLiteral("frequencyPreviewBaseBandwidthMhz")] =
        m_frequencyPreviewBaseBandwidthMhz;
    m[QStringLiteral("frequencyPreviewTargetCenterMhz")] =
        m_frequencyPreviewTargetCenterMhz;
    m[QStringLiteral("frequencyPreviewTargetBandwidthMhz")] =
        m_frequencyPreviewTargetBandwidthMhz;
    const FrequencyPreviewTransform previewTransform =
        frequencyPreviewTransform(
            FrequencyFrame{m_frequencyPreviewBaseCenterMhz,
                           m_frequencyPreviewBaseBandwidthMhz},
            FrequencyFrame{m_frequencyPreviewTargetCenterMhz,
                           m_frequencyPreviewTargetBandwidthMhz});
    m[QStringLiteral("frequencyPreviewScale")] = previewTransform.valid
        ? previewTransform.scale : 1.0;
    m[QStringLiteral("frequencyPreviewOffset")] = previewTransform.valid
        ? previewTransform.offset : 0.0;
    m[QStringLiteral("frequencyPreviewUpdateCount")] =
        static_cast<qulonglong>(m_frequencyPreviewUpdateCount);
    m[QStringLiteral("frequencyPreviewPresentCount")] =
        static_cast<qulonglong>(m_frequencyPreviewPresentCount);
    m[QStringLiteral("frequencyPreviewCommitCount")] =
        static_cast<qulonglong>(m_frequencyPreviewCommitCount);
    m[QStringLiteral("frequencyPreviewNativeVisibleRows")] =
        static_cast<qulonglong>(m_frequencyPreviewNativeVisibleRows);
    m[QStringLiteral("frequencyPreviewRemappedDssRows")] =
        static_cast<qulonglong>(m_frequencyPreviewRemappedDssRows);
    m[QStringLiteral("frequencyPreviewSuppressedViewportRebuilds")] =
        static_cast<qulonglong>(m_frequencyPreviewSuppressedViewportRebuilds);
    m[QStringLiteral("frequencyPreviewOverlayTransformCount")] =
        static_cast<qulonglong>(m_frequencyPreviewOverlayTransformCount);
    m[QStringLiteral("frequencyPreviewOverlayCommitRefreshCount")] =
        static_cast<qulonglong>(m_frequencyPreviewOverlayCommitRefreshCount);
    m[QStringLiteral("frequencyPreviewCommitLastMs")] =
        static_cast<double>(m_frequencyPreviewCommitLastNs) / 1000000.0;
    m[QStringLiteral("frequencyPreviewCommitMaxMs")] =
        static_cast<double>(m_frequencyPreviewCommitMaxNs) / 1000000.0;
#ifdef AETHER_GPU_SPECTRUM
    m[QStringLiteral("waterfallRowFrequencyFrames")] =
        m_wfPipelineMode == WaterfallPipelineMode::RowFrequencyFrames
        && m_wfFrameTexReady;
    m[QStringLiteral("waterfallPipelineReady")] =
        m_wfPipeline != nullptr && m_wfSrb != nullptr;
    m[QStringLiteral("waterfallPipelineMode")] =
        m_wfPipelineMode == WaterfallPipelineMode::RowFrequencyFrames
        ? QStringLiteral("rowFrequencyFrames")
        : QStringLiteral("legacy");
    m[QStringLiteral("waterfallPipelineFallbackReason")] =
        m_wfPipelineFallbackReason;
#endif
    if (m_wfVisibleRowCenterMhz.size() == m_waterfall.height()
        && !m_wfVisibleRowCenterMhz.isEmpty()) {
        double minCenterMhz = std::numeric_limits<double>::infinity();
        double maxCenterMhz = -std::numeric_limits<double>::infinity();
        int currentFrameRows = 0;
        for (int row = 0; row < m_wfVisibleRowCenterMhz.size(); ++row) {
            const double rowCenterMhz = m_wfVisibleRowCenterMhz[row];
            const double rowBandwidthMhz = m_wfVisibleRowBwMhz[row];
            minCenterMhz = std::min(minCenterMhz, rowCenterMhz);
            maxCenterMhz = std::max(maxCenterMhz, rowCenterMhz);
            if (mhzNearlyEqual(rowCenterMhz, m_centerMhz)
                && mhzNearlyEqual(rowBandwidthMhz, m_bandwidthMhz)) {
                ++currentFrameRows;
            }
        }
        m[QStringLiteral("waterfallVisibleFrameMinCenterMhz")] = minCenterMhz;
        m[QStringLiteral("waterfallVisibleFrameMaxCenterMhz")] = maxCenterMhz;
        m[QStringLiteral("waterfallVisibleCurrentFrameRows")] = currentFrameRows;
    }
    m[QStringLiteral("frequencyRangeCommandPending")] =
        m_frequencyRangeCommandThrottle.hasPending();
    m[QStringLiteral("frequencyRangeCommandCount")] =
        static_cast<qulonglong>(m_frequencyRangeCommandCount);
    m[QStringLiteral("historyOffsetRows")] = m_wfHistoryOffsetRows;
    m[QStringLiteral("maxHistoryOffsetRows")] = maxWaterfallHistoryOffsetRows();
    m[QStringLiteral("waterfallRows")] = m_waterfall.height();
    m[QStringLiteral("waterfallWidth")] = m_waterfall.width();
    m[QStringLiteral("waterfallWriteRow")] = m_wfWriteRow;
    m[QStringLiteral("waterfallHistoryRows")] = m_wfHistoryRowCount;
    m[QStringLiteral("waterfallHistoryCapacityRows")] =
        m_waterfallHistory.capacityRows();
    m[QStringLiteral("waterfallHistoryWidth")] = m_waterfallHistory.width();
    m[QStringLiteral("waterfallHistoryAllocatedChunks")] =
        m_waterfallHistory.allocatedChunkCount();
    m[QStringLiteral("resizeEventCount")] =
        static_cast<qulonglong>(m_resizeEventCount);
    m[QStringLiteral("resizeBufferCommitCount")] =
        static_cast<qulonglong>(m_resizeBufferCommitCount);
    m[QStringLiteral("resizeFrameHoldCount")] =
        static_cast<qulonglong>(m_resizeFrameHoldCount);
    m[QStringLiteral("resizePreviewFrameCount")] =
        static_cast<qulonglong>(m_resizePreviewFrameCount);
    m[QStringLiteral("resizeBufferPending")] =
        m_resizeBufferSettleTimer && m_resizeBufferSettleTimer->isActive();
    m[QStringLiteral("resizeBufferCommitLastMs")] =
        static_cast<double>(m_resizeBufferCommitLastNs) / 1000000.0;
    m[QStringLiteral("resizeBufferCommitMaxMs")] =
        static_cast<double>(m_resizeBufferCommitMaxNs) / 1000000.0;
#ifdef AETHER_GPU_SPECTRUM
    m[QStringLiteral("resizeRenderBufferWidth")] = fixedColorBufferSize().width();
    m[QStringLiteral("resizeRenderBufferHeight")] = fixedColorBufferSize().height();
#endif
    m[QStringLiteral("dssVisibleRows")] = m_dss.rowCount();
    m[QStringLiteral("dssRowGeneration")] =
        static_cast<qulonglong>(m_dss.rowGeneration());
    m[QStringLiteral("dssHistoryRows")] = m_dss.historyRowCount();
    m[QStringLiteral("dssHistoryCapacityRows")] = m_dss.historyCapacityRows();
    m[QStringLiteral("dssFixedBytes")] =
        static_cast<qulonglong>(m_dss.fixedStorageBytes());
    m[QStringLiteral("dssHistoryBytes")] =
        static_cast<qulonglong>(m_dss.historyStorageBytes());
    m[QStringLiteral("dssCacheBytes")] =
        static_cast<qulonglong>(m_dss.cacheStorageBytes());
    m[QStringLiteral("dssAllocatedBytes")] =
        static_cast<qulonglong>(m_dss.allocatedBytes());
    m[QStringLiteral("waterfallHistoryBytes")] =
        static_cast<qulonglong>(m_waterfallHistory.allocatedBytes());

    int peakBin = -1;
    float peakDbm = -1000.0f;
    float frontMinDbm = -1000.0f;
    float frontMaxDbm = -1000.0f;
    float frontSpanDb = 0.0f;
    int frontMinValueBins = 0;
    int frontLongestFlatRunBins = 0;
    int maxMinValueBins = 0;
    int maxLongestFlatRunBins = 0;
    int flatRows = 0;
    int nonFlatRows = 0;
    for (int age = 0; age < m_dss.rowCount(); ++age) {
        const int ring = (m_dss.headRing() + age) % m_dss.rows();
        const float* row = m_dss.rowDataRing(ring);
        const DssRenderer::RowStats rowStats = m_dss.rowStats(age);
        for (int c = 0; c < m_dss.cols(); ++c) {
            if (!std::isfinite(row[c])) {
                continue;
            }
            if (age == 0 && (peakBin < 0 || row[c] > peakDbm)) {
                peakBin = c;
                peakDbm = row[c];
            }
        }
        if (rowStats.finiteBins == 0) {
            continue;
        }
        const float rowSpanDb = rowStats.maxDbm - rowStats.minDbm;
        if (age == 0) {
            frontMinDbm = rowStats.minDbm;
            frontMaxDbm = rowStats.maxDbm;
            frontSpanDb = rowSpanDb;
            frontMinValueBins = rowStats.minValueBins;
            frontLongestFlatRunBins = rowStats.longestFlatRunBins;
        }
        maxMinValueBins = std::max(maxMinValueBins, rowStats.minValueBins);
        maxLongestFlatRunBins =
            std::max(maxLongestFlatRunBins, rowStats.longestFlatRunBins);
        if (rowSpanDb > 0.01f) {
            ++nonFlatRows;
        } else {
            ++flatRows;
        }
    }
    m[QStringLiteral("dssVisiblePeakBin")] = peakBin;
    m[QStringLiteral("dssVisiblePeakDbm")] = peakDbm;
    m[QStringLiteral("dssVisibleFrontMinDbm")] = frontMinDbm;
    m[QStringLiteral("dssVisibleFrontMaxDbm")] = frontMaxDbm;
    m[QStringLiteral("dssVisibleFrontSpanDb")] = frontSpanDb;
    m[QStringLiteral("dssVisibleFrontMinValueBins")] = frontMinValueBins;
    m[QStringLiteral("dssVisibleFrontLongestFlatRunBins")] =
        frontLongestFlatRunBins;
    m[QStringLiteral("dssVisibleMaxMinValueBins")] = maxMinValueBins;
    m[QStringLiteral("dssVisibleMaxLongestFlatRunBins")] =
        maxLongestFlatRunBins;
    m[QStringLiteral("dssVisibleFlatRows")] = flatRows;
    m[QStringLiteral("dssVisibleNonFlatRows")] = nonFlatRows;
    return m;
}

QVariantMap SpectrumWidget::automationDssReset(bool kiwiStream)
{
    if (!qEnvironmentVariableIsSet("AETHER_AUTOMATION")) {
        return QVariantMap{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QStringLiteral("AETHER_AUTOMATION is not enabled")},
        };
    }

    clearFrequencyPreview();
    if (m_frequencyRangeSettleTimer) {
        m_frequencyRangeSettleTimer->stop();
    }
    if (m_frequencyRangeCommandTimer) {
        m_frequencyRangeCommandTimer->stop();
    }
    m_frequencyRangeSettlePending = false;
    m_frequencyRangePendingValid = false;
    m_frequencyRangeCommandThrottle.clear();
    m_frequencyRangeCommandClock.invalidate();
    m_frequencyPreviewUpdateCount = 0;
    m_frequencyPreviewPresentCount = 0;
    m_frequencyPreviewCommitCount = 0;
    m_frequencyPreviewNativeVisibleRows = 0;
    m_frequencyPreviewRemappedDssRows = 0;
    m_frequencyPreviewSuppressedViewportRebuilds = 0;
    m_frequencyPreviewOverlayTransformCount = 0;
    m_frequencyPreviewOverlayCommitRefreshCount = 0;
    m_frequencyPreviewCommitLastNs = 0;
    m_frequencyPreviewCommitMaxNs = 0;
    m_frequencyRangeCommandCount = 0;

    const int desiredWidth = !m_waterfall.isNull()
        ? m_waterfall.width()
        : std::max(512, contentWidth());
    const int chromeH = freqScaleH() + DIVIDER_H;
    const int desiredHeight = !m_waterfall.isNull()
        ? m_waterfall.height()
        : std::max(96, static_cast<int>(
              std::max(160, height() - chromeH) * (1.0f - m_spectrumFrac)));

    m_nativeWaterfallState = WaterfallStreamState{};
    m_kiwiWaterfallState = WaterfallStreamState{};
    m_kiwiProfileWaterfallStates.clear();
    m_kiwiSdrWaterfallAvailable = kiwiStream;
    m_kiwiSdrWaterfallActive = kiwiStream;
    resetCurrentWaterfallRowsForSize(
        QSize(desiredWidth, desiredHeight),
        QSize(desiredWidth, waterfallHistoryCapacityRows()));
    clearCurrentWaterfallRows();
    m_kiwiSdrWaterfallAvailable = kiwiStream;
    m_kiwiSdrWaterfallActive = kiwiStream;
    m_hasNativeWaterfall = !kiwiStream;
    m_lastNativeTileMs = m_hasNativeWaterfall
        ? QDateTime::currentMSecsSinceEpoch()
        : 0;
    m_waterfallFallbackActive = false;
    m_nextFallbackWaterfallRowMs = 0;
    m_nativeWaterfallFallbackHoldUntilMs = m_hasNativeWaterfall
        ? m_lastNativeTileMs + 60'000
        : 0;
    m_wfBlankerRingCount = 0;
    m_wfBlankerRingIdx = 0;
    m_wfLastGoodLevels.clear();
    resetDssUploadState();
    update();

    QVariantMap snap = automationDssSnapshot();
    snap[QStringLiteral("reset")] = true;
    return snap;
}

QVariantMap SpectrumWidget::automationDssInjectRows(int count,
                                                    int firstPeakBin,
                                                    int stepBin,
                                                    bool kiwiStream,
                                                    double rowLowMhz,
                                                    double rowHighMhz)
{
    if (!qEnvironmentVariableIsSet("AETHER_AUTOMATION")) {
        return QVariantMap{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QStringLiteral("AETHER_AUTOMATION is not enabled")},
        };
    }
    if (count <= 0) {
        return QVariantMap{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QStringLiteral("row count must be positive")},
        };
    }
    const int maxRows = waterfallHistoryCapacityRows();
    if (count > maxRows) {
        return QVariantMap{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"),
             QStringLiteral("row count exceeds retained waterfall history capacity")},
            {QStringLiteral("maxRows"), maxRows},
        };
    }

    if (m_waterfall.isNull() || m_waterfall.width() <= 0 || m_waterfall.height() <= 1) {
        const QVariantMap reset = automationDssReset(kiwiStream);
        if (!reset.value(QStringLiteral("ok")).toBool()) {
            return reset;
        }
    }

    if (kiwiStream) {
        setKiwiSdrWaterfallAvailable(true);
        setKiwiSdrWaterfallActive(true);
    } else if (m_kiwiSdrWaterfallActive) {
        setKiwiSdrWaterfallActive(false);
    }

    const int waterfallHistoryRowsBefore = m_wfHistoryRowCount;
    const int dssHistoryRowsBefore = m_dss.historyRowCount();
    const bool hasKiwiFrameOverride =
        kiwiStream && rowLowMhz >= 0.0 && rowHighMhz > rowLowMhz;
    const double kiwiRowLowMhz = hasKiwiFrameOverride
        ? rowLowMhz
        : m_centerMhz - m_bandwidthMhz * 0.5;
    const double kiwiRowHighMhz = hasKiwiFrameOverride
        ? rowHighMhz
        : m_centerMhz + m_bandwidthMhz * 0.5;
    for (int i = 0; i < count; ++i) {
        QVector<float> bins(DssRenderer::kCols, -120.0f);
        const qint64 peakValue = static_cast<qint64>(firstPeakBin)
            + static_cast<qint64>(i) * static_cast<qint64>(stepBin);
        const int peak = static_cast<int>(
            std::clamp<qint64>(peakValue, 0, DssRenderer::kCols - 1));
        bins[peak] = -30.0f;

        if (kiwiStream) {
            updateKiwiSdrWaterfallRow(bins,
                                      kiwiRowLowMhz,
                                      kiwiRowHighMhz,
                                      static_cast<quint32>(i));
        } else {
            pushWaterfallRow(bins, m_waterfall.width());
        }
    }

    QVariantMap snap = automationDssSnapshot();
    snap[QStringLiteral("injectedRows")] = count;
    snap[QStringLiteral("waterfallHistoryRowsBefore")] = waterfallHistoryRowsBefore;
    snap[QStringLiteral("waterfallHistoryRowsAdded")] =
        m_wfHistoryRowCount - waterfallHistoryRowsBefore;
    snap[QStringLiteral("dssHistoryRowsBefore")] = dssHistoryRowsBefore;
    snap[QStringLiteral("dssHistoryRowsAdded")] =
        m_dss.historyRowCount() - dssHistoryRowsBefore;
    snap[QStringLiteral("stream")] = kiwiStream
        ? QStringLiteral("kiwi")
        : QStringLiteral("native");
    if (kiwiStream) {
        snap[QStringLiteral("rowLowMhz")] = kiwiRowLowMhz;
        snap[QStringLiteral("rowHighMhz")] = kiwiRowHighMhz;
        const double viewLowMhz = m_centerMhz - m_bandwidthMhz * 0.5;
        const double viewHighMhz = m_centerMhz + m_bandwidthMhz * 0.5;
        const double overlapMhz = std::max(
            0.0,
            std::min(kiwiRowHighMhz, viewHighMhz)
                - std::max(kiwiRowLowMhz, viewLowMhz));
        snap[QStringLiteral("viewCoverage")] = m_bandwidthMhz > 0.0
            ? overlapMhz / m_bandwidthMhz
            : 0.0;
    }
    return snap;
}

QVariantMap SpectrumWidget::automationDssSetScrollback(bool live,
                                                       int offsetRows)
{
    if (!qEnvironmentVariableIsSet("AETHER_AUTOMATION")) {
        return QVariantMap{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QStringLiteral("AETHER_AUTOMATION is not enabled")},
        };
    }

    if (live) {
        setWaterfallLive(true);
    } else {
        m_wfLive = false;
        m_wfHistoryOffsetRows =
            std::clamp(offsetRows, 0, maxWaterfallHistoryOffsetRows());
        rebuildWaterfallViewport();
        markOverlayDirty();
    }

    QVariantMap snap = automationDssSnapshot();
    snap[QStringLiteral("scrollbackSet")] = true;
    return snap;
}

QVariantMap SpectrumWidget::traceDebugSnapshot()
{
    const auto vectorStats = [this](const QVector<float>& bins) {
        QVariantMap stats;
        stats[QStringLiteral("count")] = bins.size();
        if (bins.isEmpty()) {
            stats[QStringLiteral("finiteCount")] = 0;
            stats[QStringLiteral("minDbm")] = -1000.0;
            stats[QStringLiteral("maxDbm")] = -1000.0;
            stats[QStringLiteral("noiseFloorDbm")] = -1000.0;
            return stats;
        }

        float minDbm = std::numeric_limits<float>::infinity();
        float maxDbm = -std::numeric_limits<float>::infinity();
        int finiteCount = 0;
        for (float v : bins) {
            if (!std::isfinite(v)) {
                continue;
            }
            minDbm = std::min(minDbm, v);
            maxDbm = std::max(maxDbm, v);
            ++finiteCount;
        }
        stats[QStringLiteral("finiteCount")] = finiteCount;
        stats[QStringLiteral("minDbm")] = finiteCount > 0 ? minDbm : -1000.0f;
        stats[QStringLiteral("maxDbm")] = finiteCount > 0 ? maxDbm : -1000.0f;
        stats[QStringLiteral("noiseFloorDbm")] =
            finiteCount > 0 ? estimateNoiseFloorDbm(bins) : -1000.0f;
        return stats;
    };

    const QVector<float>& flexTrace =
        !m_smoothed.isEmpty() ? m_smoothed : m_bins;

    QVariantMap m;
    m[QStringLiteral("panIndex")] = m_panIndex;
    m[QStringLiteral("name")] = objectName();
    m[QStringLiteral("visible")] = isVisible();
    m[QStringLiteral("widthPx")] = width();
    m[QStringLiteral("heightPx")] = height();
    m[QStringLiteral("renderMode")] =
        m_spectrumRenderMode == SpectrumRenderMode::Mode3D
            ? QStringLiteral("3D") : QStringLiteral("2D");
    m[QStringLiteral("kiwiWaterfallAvailable")] = m_kiwiSdrWaterfallAvailable;
    m[QStringLiteral("kiwiWaterfallActive")] = m_kiwiSdrWaterfallActive;
    m[QStringLiteral("centerMhz")] = m_centerMhz;
    m[QStringLiteral("bandwidthMhz")] = m_bandwidthMhz;
    m[QStringLiteral("refLevelDbm")] = m_refLevel;
    m[QStringLiteral("dynamicRangeDb")] = m_dynamicRange;

    m[QStringLiteral("noiseFloorPosition")] = m_noiseFloorPosition;
    m[QStringLiteral("flexNoiseFloorPosition")] = m_flexNoiseFloorPosition;
    m[QStringLiteral("kiwiNoiseFloorPosition")] = m_kiwiNoiseFloorPosition;
    m[QStringLiteral("noiseFloorEnabled")] = m_noiseFloorEnable;
    m[QStringLiteral("measuredNoiseFloorDbm")] = m_measuredNoiseFloorDbm;
    m[QStringLiteral("displayFloorDbm")] = displayFloorDbm();

    m[QStringLiteral("dssFloorDepth")] = dssFloorDepth();
    m[QStringLiteral("flexDssFloorDepth")] = m_flexDssFloorDepth;
    m[QStringLiteral("kiwiDssFloorDepth")] = m_kiwiDssFloorDepth;
    m[QStringLiteral("dssFloorOffsetDb")] = m_dssFloorOffsetDb;
    m[QStringLiteral("dssFloorDbm")] = peekDssFloorDbm();
    m[QStringLiteral("dssSpanDb")] = dssSpanDb();
    m[QStringLiteral("dssFloorAnchorDbm")] = m_dssFloorAnchorDbm;
    m[QStringLiteral("dssFloorAnchorValid")] = m_dssFloorAnchorValid;
    m[QStringLiteral("dssRows")] = m_dss.rowCount();
    m[QStringLiteral("dssCapacityRows")] = m_dss.rows();
    const DssRenderer& flexDss =
        m_kiwiSdrWaterfallActive ? m_nativeWaterfallState.dss : m_dss;
    m[QStringLiteral("flexDssRows")] = flexDss.rowCount();
    const WaterfallStreamState* kiwiState = activeKiwiWaterfallStateConst();
    m[QStringLiteral("kiwiDssRows")] = m_kiwiSdrWaterfallActive
        ? m_dss.rowCount()
        : (kiwiState ? kiwiState->dss.rowCount() : 0);

    m[QStringLiteral("kiwiDisplayRangeValid")] = m_kiwiSdrDisplayRangeValid;
    m[QStringLiteral("kiwiDisplayRangeAutoRange")] =
        m_kiwiSdrDisplayRangeAutoRange;
    m[QStringLiteral("kiwiDisplayFloorDbm")] = m_kiwiSdrDisplayFloorDbm;
    m[QStringLiteral("kiwiDisplayCeilDbm")] = m_kiwiSdrDisplayCeilDbm;
    m[QStringLiteral("kiwiFftTraceFloorDbm")] = m_kiwiSdrFftTraceFloorDbm;
    m[QStringLiteral("kiwiFftTraceFloorValid")] =
        m_kiwiSdrFftTraceFloorValid;
    m[QStringLiteral("kiwiFftTraceEstimatedFloorDbm")] =
        m_kiwiSdrFftTrace.isEmpty()
            ? -1000.0f
            : estimateKiwiSdrTraceFloorDbm(m_kiwiSdrFftTrace);

    m[QStringLiteral("activeBins")] = vectorStats(noiseFloorAutoLevelBins());
    m[QStringLiteral("flexBins")] = vectorStats(flexTrace);
    m[QStringLiteral("kiwiBins")] = vectorStats(m_kiwiSdrFftTrace);
    return m;
}

void SpectrumWidget::applyNativeWindowIsolationPolicy()
{
#if defined(AETHER_GPU_SPECTRUM) && defined(Q_OS_MAC)
    if (nativeWindowPreferred()) {
        // Order matters: block ancestor promotion *before* requesting the native
        // window, so realizing the leaf's NSView can't drag its QWidget tree
        // native (redundant window-sized Core Animation backing stores, #4339).
        // Both attributes are set here, as one unit, so a reparent that
        // re-realizes the native window can never reassert WA_NativeWindow
        // without the paired ancestor isolation.
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        setAttribute(Qt::WA_NativeWindow);
    }
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

    m_panStats.clock.start();  // panstats rates are meaningless without an epoch

    setMinimumHeight(100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
    // SpectrumWidget paints every output pixel, including the background under
    // its internally-composited transparent overlay textures.  Tell QWidget's
    // repaint manager it does not need to paint ancestors or siblings behind
    // this QRhiWidget whenever a 60 Hz data/preview frame is presented.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName(tr("Panadapter spectrum display"));
#ifdef AETHER_GPU_SPECTRUM
    // Explicitly request Metal on macOS.
#  ifdef Q_OS_MAC
    setApi(QRhiWidget::Api::Metal);
    // WA_NativeWindow forces Qt to create a dedicated native NSView for this widget.
    // Without it, QRhiWidget embedded in a QWidget hierarchy (especially one whose
    // backing store was created before this widget was added) fails to obtain a QRhi
    // context because the parent window's surface type is RasterSurface, not MetalSurface
    // (#714, Qt 6.6 era). The native view is expensive, though: every present forces
    // a raster flushSubWindow blend of the pan region on the GUI thread.
    // AETHER_PAN_NO_NATIVE_WINDOW=1 skips it to validate the composited path on
    // newer Qt, where the whole window flushes through one rhi swapchain.
    // WA_NativeWindow is set together with WA_DontCreateNativeAncestors so the
    // native leaf never promotes its QWidget ancestors (#4339); see the helper.
    applyNativeWindowIsolationPolicy();
#  else
    // AETHER_NO_GPU / QT_OPENGL=software: force the OpenGL QRhi backend so the
    // software OpenGL rasterizer requested in main.cpp actually takes effect.
    // Without this, QRhiWidget defaults to D3D11 on Windows and ignores
    // QT_OPENGL entirely, making the flag a silent no-op there (#3597). On Linux
    // the default backend is already OpenGL, so this is a harmless explicit
    // restatement that keeps the two platforms on the same code path.
    if (qtSoftwareOpenGlRequested()) {
        setApi(QRhiWidget::Api::OpenGL);
        qInfo() << "SpectrumWidget: AETHER_NO_GPU/QT_OPENGL=software — forcing "
                   "OpenGL QRhi backend (software rasterizer) instead of D3D11";
    }
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

    m_panadapterMessageOverlay = new PanadapterMessageOverlay(this);
    m_panadapterMessageOverlay->hide();

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
        if (m_kiwiSdrWaterfallActive && m_kiwiSdrSquelchLineVisible) {
            m_kiwiSdrSquelchLineVisible = false;
        } else {
            m_flexSquelchLineVisible = false;
        }
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
    m_panDragSettleTimer = new QTimer(this);
    m_panDragSettleTimer->setSingleShot(true);
    m_panDragSettleTimer->setInterval(kPanDragSettleMs);
    connect(m_panDragSettleTimer, &QTimer::timeout, this, [this]() {
        if (m_draggingPan) {
            // A pause with the mouse button still down is a radio-command
            // settle point, not the end of the visual gesture. Keep the GPU
            // preview alive; committing here used to rebuild the full visible
            // waterfall synchronously and caused a hitch before dragging
            // resumed.
            emit panDragSettled(m_centerMhz, m_bandwidthMhz);
        }
    });
    m_frequencyRangeSettleTimer = new QTimer(this);
    m_frequencyRangeSettleTimer->setSingleShot(true);
    m_frequencyRangeSettleTimer->setInterval(kFrequencyRangeSettleMs);
    connect(m_frequencyRangeSettleTimer, &QTimer::timeout,
            this, &SpectrumWidget::finishFrequencyRangeSettleUpdate);
    m_frequencyRangeCommandTimer = new QTimer(this);
    m_frequencyRangeCommandTimer->setSingleShot(true);
    connect(m_frequencyRangeCommandTimer, &QTimer::timeout, this, [this]() {
        const std::optional<FrequencyRangeCommand> command =
            m_frequencyRangeCommandThrottle.takePending();
        if (!command.has_value()) {
            return;
        }
        m_frequencyRangeCommandClock.restart();
        ++m_frequencyRangeCommandCount;
        emit frequencyRangeChangeRequested(command->centerMhz,
                                           command->bandwidthMhz);
    });
    m_resizeBufferSettleTimer = new QTimer(this);
    m_resizeBufferSettleTimer->setSingleShot(true);
    m_resizeBufferSettleTimer->setInterval(kResizeBufferSettleMs);
    connect(m_resizeBufferSettleTimer, &QTimer::timeout,
            this, &SpectrumWidget::applySettledResizeBuffers);

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

    // objectName + accessibleName let the automation bridge target these by a
    // stable handle instead of the visible label \u2014 notably zoom-out, whose glyph
    // is a U+2212 minus sign that is awkward to send as button text. (#3646)
    auto makeBtn = [&](const QString& text, const QString& objName, const QString& a11y) {
        auto* btn = new QPushButton(text, this);
        btn->setObjectName(objName);
        btn->setAccessibleName(a11y);
        btn->setFixedSize(22, 22);
        btn->setStyleSheet(kZoomBtnStyle);
        btn->setCursor(Qt::PointingHandCursor);
        return btn;
    };
    m_kiwiSdrDisplaySourceBtn = new QPushButton(QStringLiteral("Flex"), this);
    m_kiwiSdrDisplaySourceBtn->setObjectName(QStringLiteral("panDisplaySourceBtn"));
    m_kiwiSdrDisplaySourceBtn->setAccessibleName(tr("Panadapter spectrum and waterfall source"));
    m_kiwiSdrDisplaySourceBtn->setAccessibleDescription(
        tr("Switches the displayed spectrum and waterfall between Flex and KiwiSDR. Audio and meters are unchanged."));
    m_kiwiSdrDisplaySourceBtn->setCheckable(true);
    m_kiwiSdrDisplaySourceBtn->setFixedSize(46, 22);
    m_kiwiSdrDisplaySourceBtn->setStyleSheet(kZoomBtnStyle);
    m_kiwiSdrDisplaySourceBtn->setCursor(Qt::PointingHandCursor);
    m_kiwiSdrDisplaySourceBtn->setToolTip(
        tr("Show Flex or KiwiSDR spectrum/waterfall. Audio and meters are unchanged."));
    m_kiwiSdrDisplaySourceBtn->hide();
    connect(m_kiwiSdrDisplaySourceBtn, &QPushButton::clicked,
            this, [this](bool checked) {
        emit kiwiSdrDisplaySourceRequested(checked);
    });

    m_zoomSegBtn  = makeBtn("S", QStringLiteral("panZoomSegBtn"),  QStringLiteral("Zoom to segment"));
    m_zoomBandBtn = makeBtn("B", QStringLiteral("panZoomBandBtn"), QStringLiteral("Zoom to band"));
    m_zoomOutBtn  = makeBtn("\u2212", QStringLiteral("panZoomOutBtn"), QStringLiteral("Zoom out"));  // minus sign U+2212
    m_zoomInBtn   = makeBtn("+", QStringLiteral("panZoomInBtn"),   QStringLiteral("Zoom in"));

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

        const bool previewed = updateFrequencyPreview(
            m_centerMhz, m_bandwidthMhz, newCenter, newBw);
        if (!previewed) {
            handleWaterfallFrequencyFrameChange(m_centerMhz, m_bandwidthMhz,
                                                newCenter, newBw);
        }
        if (!reprojectSpectrum(m_centerMhz, m_bandwidthMhz, newCenter, newBw)) {
            m_bins.clear();
            m_smoothed.clear();
            m_resetFftSmoothingOnNextFrame = true;
        }
        m_centerMhz = newCenter;
        m_bandwidthMhz = newBw;
        resetNoiseFloorBaseline();
        if (previewed) {
            scheduleFrequencyPreviewFrame();
        } else {
            markOverlayDirty();
        }
        scheduleFrequencyRangeSettleUpdate(newCenter, newBw);
        requestFrequencyRangeChange(newCenter, newBw);
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
    // QRhiWidget registers a cleanup callback with the current top-level
    // backing-store QRhi. Direct splitter/floating-window reparenting can miss
    // Qt's internal notification, leaving the old QRhi with a stale callback;
    // when that QRhi is later torn down, runCleanup() fires against a stale
    // QRhiWidgetPrivate and crashes during deferred event delivery (#2495).
    //
    // QEvent::WindowAboutToChangeInternal is cross-platform Qt machinery —
    // QRhiWidgetPrivate deregisters the callback the same way on Metal,
    // D3D/Vulkan and OpenGL — so this must fire on every GPU platform, not
    // just macOS. It was originally gated to Q_OS_MAC because #2495 was first
    // reproduced there; leaving Windows ungated let the identical crash slip
    // through on connect-time multi-panadapter restore (#3714). The send must
    // happen exactly once, before the reparent (refreshAfterReparent must not
    // re-send it — see PanadapterStack.cpp).
    QEvent event(QEvent::WindowAboutToChangeInternal);
    QCoreApplication::sendEvent(this, &event);
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

bool SpectrumWidget::vfoFlagOnLeftForSlice(
    int sliceId, double freqMhz, int panelWidth, bool previousOnLeft) const
{
    QVector<VfoPos> vfos;
    for (const SliceOverlay& overlay : m_sliceOverlays) {
        if (VfoWidget* widget = m_vfoWidgets.value(overlay.sliceId, nullptr)) {
            const double markerMhz = overlay.sliceId == sliceId ? freqMhz : overlay.freqMhz;
            int x = mhzToX(markerMhz);
            if (overlay.mode == "RTTY" || overlay.mode == "DIGL") {
                const double hiMhz = markerMhz + overlay.filterHighHz / 1.0e6;
                x = mhzToX(hiMhz) + 4;
            }
            vfos.append({overlay.sliceId, x, widget, overlay.splitPartnerId, &overlay});
        }
    }
    if (vfos.isEmpty()) {
        return previousOnLeft;
    }
    std::sort(vfos.begin(), vfos.end(), [](const VfoPos& a, const VfoPos& b) {
        return a.x < b.x;
    });

    int targetIndex = -1;
    for (int i = 0; i < vfos.size(); ++i) {
        if (vfos[i].sliceId == sliceId) {
            targetIndex = i;
            break;
        }
    }
    if (targetIndex < 0 || !vfos[targetIndex].overlay) {
        return previousOnLeft;
    }

    QMap<int, VfoWidget::FlagDir> dirMap;
    assignDiversityPairDirections(vfos, dirMap);
    assignSplitPairDirections(vfos, dirMap);
    assignModeForcedDirections(m_sliceOverlays, dirMap);

    const SliceOverlay& overlay = *vfos[targetIndex].overlay;
    if (dirMap.contains(sliceId)) {
        return flagDirectionOnLeft(dirMap[sliceId]);
    }

    VfoWidget::FlagDir dir = VfoWidget::Auto;
    // contentWidth(), not width(): the renderer flips flags at the content
    // edge (repositionVfoFlags / the paintEvent copy), so the predictor must
    // use the same edge — a width() prediction disagrees with the rendered
    // side for markers in the right DBM_STRIP_W band, and panFollowVfo then
    // extends the flag-width trigger on the wrong side (#3482).
    if (vfos.size() == 1) {
        const bool defaultOnLeft = VfoWidget::defaultFlagOnLeftForMode(overlay.mode);
        dir = VfoWidget::autoDirectionForSingleFlag(
            vfos[targetIndex].x, panelWidth, contentWidth(), defaultOnLeft,
            previousOnLeft);
    } else {
        const int deconflictPanelWidth = vfos.first().w ? vfos.first().w->width() : panelWidth;
        dir = deconflictedVfoFlagDirection(
            vfos, targetIndex, deconflictPanelWidth, contentWidth());
    }
    return flagDirectionOnLeft(dir);
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
    // These four values are stored by the radio (including in profiles). Older
    // releases persisted a competing client copy and reasserted it after status
    // updates (#2465, #4126). Remove the stale copies once; the member defaults
    // are only placeholders until the first PanadapterModel status arrives.
    bool removedRadioOwnedDisplaySetting = false;
    const QStringList radioOwnedDisplaySettings = {
        QStringLiteral("DisplayFftAverage"),
        QStringLiteral("DisplayFftFps"),
        QStringLiteral("DisplayFftWeightedAvg"),
        QStringLiteral("DisplayWfLineDuration"),
    };
    for (const QString& base : radioOwnedDisplaySettings) {
        const QString key = settingsKey(base);
        if (s.contains(key)) {
            s.remove(key);
            removedRadioOwnedDisplaySetting = true;
        }
    }
    if (removedRadioOwnedDisplaySetting) {
        s.save();
    }

    m_spectrumFrac   = std::clamp(s.value(settingsKey("SpectrumSplitRatio"), "0.40").toFloat(), 0.10f, 0.90f);
    m_fftFillAlpha   = s.value(settingsKey("DisplayFftFillAlpha"), "0.70").toFloat();
    const QString fillColorStr = s.value(settingsKey("DisplayFftFillColor"), "#00e5ff").toString();
    QColor parsed(fillColorStr);
    if (parsed.isValid())
        m_fftFillColor = parsed;
    // Trace line color is independent of the fill (#4239). Default matches the
    // legacy look where the line was drawn in the fill color (cyan).
    const QString lineColorStr = s.value(settingsKey("DisplayFftLineColor"), "#00e5ff").toString();
    QColor parsedLine(lineColorStr);
    if (parsedLine.isValid())
        m_fftLineColor = parsedLine;
    m_wfColorGain    = s.value(settingsKey("DisplayWfColorGain"), "50").toInt();
    m_wfBlackLevel   = s.value(settingsKey("DisplayWfBlackLevel"), "15").toInt();
    m_wfAutoBlack    = s.value(settingsKey("DisplayWfAutoBlack"), "True").toString() == "True";
    m_wfAutoBlackOffset = s.value(settingsKey("DisplayWfAutoBlackOffset"), "50").toInt();
    // Auto-black source defaults to client-side (legacy look); radio-side opt-in.
    m_wfAutoBlackRadioSide = s.value(settingsKey("DisplayWfAutoBlackRadioSide"), "False").toString() == "True";
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
    m_noiseFloorEnable = s.value(settingsKey("DisplayNoiseFloorEnable"), "False").toString() == "True";
    const int legacyNoiseFloorPosition = std::clamp(
        s.value(settingsKey("DisplayNoiseFloorPosition"), "75").toInt(), 1, 99);
    const int legacyDssFloorDepth = std::clamp(
        s.value(settingsKey("Display3DFloorDepth"), "6").toInt(), 0, 24);
    loadDisplaySourceTraceSettings(legacyNoiseFloorPosition,
                                   legacyDssFloorDepth);
    m_noiseFloorPosition = m_kiwiSdrWaterfallActive
        ? m_kiwiNoiseFloorPosition
        : m_flexNoiseFloorPosition;
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
    m_spectrumRenderMode = static_cast<SpectrumRenderMode>(
        std::clamp(s.value(settingsKey("DisplaySpectrumRenderMode"), "0").toInt(),
                   0, static_cast<int>(SpectrumRenderMode::Count) - 1));
    m_dssFloorOffsetDb = -static_cast<float>(
        m_kiwiSdrWaterfallActive ? m_kiwiDssFloorDepth : m_flexDssFloorDepth);
    m_singleClickTune = s.value("SingleClickTune", "False").toString() == "True";
    m_showTuneGuides  = s.value("ShowTuneGuides", "False").toString() == "True";
    m_extendedFrequencyLine = s.value("ExtendedFrequencyLine", "False").toString() == "True";
    m_extendedPassband = DisplaySettings::extendedPassband();

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
            m_fftLineWidth, m_wfAutoBlackRadioSide,
            static_cast<int>(m_spectrumRenderMode), dssFloorDepth(),
            m_dssGain, m_fftLineColor);
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
    w->setProperty("sliceId", sliceId);
    installVfoCursorEventFilter(w);
    // The flag's SmartMTR value labels are painted in our overlay pass; refresh
    // the overlay whenever they change or the flag moves.
    connect(w, &VfoWidget::smartMtrLabelsChanged, this,
            [this]() { markOverlayDirty("smartMtr"); });
    m_vfoWidgets[sliceId] = w;
    w->show();
    w->raise();
    applyActiveVfoZOrder();
    m_overlayMenu->raiseAll();  // keep overlay + panels on top of all VFO widgets
    raisePanadapterMessageOverlay();
    return w;
}

VfoWidget* SpectrumWidget::takeVfoWidget(int sliceId)
{
    VfoWidget* w = m_vfoWidgets.take(sliceId);
    if (!w) {
        return nullptr;
    }
    if (m_vfoWidget == w) {
        m_vfoWidget = nullptr;
    }
    disconnect(w, nullptr, this, nullptr);
    w->removeEventFilter(this);
    const QList<QWidget*> children = w->findChildren<QWidget*>();
    for (QWidget* child : children) {
        child->removeEventFilter(this);
    }
    w->setParent(nullptr);
    return w;
}

void SpectrumWidget::adoptVfoWidget(int sliceId, VfoWidget* widget)
{
    if (!widget) {
        return;
    }
    if (VfoWidget* existing = m_vfoWidgets.value(sliceId, nullptr)) {
        if (existing == widget) {
            return;
        }
        removeVfoWidget(sliceId);
    }
    widget->setParent(this);
    // The flag's close/lock/record/play buttons and collapsed freq label are
    // siblings parented to the SPECTRUM (so they render outside the flag's
    // bounds) — they must move with the flag or they ghost on the old pan
    // (#4037 review).
    widget->reparentFlagSatellites(this);
    widget->setProperty("sliceId", sliceId);
    installVfoCursorEventFilter(widget);
    connect(widget, &VfoWidget::smartMtrLabelsChanged, this,
            [this]() { markOverlayDirty("smartMtr"); });
    m_vfoWidgets[sliceId] = widget;
    widget->show();
    widget->raise();
    applyActiveVfoZOrder();
    m_overlayMenu->raiseAll();
    raisePanadapterMessageOverlay();
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
    applyActiveVfoZOrder();
}

void SpectrumWidget::applyActiveVfoZOrder()
{
    const SliceOverlay* active = activeOverlay();
    if (active && active->diversity) {
        for (const SliceOverlay& overlay : m_sliceOverlays) {
            if (overlay.sliceId == active->sliceId) {
                continue;
            }
            if (overlaysAreAttachedDiversityPair(active, overlay)) {
                if (VfoWidget* partner = m_vfoWidgets.value(overlay.sliceId, nullptr)) {
                    partner->raise();
                }
            }
        }
    }

    if (m_vfoWidget) {
        m_vfoWidget->raise();
    }

    if (m_overlayMenu) {
        m_overlayMenu->raiseAll();  // keep overlay above VFO
    }
    raisePanadapterMessageOverlay();
}

// ── Display control setters ──────────────────────────────────────────────────

void SpectrumWidget::setBandPlanManager(BandPlanManager* mgr) {
    m_bandPlanMgr = mgr;
    if (mgr)
        connect(mgr, &BandPlanManager::planChanged, this, QOverload<>::of(&QWidget::update));
}

void SpectrumWidget::setFftAverage(int frames) {
    m_fftAverage = frames;
    if (m_overlayMenu) {
        m_overlayMenu->syncPanProcessingSettings(
            m_fftAverage, m_fftFps, m_fftWeightedAvg);
    }
}

QString SpectrumWidget::displaySourceTraceSettingsKey() const
{
    return settingsKey(QStringLiteral("DisplaySourceTraceSettings"));
}

void SpectrumWidget::loadDisplaySourceTraceSettings(int legacyNoiseFloorPosition,
                                                    int legacyDssFloorDepth)
{
    m_flexNoiseFloorPosition = std::clamp(legacyNoiseFloorPosition, 1, 99);
    m_kiwiNoiseFloorPosition = m_flexNoiseFloorPosition;
    m_flexDssFloorDepth = static_cast<float>(
        std::clamp(legacyDssFloorDepth, 0, 24));
    m_kiwiDssFloorDepth = m_flexDssFloorDepth;

    const QString raw = AppSettings::instance()
        .value(displaySourceTraceSettingsKey(), QString()).toString();
    if (raw.trimmed().isEmpty()) {
        return;
    }

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcGui).noquote()
            << "SpectrumWidget: ignoring invalid DisplaySourceTraceSettings"
            << displaySourceTraceSettingsKey() << error.errorString();
        return;
    }

    const auto applySource = [](const QJsonObject& source,
                                int& noiseFloorPosition,
                                float& dssFloorDepth) {
        if (source.contains(QStringLiteral("noiseFloorPosition"))) {
            noiseFloorPosition = std::clamp(
                source.value(QStringLiteral("noiseFloorPosition")).toInt(
                    noiseFloorPosition),
                1, 99);
        }
        if (source.contains(QStringLiteral("dssFloorDepth"))) {
            dssFloorDepth = std::clamp(
                static_cast<float>(source.value(
                    QStringLiteral("dssFloorDepth")).toDouble(dssFloorDepth)),
                0.0f, 24.0f);
        }
    };

    const QJsonObject obj = doc.object();
    applySource(obj.value(QStringLiteral("flex")).toObject(),
                m_flexNoiseFloorPosition, m_flexDssFloorDepth);
    applySource(obj.value(QStringLiteral("kiwi")).toObject(),
                m_kiwiNoiseFloorPosition, m_kiwiDssFloorDepth);
}

void SpectrumWidget::saveDisplaySourceTraceSettings()
{
    auto sourceObject = [](int noiseFloorPosition, float dssFloorDepth) {
        QJsonObject source;
        source.insert(QStringLiteral("noiseFloorPosition"),
                      std::clamp(noiseFloorPosition, 1, 99));
        source.insert(QStringLiteral("dssFloorDepth"),
                      static_cast<double>(
                          std::clamp(dssFloorDepth, 0.0f, 24.0f)));
        return source;
    };

    QJsonObject obj;
    obj.insert(QStringLiteral("version"), 1);
    obj.insert(QStringLiteral("flex"),
               sourceObject(m_flexNoiseFloorPosition, m_flexDssFloorDepth));
    obj.insert(QStringLiteral("kiwi"),
               sourceObject(m_kiwiNoiseFloorPosition, m_kiwiDssFloorDepth));

    auto& s = AppSettings::instance();
    s.setValue(displaySourceTraceSettingsKey(), QString::fromUtf8(
        QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    s.save();
}

void SpectrumWidget::setNoiseFloorPositionForSource(bool kiwiSource,
                                                    int pos,
                                                    bool persist)
{
    const int clampedPosition = std::clamp(pos, 1, 99);
    if (kiwiSource) {
        m_kiwiNoiseFloorPosition = clampedPosition;
    } else {
        m_flexNoiseFloorPosition = clampedPosition;
    }

    if (kiwiSource == m_kiwiSdrWaterfallActive) {
        m_noiseFloorPosition = clampedPosition;
    }

    if (persist) {
        saveDisplaySourceTraceSettings();
    }
}

void SpectrumWidget::restoreNoiseFloorPositionForCurrentSource(bool syncMenu)
{
    m_noiseFloorPosition = m_kiwiSdrWaterfallActive
        ? m_kiwiNoiseFloorPosition
        : m_flexNoiseFloorPosition;
    refreshNoiseFloorTarget();
    if (syncMenu) {
        emit noiseFloorPositionResolved(m_noiseFloorPosition);
    }
}

void SpectrumWidget::setNoiseFloorPosition(int pos) {
    setNoiseFloorPositionForSource(m_kiwiSdrWaterfallActive, pos, true);
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
    m_dssFloorAnchorDbm = -1000.0f;
    m_dssFloorAnchorValid = false;
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
    const QVector<float>& bins = noiseFloorAutoLevelBins();
    if (m_noiseFloorEnable) {
        reacquireNoiseFloorLock();
    } else {
        m_measuredNoiseFloorDbm = -1000.0f;
        m_dssFloorAnchorDbm = -1000.0f;
        m_dssFloorAnchorValid = false;
    }

    if (bins.isEmpty()) {
        return;
    }

    const float frameFloor = estimateNoiseFloorDbm(bins);
    if (frameFloor > -500.0f) {
        m_measuredNoiseFloorDbm = frameFloor;
        if (!m_kiwiSdrWaterfallActive) {
            m_dssFloorAnchorDbm = frameFloor;
            m_dssFloorAnchorValid = true;
        }
    }
    if (!m_noiseFloorEnable) {
        return;
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
    if (m_overlayMenu) {
        m_overlayMenu->syncPanProcessingSettings(
            m_fftAverage, m_fftFps, m_fftWeightedAvg);
    }
}
void SpectrumWidget::setFftFps(int fps) {
    m_fftFps = fps;
    if (m_overlayMenu) {
        m_overlayMenu->syncPanProcessingSettings(
            m_fftAverage, m_fftFps, m_fftWeightedAvg);
    }
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

void SpectrumWidget::setFpsMeterSyncStatsProvider(std::function<QString()> provider) {
    m_fpsMeterSyncStatsProvider = std::move(provider);
    updateFpsMeterSyncStatsLabel(true);
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
    updateFpsMeterSyncStatsLabel(true);
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
    if (m_showFpsMeters) {
        ++m_panadapterFrameCount;
        updateFpsMeterSyncStatsLabel();
    }
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
    m_syncFpsMeterLabel = makeLabel();
    m_syncFpsMeterLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    updateFpsMeterLabels();
}

void SpectrumWidget::updateFpsMeterLabels() {
    if (!m_panFpsMeterLabel || !m_wfFpsMeterLabel || !m_syncFpsMeterLabel) {
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
    updateFpsMeterSyncStatsLabel(true);
    positionFpsMeterLabels();
}

void SpectrumWidget::updateFpsMeterSyncStatsLabel(bool force) {
    if (!m_syncFpsMeterLabel) {
        return;
    }
    if (!force && m_syncFpsMeterUpdateTimer.isValid()
        && m_syncFpsMeterUpdateTimer.elapsed() < 200) {
        return;
    }
    m_syncFpsMeterUpdateTimer.restart();

    if (!m_showFpsMeters || !m_fpsMeterSyncStatsProvider) {
        m_syncFpsMeterLabel->clear();
        m_syncFpsMeterLabel->hide();
        return;
    }

    const QString text = m_fpsMeterSyncStatsProvider();
    if (text.isEmpty()) {
        m_syncFpsMeterLabel->clear();
        m_syncFpsMeterLabel->hide();
        return;
    }

    if (m_syncFpsMeterLabel->text() != text) {
        m_syncFpsMeterLabel->setText(text);
        m_syncFpsMeterLabel->adjustSize();
    }
    positionFpsMeterLabels();
}

int SpectrumWidget::spectrumPixelHeight() const
{
    const int chromeH = freqScaleH() + DIVIDER_H;
    const int contentH = std::max(0, height() - chromeH);
    return std::max(1, static_cast<int>(contentH * m_spectrumFrac));
}

int SpectrumWidget::waterfallPixelHeight() const
{
    // Derived as the REMAINDER of the split, not as a second independent
    // truncation of contentH * (1 - m_spectrumFrac). Those two expressions are
    // not equivalent under integer truncation — they differ by one pixel for
    // most window heights — and using each in a different place is what caused
    // the waterfall image and its destination rect to disagree.
    const int chromeH = freqScaleH() + DIVIDER_H;
    const int contentH = std::max(0, height() - chromeH);
    return std::max(0, contentH - spectrumPixelHeight());
}

void SpectrumWidget::positionFpsMeterLabels() {
    if (!m_panFpsMeterLabel || !m_wfFpsMeterLabel || !m_syncFpsMeterLabel) {
        return;
    }

    auto hideMeters = [this]() {
        m_panFpsMeterLabel->hide();
        m_wfFpsMeterLabel->hide();
        m_syncFpsMeterLabel->hide();
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
                            int bottomInset, int rightInset) -> QRect {
        if (area.width() < 56 || area.height() < 18) {
            label->hide();
            return {};
        }

        const QSize labelSize = label->sizeHint();
        if (area.width() < labelSize.width() + rightInset + 12
            || area.height() < labelSize.height() + 8) {
            label->hide();
            return {};
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
        return QRect(QPoint(x, y), labelSize);
    };

    const int panBottomInset = (m_bandPlanFontSize > 0)
        ? m_bandPlanFontSize + 12
        : 6;
    const QRect panMeterRect =
        positionMeter(m_panFpsMeterLabel, specRect, panBottomInset, DBM_STRIP_W);
    if (m_syncFpsMeterLabel->text().isEmpty() || panMeterRect.isEmpty()) {
        m_syncFpsMeterLabel->hide();
    } else {
        const QSize syncSize = m_syncFpsMeterLabel->sizeHint();
        const int gap = 6;
        const int minX = specRect.left() + 4;
        const int minY = specRect.top() + 4;
        const int maxBottom = specRect.bottom() - 4;
        const int x = panMeterRect.left() - syncSize.width() - gap;
        int y = panMeterRect.bottom() - syncSize.height() + 1;
        if (y < minY) {
            y = minY;
        }
        if (y + syncSize.height() > maxBottom) {
            y = maxBottom - syncSize.height();
        }

        if (x < minX || y < minY
            || specRect.height() < syncSize.height() + 8) {
            m_syncFpsMeterLabel->hide();
        } else {
            m_syncFpsMeterLabel->move(x, y);
            m_syncFpsMeterLabel->show();
        }
    }
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
        || m_draggingDssFloor
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

float SpectrumWidget::estimateKiwiSdrVisualNoiseFloorDbm(
    const QVector<float>& bins) const
{
    if (bins.isEmpty()) {
        return -1000.0f;
    }

    const int stride = std::max(1, static_cast<int>(bins.size() / 512));
    QVector<float> finite;
    finite.reserve((bins.size() + stride - 1) / stride);
    for (int i = 0; i < bins.size(); i += stride) {
        const float v = bins[i];
        if (std::isfinite(v)) {
            finite.append(v);
        }
    }
    if (finite.isEmpty()) {
        return -1000.0f;
    }

    const int middle = finite.size() / 2;
    std::nth_element(finite.begin(), finite.begin() + middle, finite.end());
    return finite[middle];
}

float SpectrumWidget::estimateKiwiSdrTraceFloorDbm(
    const QVector<float>& bins) const
{
    return KiwiSdrTraceMath::estimateTraceFloorDbm(
        bins, kKiwiSdrWaterfallMinDbm);
}

void SpectrumWidget::stabilizeKiwiSdrFftTrace(QVector<float>& bins,
                                              bool allowFloorAdapt)
{
    KiwiSdrTraceMath::TraceFloorState state{
        m_kiwiSdrFftTraceFloorDbm,
        m_kiwiSdrFftTraceFloorValid
    };
    KiwiSdrTraceMath::stabilizeTraceFloor(
        bins, state, allowFloorAdapt,
        kKiwiSdrWaterfallMinDbm, kKiwiSdrWaterfallMaxDbm);
    m_kiwiSdrFftTraceFloorDbm = state.floorDbm;
    m_kiwiSdrFftTraceFloorValid = state.valid;
}

void SpectrumWidget::updateKiwiSdrSquelchVisualFloor(float floorDbm)
{
    if (m_kiwiSdrSquelchMeterFloorValid) {
        return;
    }
    if (!std::isfinite(floorDbm)) {
        return;
    }

    const float clamped = std::clamp(
        floorDbm, kKiwiSdrWaterfallMinDbm, kKiwiSdrWaterfallMaxDbm);
    if (m_kiwiSdrSquelchLiveFloorDbm <= -500.0f) {
        m_kiwiSdrSquelchLiveFloorDbm = clamped;
    } else {
        const float delta = clamped - m_kiwiSdrSquelchLiveFloorDbm;
        if (std::abs(delta) >= 0.10f) {
            const float alpha = delta > 0.0f ? 0.06f : 0.18f;
            m_kiwiSdrSquelchLiveFloorDbm =
                (1.0f - alpha) * m_kiwiSdrSquelchLiveFloorDbm
                + alpha * clamped;
        }
    }

    if (!m_kiwiSdrSquelchLineVisible) {
        return;
    }

    if (m_kiwiSdrSquelchLineFloorRelative) {
        markOverlayDirty();
    } else if (!m_kiwiSdrSquelchPinnedThresholdValid
               || !m_kiwiSdrSquelchPinnedDisplayNormValid) {
        pinKiwiSdrManualSquelchLine();
        markOverlayDirty();
    }
}

void SpectrumWidget::pinKiwiSdrManualSquelchLine()
{
    if (!m_kiwiSdrSquelchLineVisible
        || m_kiwiSdrSquelchLineFloorRelative
        || m_kiwiSdrSquelchLevel == KiwiSdrProtocol::kSquelchOffLevel) {
        return;
    }

    if (m_kiwiSdrSquelchLiveFloorDbm <= -500.0f) {
        m_kiwiSdrSquelchPinnedFloorDbm = -999.0f;
        m_kiwiSdrSquelchPinnedFloorValid = false;
        m_kiwiSdrSquelchPinnedThresholdDbm = -999.0f;
        m_kiwiSdrSquelchPinnedThresholdValid = false;
        m_kiwiSdrSquelchPinnedDisplayNorm = -1.0f;
        m_kiwiSdrSquelchPinnedDisplayNormValid = false;
        return;
    }

    m_kiwiSdrSquelchPinnedFloorDbm = m_kiwiSdrSquelchLiveFloorDbm;
    m_kiwiSdrSquelchPinnedFloorValid = true;
    m_kiwiSdrSquelchPinnedThresholdDbm =
        m_kiwiSdrSquelchPinnedFloorDbm
        + static_cast<float>(m_kiwiSdrSquelchLevel);
    m_kiwiSdrSquelchPinnedThresholdValid = true;
    if (m_dynamicRange > 0.0f) {
        m_kiwiSdrSquelchPinnedDisplayNorm = std::clamp(
            (m_refLevel - m_kiwiSdrSquelchPinnedThresholdDbm)
                / m_dynamicRange,
            0.0f, 1.0f);
        m_kiwiSdrSquelchPinnedDisplayNormValid = true;
    } else {
        m_kiwiSdrSquelchPinnedDisplayNorm = -1.0f;
        m_kiwiSdrSquelchPinnedDisplayNormValid = false;
    }
}

void SpectrumWidget::beginDbmRangeTransition(float oldMinDbm, float oldMaxDbm,
                                             float newMinDbm, float newMaxDbm)
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_pendingDbmRangeEcho = true;
    m_pendingDbmRangeEchoFromAutoFloor = false;
    m_pendingDbmRangeEchoStartMs = nowMs;
    m_pendingMinDbm = newMinDbm;
    m_pendingMaxDbm = newMaxDbm;
    m_dbmReleasePreviewOldMinDbm = oldMinDbm;
    m_dbmReleasePreviewOldMaxDbm = oldMaxDbm;
    m_dbmReleasePreviewNewMinDbm = newMinDbm;
    m_dbmReleasePreviewNewMaxDbm = newMaxDbm;
    m_dbmReleaseRebaseUntilMs =
        (std::abs(oldMinDbm - newMinDbm) > kDbmReleasePreviewChangeThresholdDb
         || std::abs(oldMaxDbm - newMaxDbm) > kDbmReleasePreviewChangeThresholdDb)
        ? nowMs + kDbmRangeHandshakeTimeoutMs
        : 0;
    m_resetFftSmoothingOnNextFrame = true;
}

void SpectrumWidget::clearDbmReleaseRebase()
{
    m_dbmReleaseRebaseUntilMs = 0;
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
    setNoiseFloorPositionForSource(m_kiwiSdrWaterfallActive, newPosition, persist);
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

void SpectrumWidget::setExtendedPassband(bool on) {
    m_extendedPassband = on;
    DisplaySettings::setExtendedPassband(on);
    markOverlayDirty();

    // Propagate to all sibling SpectrumWidgets so the toggle is global.
    if (QWidget* top = window()) {
        const auto siblings = top->findChildren<SpectrumWidget*>();
        for (SpectrumWidget* sw : siblings) {
            if (sw != this && sw->m_extendedPassband != on) {
                sw->m_extendedPassband = on;
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
void SpectrumWidget::setFftLineColor(const QColor& c) {
    m_fftLineColor = c;
    auto& s = AppSettings::instance();
    s.setValue(settingsKey("DisplayFftLineColor"), c.name());
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
    if (m_overlayMenu) {
        m_overlayMenu->syncWfLineDuration(m_wfLineDuration);
    }
    // Re-calibrate the time scale for the new rate
    resetWfTimeScale();
    markOverlayDirty();
}

void SpectrumWidget::setSquelchLine(bool visible, int level)
{
    m_flexSquelchLineVisible = visible;
    m_flexSquelchLevel = std::clamp(level, 0, 100);
    markOverlayDirty();
    // Manual SQL: 3 s auto-hide; each slider adjustment restarts the timer.
    // Auto SQL: line stays pinned to the tracked floor level — no timer.
    if (m_squelchLineHideTimer) {
        if (visible && !m_autoSquelchEnabled && !m_kiwiSdrWaterfallActive) {
            m_squelchLineHideTimer->start();
        } else if (!m_kiwiSdrWaterfallActive) {
            m_squelchLineHideTimer->stop();
        }
    }
}

void SpectrumWidget::setKiwiSdrSquelchMeterDbm(float dbm, bool squelched)
{
    if (!std::isfinite(dbm)) {
        return;
    }

    const float clamped = std::clamp(dbm, -127.0f, 3.4f);
    constexpr int kServerRssiMedianSamples = 65;
    const bool shouldSample =
        m_kiwiSdrSquelchMeterSamples.size() < kServerRssiMedianSamples
        || squelched
        || !m_kiwiSdrSquelchLineVisible
        || m_kiwiSdrSquelchLineFloorRelative;
    if (shouldSample) {
        m_kiwiSdrSquelchMeterSamples.append(clamped);
        while (m_kiwiSdrSquelchMeterSamples.size()
               > kServerRssiMedianSamples) {
            m_kiwiSdrSquelchMeterSamples.removeFirst();
        }
    }

    if (m_kiwiSdrSquelchMeterSamples.isEmpty()) {
        return;
    }

    QVector<float> sorted = m_kiwiSdrSquelchMeterSamples;
    const int middle = sorted.size() / 2;
    std::nth_element(sorted.begin(), sorted.begin() + middle, sorted.end());
    const float floorDbm = sorted[middle];
    m_kiwiSdrSquelchMeterFloorValid = true;
    if (m_kiwiSdrSquelchLiveFloorDbm <= -500.0f) {
        m_kiwiSdrSquelchLiveFloorDbm = floorDbm;
    } else {
        const float delta = floorDbm - m_kiwiSdrSquelchLiveFloorDbm;
        if (std::abs(delta) >= 0.10f) {
            const float alpha = delta > 0.0f ? 0.06f : 0.18f;
            m_kiwiSdrSquelchLiveFloorDbm =
                (1.0f - alpha) * m_kiwiSdrSquelchLiveFloorDbm
                + alpha * floorDbm;
        }
    }

    if (!m_kiwiSdrSquelchLineVisible) {
        return;
    }
    if (m_kiwiSdrSquelchLineFloorRelative) {
        markOverlayDirty();
    } else if (!m_kiwiSdrSquelchPinnedThresholdValid
               || !m_kiwiSdrSquelchPinnedDisplayNormValid) {
        pinKiwiSdrManualSquelchLine();
        markOverlayDirty();
    }
}

void SpectrumWidget::setKiwiSdrSquelchLine(bool visible, int marginDb,
                                           bool floorRelative)
{
    const bool wasVisible = m_kiwiSdrSquelchLineVisible;
    const int previousLevel = m_kiwiSdrSquelchLevel;
    const bool wasFloorRelative = m_kiwiSdrSquelchLineFloorRelative;
    const int clampedLevel = std::clamp(
        marginDb, KiwiSdrProtocol::kSquelchServerMinMarginDb,
        KiwiSdrProtocol::kSquelchServerMaxMarginDb);
    m_kiwiSdrSquelchLevel = clampedLevel;
    m_kiwiSdrSquelchLineVisible =
        visible && m_kiwiSdrSquelchLevel != KiwiSdrProtocol::kSquelchOffLevel;
    m_kiwiSdrSquelchLineFloorRelative = floorRelative;
    const bool shouldPin =
        m_kiwiSdrSquelchLineVisible
        && !floorRelative
        && (!wasVisible || wasFloorRelative || previousLevel != clampedLevel
            || !m_kiwiSdrSquelchPinnedThresholdValid);
    if (shouldPin) {
        pinKiwiSdrManualSquelchLine();
    } else if (!m_kiwiSdrSquelchLineVisible) {
        m_kiwiSdrSquelchPinnedFloorDbm = -999.0f;
        m_kiwiSdrSquelchPinnedFloorValid = false;
        m_kiwiSdrSquelchPinnedThresholdDbm = -999.0f;
        m_kiwiSdrSquelchPinnedThresholdValid = false;
        m_kiwiSdrSquelchPinnedDisplayNorm = -1.0f;
        m_kiwiSdrSquelchPinnedDisplayNormValid = false;
    }
    m_flexSquelchLineVisible = false;
    m_flexSquelchLevel = 0;
    markOverlayDirty();
    if (m_squelchLineHideTimer) {
        m_squelchLineHideTimer->stop();
    }
}

void SpectrumWidget::clearKiwiSdrSquelchLine()
{
    if (!m_kiwiSdrSquelchLineVisible && m_kiwiSdrSquelchLevel == 0
        && m_sqlNoiseFloorDbm <= -500.0f
        && m_kiwiSdrSquelchLiveFloorDbm <= -500.0f) {
        return;
    }
    m_kiwiSdrSquelchLineVisible = false;
    m_kiwiSdrSquelchLevel = 0;
    m_kiwiSdrSquelchLineFloorRelative = false;
    m_sqlNoiseFloorDbm = -999.0f;
    m_kiwiSdrSquelchLiveFloorDbm = -999.0f;
    m_kiwiSdrSquelchPinnedFloorDbm = -999.0f;
    m_kiwiSdrSquelchPinnedFloorValid = false;
    m_kiwiSdrSquelchPinnedThresholdDbm = -999.0f;
    m_kiwiSdrSquelchPinnedThresholdValid = false;
    m_kiwiSdrSquelchPinnedDisplayNorm = -1.0f;
    m_kiwiSdrSquelchPinnedDisplayNormValid = false;
    m_kiwiSdrSquelchMeterFloorValid = false;
    m_kiwiSdrSquelchMeterSamples.clear();
    markOverlayDirty();
    if (m_squelchLineHideTimer) {
        m_squelchLineHideTimer->stop();
    }
}

void SpectrumWidget::drawAutoSqlFloor(QPainter& p, const QRect& specRect)
{
    const bool useKiwiFloor =
        m_kiwiSdrWaterfallActive || m_kiwiSdrSquelchLineFloorRelative;
    const float floorDbm = useKiwiFloor
        ? m_kiwiSdrSquelchLiveFloorDbm
        : m_sqlNoiseFloorDbm;
    if (!m_autoSquelchEnabled || floorDbm <= -500.0f) { return; }
    const float norm = (m_refLevel - floorDbm) / m_dynamicRange;
    const int y = specRect.top()
        + static_cast<int>(std::clamp(norm, 0.0f, 1.0f) * specRect.height());
    p.setPen(QPen(AetherSDR::theme::withAlpha("color.accent.warning", 200), 1, Qt::DashLine));
    p.drawLine(specRect.left(), y, specRect.right(), y);
    QFont f = p.font();
    f.setPointSize(8);
    f.setBold(true);
    p.setFont(f);
    p.setPen(AetherSDR::theme::withAlpha("color.accent.warning", 200));
    const QString lbl = QString("Floor %1 dBm").arg(static_cast<int>(floorDbm));
    p.drawText(specRect.right() - p.fontMetrics().horizontalAdvance(lbl) - 4, y - 2, lbl);
}

void SpectrumWidget::drawSquelchLine(QPainter& p, const QRect& specRect)
{
    constexpr float kSqlMinDbm = -160.0f;
    float squelchDbm = 0.0f;
    float pinnedDisplayNorm = 0.0f;
    bool usePinnedDisplayNorm = false;
    QString label;
    if (m_kiwiSdrSquelchLineVisible) {
        if (m_kiwiSdrSquelchLevel == KiwiSdrProtocol::kSquelchOffLevel) {
            return;
        }
        if (m_kiwiSdrSquelchLineFloorRelative) {
            const float floorDbm = m_kiwiSdrSquelchLiveFloorDbm;
            if (floorDbm <= -500.0f) {
                return;
            }
            squelchDbm = floorDbm + static_cast<float>(m_kiwiSdrSquelchLevel);
        } else {
            if (!m_kiwiSdrSquelchPinnedThresholdValid
                || m_kiwiSdrSquelchPinnedThresholdDbm <= -500.0f) {
                return;
            }
            squelchDbm = m_kiwiSdrSquelchPinnedThresholdDbm;
            if (m_kiwiSdrSquelchPinnedDisplayNormValid) {
                pinnedDisplayNorm = m_kiwiSdrSquelchPinnedDisplayNorm;
                usePinnedDisplayNorm = true;
            }
        }
        label = QStringLiteral("SQL %1%2 dB")
            .arg(m_kiwiSdrSquelchLevel > 0 ? QStringLiteral("+")
                                            : QString())
            .arg(m_kiwiSdrSquelchLevel);
    } else {
        if (!m_flexSquelchLineVisible || m_flexSquelchLevel <= 0) {
            return;
        }
        squelchDbm = kSqlMinDbm + static_cast<float>(m_flexSquelchLevel);
        label = QStringLiteral("SQL %1").arg(m_flexSquelchLevel);
    }

    const float norm = usePinnedDisplayNorm
        ? pinnedDisplayNorm
        : (m_refLevel - squelchDbm) / m_dynamicRange;
    const int y = specRect.top()
        + static_cast<int>(std::clamp(norm, 0.0f, 1.0f) * specRect.height());
    p.setPen(QPen(AetherSDR::theme::withAlpha("color.accent.warning", 220), 1));
    p.drawLine(specRect.left(), y, specRect.right(), y);
    QFont f = p.font();
    f.setPointSize(8);
    f.setBold(true);
    p.setFont(f);
    p.setPen(AetherSDR::theme::withAlpha("color.accent.warning", 220));
    p.drawText(4, y - 2, label);
}

void SpectrumWidget::setAutoSquelchEnable(bool on)
{
    if (m_autoSquelchEnabled == on) {
        return;
    }

    m_autoSquelchEnabled = on;
    if (on) {
        m_sqlNoiseFloorDbm = -999.0f;  // cold-start the floor EWMA on enable
        m_lastAutoSquelchLevel = -1;
    }
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

void SpectrumWidget::updateAutoSquelchFromBins(const QVector<float>& binsDbm)
{
    if (!m_autoSquelchEnabled || m_transmitting || binsDbm.isEmpty()) {
        return;
    }

    if (m_kiwiSdrWaterfallActive || m_kiwiSdrSquelchLineFloorRelative) {
        // Kiwi non-NBFM squelch is server-relative:
        // SET squelch=N opens at median RSSI + N dB. Do not apply the Flex
        // absolute dBm conversion here.
        const int level = m_autoSqlMarginDb;
        if (level != m_lastAutoSquelchLevel) {
            m_lastAutoSquelchLevel = level;
            emit autoSquelchLevelSuggested(level);
        }
        return;
    }

    float sum1 = 0.0f;
    int cnt1 = 0;
    for (int j = 0; j < binsDbm.size(); j += 4) {
        sum1 += binsDbm[j];
        ++cnt1;
    }
    if (cnt1 <= 0) {
        return;
    }
    const float mean1 = sum1 / static_cast<float>(cnt1);

    float sum2 = 0.0f;
    int cnt2 = 0;
    for (int j = 0; j < binsDbm.size(); j += 4) {
        if (binsDbm[j] <= mean1) {
            sum2 += binsDbm[j];
            ++cnt2;
        }
    }
    const float frameFloor =
        (cnt2 > 0) ? sum2 / static_cast<float>(cnt2) : mean1;
    m_sqlNoiseFloorDbm =
        (m_sqlNoiseFloorDbm <= -500.0f)
            ? frameFloor
            : 0.1f * frameFloor + 0.9f * m_sqlNoiseFloorDbm;

    constexpr float kSqlMinDbm = -160.0f;
    const float targetDbm =
        m_sqlNoiseFloorDbm + static_cast<float>(m_autoSqlMarginDb);
    const int level = std::clamp(
        static_cast<int>(targetDbm - kSqlMinDbm + 0.5f), 1, 100);
    if (level != m_lastAutoSquelchLevel) {
        m_lastAutoSquelchLevel = level;
        emit autoSquelchLevelSuggested(level);
    }
    markOverlayDirty();
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

void SpectrumWidget::setDssFloorDepthForSource(bool kiwiSource,
                                               float dB,
                                               bool persist)
{
    const float clampedDepth = std::clamp(dB, 0.0f, 24.0f);
    if (kiwiSource) {
        m_kiwiDssFloorDepth = clampedDepth;
    } else {
        m_flexDssFloorDepth = clampedDepth;
    }

    if (persist) {
        saveDisplaySourceTraceSettings();
    }

    if (kiwiSource != m_kiwiSdrWaterfallActive) {
        return;
    }

    const int previousResolvedDepth = dssFloorDepth();
    const float offsetDb = -clampedDepth;
    if (offsetDb != m_dssFloorOffsetDb) {
        m_dssFloorOffsetDb = offsetDb;
        m_dss.invalidate();   // CPU fallback cache; mesh re-reads each frame
        // In 3D mode the visible dBm markings are anchored to this floor, so the
        // cached overlay must redraw even when the span is stable.
        markOverlayDirty();
        const int resolvedDepth = dssFloorDepth();
        if (resolvedDepth != previousResolvedDepth) {
            emit dssFloorDepthResolved(resolvedDepth);
        }
    }
    update();
}

void SpectrumWidget::restoreDssFloorDepthForCurrentSource(bool syncMenu)
{
    const float depth = m_kiwiSdrWaterfallActive
        ? m_kiwiDssFloorDepth
        : m_flexDssFloorDepth;
    const float offsetDb = -depth;
    if (offsetDb != m_dssFloorOffsetDb) {
        m_dssFloorOffsetDb = offsetDb;
        m_dss.invalidate();
        markOverlayDirty();
    }
    if (syncMenu) {
        emit dssFloorDepthResolved(
            static_cast<int>(std::lround(depth)));
    }
    update();
}

void SpectrumWidget::setDssFloorDepth(int dB) {
    setDssFloorDepthForSource(m_kiwiSdrWaterfallActive, dB, true);
}

void SpectrumWidget::setDssGain(int pct) {
    pct = std::clamp(pct, 0, 100);
    if (pct != m_dssGain) {
        m_dssGain = pct;
        auto& s = AppSettings::instance();
        s.setValue(settingsKey("Display3DGain"), QString::number(pct));
        s.save();
#ifdef AETHER_GPU_SPECTRUM
        m_dssLutToken = ~0ull;  // force the GPU palette LUT to re-bake next frame
#endif
        m_dss.invalidate();     // CPU fallback surface re-colours too
    }
    update();
}

void SpectrumWidget::setSpectrumRenderMode(int mode) {
    auto clamped = static_cast<SpectrumRenderMode>(
        std::clamp(mode, 0, static_cast<int>(SpectrumRenderMode::Count) - 1));
    if (clamped != m_spectrumRenderMode) {
        commitFrequencyPreview();
        m_spectrumRenderMode = clamped;
        auto& s = AppSettings::instance();
        s.setValue(settingsKey("DisplaySpectrumRenderMode"),
                   QString::number(static_cast<int>(m_spectrumRenderMode)));
        s.save();
        if (m_spectrumRenderMode == SpectrumRenderMode::Mode3D) {
            // Allocate retained DSS scrollback only while it can be displayed.
            ensureWaterfallHistory();
        } else {
            m_dss.setHistoryCapacityRows(0);
            m_nativeWaterfallState.dss.setHistoryCapacityRows(0);
            m_kiwiWaterfallState.dss.setHistoryCapacityRows(0);
            for (auto it = m_kiwiProfileWaterfallStates.begin();
                 it != m_kiwiProfileWaterfallStates.end(); ++it) {
                it->dss.setHistoryCapacityRows(0);
            }
        }
        // Force a full rebuild of the 3DSS surface + its GPU texture, and the
        // overlay (grid/scales differ between modes).
        m_dss.invalidate();
#ifdef AETHER_GPU_SPECTRUM
        m_dssTexNeedsUpload = true;
#endif
        markOverlayDirty();
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
    if (!m_waterfallHistory.isConfigured()
        || ageRows < 0 || ageRows >= m_wfHistoryRowCount) {
        return -1;
    }
    return (m_wfHistoryWriteRow + ageRows)
        % m_waterfallHistory.capacityRows();
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
    if (!m_waterfallHistory.isConfigured() || m_wfHistoryTimestamps.isEmpty()
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

    const bool retainDssHistory =
        m_waterfallWriteVisible
        && m_spectrumRenderMode == SpectrumRenderMode::Mode3D;
    if (m_waterfallHistory.size() == desiredSize) {
        m_dss.setHistoryCapacityRows(retainDssHistory ? desiredSize.height() : 0);
        return;
    }

    // Preserve allocated chunks across width changes (e.g. band stack toggle,
    // settled window resize). Unreached history remains unallocated.
    const bool resizedExisting = m_waterfallHistory.isConfigured()
        && m_waterfallHistory.capacityRows() == desiredSize.height()
        && m_waterfallHistory.resizeWidth(desiredSize.width());
    if (!resizedExisting) {
        m_waterfallHistory.configure(
            desiredSize.width(), desiredSize.height());
        m_wfHistoryTimestamps = QVector<qint64>(desiredSize.height(), 0);
        m_wfHistoryRowCenterMhz = QVector<double>(desiredSize.height(), 0.0);
        m_wfHistoryRowBwMhz = QVector<double>(desiredSize.height(), 0.0);
        m_wfHistoryWriteRow = 0;
        m_wfHistoryRowCount = 0;
        m_wfHistoryOffsetRows = 0;
        m_wfLive = true;
    }
    m_dss.setHistoryCapacityRows(retainDssHistory ? desiredSize.height() : 0);
}

float SpectrumWidget::dssHistoryFallbackDbm() const
{
    return m_kiwiSdrWaterfallActive
        ? kKiwiSdrWaterfallMinDbm
        : m_refLevel - m_dynamicRange;
}

void SpectrumWidget::appendDssHistoryRow(const QVector<float>& binsDbm,
                                         double frameCenterMhz,
                                         double frameBandwidthMhz)
{
    if (!m_waterfallWriteVisible
        || m_spectrumRenderMode != SpectrumRenderMode::Mode3D) {
        return;
    }
    const FrequencyFrame requestedFrame{frameCenterMhz, frameBandwidthMhz};
    const FrequencyFrame stampFrame = requestedFrame.isValid()
        ? requestedFrame
        : FrequencyFrame{m_centerMhz, m_bandwidthMhz};
    retainDssHistoryRow(m_dss, binsDbm,
                        stampFrame.centerMhz, stampFrame.bandwidthMhz,
                        dssHistoryFallbackDbm());
}

void SpectrumWidget::appendDssWaterfallRow(const QVector<float>& binsDbm,
                                           double frameCenterMhz,
                                           double frameBandwidthMhz,
                                           bool updateLiveSurface)
{
    // Keep live DSS and retained scrollback DSS on the same waterfall-row cadence.
    if (m_wfLive && updateLiveSurface) {
        if (m_frequencyPreviewActive && m_waterfallWriteVisible) {
            const QVector<float>& previewBins = remapPreviewDssRow(
                binsDbm, frameCenterMhz, frameBandwidthMhz);
            pushDssLiveRow(m_dss, previewBins, false);
            ++m_frequencyPreviewRemappedDssRows;
        } else {
            pushDssLiveRow(m_dss, binsDbm, !m_waterfallWriteVisible);
        }
    }
    appendDssHistoryRow(binsDbm, frameCenterMhz, frameBandwidthMhz);
}

void SpectrumWidget::pushDssLiveRow(DssRenderer& dss,
                                    const QVector<float>& binsDbm,
                                    bool hiddenStream)
{
    QElapsedTimer timer;
    timer.start();
    dss.pushRow(binsDbm);
    m_panStats.dssLiveUs += static_cast<quint64>(timer.nsecsElapsed() / 1000);
    ++m_panStats.dssLiveRows;
    if (hiddenStream) {
        ++m_panStats.dssHiddenLiveRows;
    }
}

void SpectrumWidget::retainDssHistoryRow(DssRenderer& dss,
                                         const QVector<float>& binsDbm,
                                         double centerMhz,
                                         double bandwidthMhz,
                                         float fallbackDbm)
{
    // Hidden sources release their retained DSS history (capacity 0), so this
    // early-returns for them and only the visible source retains scrollback —
    // there is deliberately no "hidden history" to count (#4081/#4083).
    if (dss.historyCapacityRows() <= 0) {
        return;
    }
    QElapsedTimer timer;
    timer.start();
    dss.appendHistoryRow(binsDbm, centerMhz, bandwidthMhz, fallbackDbm);
    m_panStats.dssHistoryUs += static_cast<quint64>(timer.nsecsElapsed() / 1000);
    ++m_panStats.dssHistoryRows;
}

void SpectrumWidget::appendLatestDssWaterfallRow(double frameCenterMhz,
                                                 double frameBandwidthMhz)
{
    appendDssWaterfallRow(displaySpectrumBins(), frameCenterMhz, frameBandwidthMhz);
}

void SpectrumWidget::appendVisibleRow(const QRgb* rowData,
                                      double frameCenterMhz,
                                      double frameBandwidthMhz)
{
    if (m_waterfall.isNull() || rowData == nullptr) {
        return;
    }

    const int h = m_waterfall.height();
    if (h <= 0) {
        return;
    }

    const FrequencyFrame requestedFrame{frameCenterMhz, frameBandwidthMhz};
    const FrequencyFrame rowFrame = requestedFrame.isValid()
        ? requestedFrame
        : FrequencyFrame{m_centerMhz, m_bandwidthMhz};

    QElapsedTimer timer;
    timer.start();
    m_wfWriteRow = (m_wfWriteRow - 1 + h) % h;
    auto* row = reinterpret_cast<QRgb*>(m_waterfall.bits() + m_wfWriteRow * m_waterfall.bytesPerLine());
    std::memcpy(row, rowData, m_waterfall.width() * sizeof(QRgb));
    if (m_wfVisibleRowCenterMhz.size() != h
        || m_wfVisibleRowBwMhz.size() != h) {
        resetVisibleWaterfallFrequencyFrames(m_centerMhz, m_bandwidthMhz);
    }
    m_wfVisibleRowCenterMhz[m_wfWriteRow] = rowFrame.centerMhz;
    m_wfVisibleRowBwMhz[m_wfWriteRow] = rowFrame.bandwidthMhz;
#ifdef AETHER_GPU_SPECTRUM
    m_wfFrameTexDirty = true;
#endif
    if (m_frequencyPreviewActive && m_waterfallWriteVisible) {
        ++m_frequencyPreviewNativeVisibleRows;
    }
    m_panStats.waterfallVisibleRowUs +=
        static_cast<quint64>(timer.nsecsElapsed() / 1000);
    ++m_panStats.waterfallVisibleRows;
    if (PerfTelemetry::instance().enabled())
        PerfTelemetry::instance().recordWaterfallVisibleRows();
}

void SpectrumWidget::appendHistoryRow(const quint8* intensityData,
                                      qint64 timestampMs,
                                      double frameCenterMhz,
                                      double frameBandwidthMhz)
{
    // A hidden Flex/Kiwi source keeps only its small viewport and 96-row live
    // DSS surface warm. Retained scrollback belongs to the visible source; it
    // is rebuilt from new rows after a source switch (#4081, #4083).
    if (!m_waterfallWriteVisible) {
        return;
    }

    QElapsedTimer timer;
    timer.start();
    ensureWaterfallHistory();
    if (!m_waterfallHistory.isConfigured() || intensityData == nullptr) {
        return;
    }

    const int h = m_waterfallHistory.capacityRows();
    m_wfHistoryWriteRow = (m_wfHistoryWriteRow - 1 + h) % h;
    quint8* row = m_waterfallHistory.writableRow(m_wfHistoryWriteRow);
    if (!row) {
        return;
    }
    std::memcpy(row, intensityData, m_waterfallHistory.width());
    if (m_wfHistoryWriteRow >= 0 && m_wfHistoryWriteRow < m_wfHistoryTimestamps.size()) {
        m_wfHistoryTimestamps[m_wfHistoryWriteRow] = timestampMs;
    }
    // Stamp the frequency frame this row was captured in, so the viewport can
    // remap it later regardless of how the center/bandwidth has since panned.
    const FrequencyFrame requestedFrame{frameCenterMhz, frameBandwidthMhz};
    const FrequencyFrame stampFrame = requestedFrame.isValid()
        ? requestedFrame
        : FrequencyFrame{m_centerMhz, m_bandwidthMhz};
    if (m_wfHistoryWriteRow >= 0 && m_wfHistoryWriteRow < m_wfHistoryRowCenterMhz.size()) {
        m_wfHistoryRowCenterMhz[m_wfHistoryWriteRow] = stampFrame.centerMhz;
        m_wfHistoryRowBwMhz[m_wfHistoryWriteRow] = stampFrame.bandwidthMhz;
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
    m_panStats.waterfallHistoryRowUs +=
        static_cast<quint64>(timer.nsecsElapsed() / 1000);
    ++m_panStats.waterfallHistoryRows;
    // No hidden-history counter: appendHistoryRow early-returns for a hidden
    // source above, so compact intensity history is only written for the
    // visible one.
}

// Copy one history scanline into the viewport, remapping its columns from the
// frame it was captured in (rowCenter/rowBw) to the current frame (curCenter/
// curBw). When the frames match (no pan/zoom since capture) this is a plain
// colorized copy; otherwise it is a horizontal resample and newly-exposed
// columns are black. Kiwi rows can be narrower than the current viewport after
// a zoom-out, so its path preserves the strongest normalized source sample
// covered by each destination column instead of point-sampling carriers out of
// existence.
static quint8 peakPreservedWaterfallSample(const quint8* src, int w,
                                           double srcLeft, double srcRight,
                                           double srcCenter)
{
    if (srcRight <= 0.0 || srcLeft >= static_cast<double>(w)) {
        return 0;
    }

    const double clampedLeft = std::clamp(srcLeft, 0.0, static_cast<double>(w));
    const double clampedRight = std::clamp(srcRight, 0.0, static_cast<double>(w));
    if (clampedRight - clampedLeft <= 1.0) {
        return (srcCenter < 0.0 || srcCenter >= static_cast<double>(w))
            ? 0
            : src[static_cast<int>(srcCenter)];
    }

    const int first = std::clamp(static_cast<int>(std::floor(clampedLeft)), 0, w - 1);
    const int last = std::clamp(static_cast<int>(std::ceil(clampedRight)) - 1, 0, w - 1);
    quint8 best = src[first];
    for (int i = first + 1; i <= last; ++i) {
        if (src[i] > best) {
            best = src[i];
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

const QVector<float>& SpectrumWidget::remapPreviewDssRow(
    const QVector<float>& binsDbm,
    double frameCenterMhz,
    double frameBandwidthMhz)
{
    if (!m_frequencyPreviewActive || binsDbm.isEmpty()) {
        return binsDbm;
    }

    const bool hasFrame = frameCenterMhz > 0.0 && frameBandwidthMhz > 0.0;
    const double sourceCenterMhz = hasFrame ? frameCenterMhz : m_centerMhz;
    const double sourceBandwidthMhz = hasFrame
        ? frameBandwidthMhz
        : m_bandwidthMhz;
    const double destinationCenterMhz = m_frequencyPreviewBaseCenterMhz;
    const double destinationBandwidthMhz =
        m_frequencyPreviewBaseBandwidthMhz;
    if (sourceBandwidthMhz <= 0.0 || destinationBandwidthMhz <= 0.0
        || !std::isfinite(sourceCenterMhz)
        || !std::isfinite(sourceBandwidthMhz)
        || !std::isfinite(destinationCenterMhz)
        || !std::isfinite(destinationBandwidthMhz)) {
        return binsDbm;
    }
    if (mhzNearlyEqual(sourceCenterMhz, destinationCenterMhz)
        && mhzNearlyEqual(sourceBandwidthMhz,
                          destinationBandwidthMhz)) {
        return binsDbm;
    }

    const int count = binsDbm.size();
    const double sourceStartMhz = sourceCenterMhz
        - sourceBandwidthMhz * 0.5;
    const double destinationStartMhz = destinationCenterMhz
        - destinationBandwidthMhz * 0.5;
    const float fallback = dssHistoryFallbackDbm();
    m_frequencyPreviewDssScratch.fill(fallback, count);
    for (int destination = 0; destination < count; ++destination) {
        const double destinationLeftMhz = destinationStartMhz
            + static_cast<double>(destination) / count
                * destinationBandwidthMhz;
        const double destinationRightMhz = destinationStartMhz
            + static_cast<double>(destination + 1) / count
                * destinationBandwidthMhz;
        const double destinationCenterBinMhz =
            (destinationLeftMhz + destinationRightMhz) * 0.5;
        const double sourceLeft = (destinationLeftMhz - sourceStartMhz)
            / sourceBandwidthMhz * count;
        const double sourceRight = (destinationRightMhz - sourceStartMhz)
            / sourceBandwidthMhz * count;
        const double sourceCenter =
            (destinationCenterBinMhz - sourceStartMhz)
                / sourceBandwidthMhz * count - 0.5;
        m_frequencyPreviewDssScratch[destination] = peakPreservedBinSample(
            binsDbm, sourceLeft, sourceRight, sourceCenter, fallback);
    }
    return m_frequencyPreviewDssScratch;
}

static void remapHistoryRowInto(QRgb* dst, const quint8* src, int w,
                                const std::array<QRgb, 256>& colorLut,
                                double rowCenterMhz, double rowBwMhz,
                                double curCenterMhz, double curBwMhz,
                                bool preservePeaks)
{
    if (rowBwMhz <= 0.0 || curBwMhz <= 0.0
        || (rowCenterMhz == curCenterMhz && rowBwMhz == curBwMhz)) {
        for (int x = 0; x < w; ++x) {
            dst[x] = colorLut[src[x]];
        }
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
                : colorLut[src[static_cast<int>(srcX)]];
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
        dst[x] = colorLut[peakPreservedWaterfallSample(
            src, w, srcLeft, srcRight, srcX)];
    }
}

void SpectrumWidget::resetVisibleWaterfallFrequencyFrames(
    double centerMhz,
    double bandwidthMhz)
{
    const int height = m_waterfall.height();
    if (height <= 0) {
        m_wfVisibleRowCenterMhz.clear();
        m_wfVisibleRowBwMhz.clear();
#ifdef AETHER_GPU_SPECTRUM
        m_wfFrameTexDirty = true;
#endif
        return;
    }

    const double validCenterMhz = std::isfinite(centerMhz) && centerMhz > 0.0
        ? centerMhz
        : m_centerMhz;
    const double validBandwidthMhz = std::isfinite(bandwidthMhz)
            && bandwidthMhz > 0.0
        ? bandwidthMhz
        : m_bandwidthMhz;
    m_wfVisibleRowCenterMhz.fill(validCenterMhz, height);
    m_wfVisibleRowBwMhz.fill(validBandwidthMhz, height);
#ifdef AETHER_GPU_SPECTRUM
    m_wfFrameTexDirty = true;
#endif
}

#ifdef AETHER_GPU_SPECTRUM
void SpectrumWidget::prepareWaterfallFrameUpload()
{
    const int visibleHeight = m_waterfall.height();
    const int uploadHeight = std::max(visibleHeight, m_wfGpuTexH);
    if (uploadHeight <= 0) {
        m_wfFrameUpload.clear();
        return;
    }
    if (m_wfVisibleRowCenterMhz.size() != visibleHeight
        || m_wfVisibleRowBwMhz.size() != visibleHeight) {
        resetVisibleWaterfallFrequencyFrames(m_centerMhz, m_bandwidthMhz);
    }

    // Store center as an offset from a nearby double-precision reference.
    // Sending absolute MHz as float would lose fine tuning precision at
    // VHF/UHF frequencies even though the per-row metadata texture is float32.
    m_wfFrameReferenceCenterMhz = m_centerMhz;
    m_wfFrameUpload.resize(uploadHeight * 4);
    for (int row = 0; row < uploadHeight; ++row) {
        const bool visibleRow = row < visibleHeight;
        const double centerMhz = visibleRow
            ? m_wfVisibleRowCenterMhz[row]
            : m_centerMhz;
        const double bandwidthMhz = visibleRow
            ? m_wfVisibleRowBwMhz[row]
            : m_bandwidthMhz;
        m_wfFrameUpload[row * 4] = static_cast<float>(
            centerMhz - m_wfFrameReferenceCenterMhz);
        m_wfFrameUpload[row * 4 + 1] = static_cast<float>(bandwidthMhz);
        m_wfFrameUpload[row * 4 + 2] = 0.0f;
        m_wfFrameUpload[row * 4 + 3] = 0.0f;
    }
}
#endif

void SpectrumWidget::rebuildWaterfallViewport()
{
    rebuildWaterfallViewportForFrame(m_centerMhz, m_bandwidthMhz);
}

void SpectrumWidget::rebuildDssViewportFromHistory()
{
    rebuildDssViewportFromHistoryForFrame(m_centerMhz, m_bandwidthMhz);
}

void SpectrumWidget::rebuildDssViewportFromHistoryForFrame(double centerMhz,
                                                           double bandwidthMhz)
{
    m_dss.rebuildVisibleFromHistory(m_wfHistoryOffsetRows,
                                    centerMhz, bandwidthMhz,
                                    dssHistoryFallbackDbm());
    resetDssUploadState();
}

void SpectrumWidget::rebuildWaterfallViewportForFrame(double centerMhz,
                                                      double bandwidthMhz)
{
    if (m_frequencyPreviewActive) {
        ++m_frequencyPreviewSuppressedViewportRebuilds;
        return;
    }
    if (m_waterfall.isNull()) {
        return;
    }

    m_wfHistoryOffsetRows = std::clamp(m_wfHistoryOffsetRows, 0, maxWaterfallHistoryOffsetRows());
    m_waterfall.fill(Qt::black);
    m_wfWriteRow = 0;
    resetVisibleWaterfallFrequencyFrames(centerMhz, bandwidthMhz);

    if (!m_waterfallHistory.isConfigured()) {
        update();
        return;
    }

    const int w = m_waterfall.width();
    const bool haveFrames = m_wfHistoryRowCenterMhz.size()
        == m_waterfallHistory.capacityRows();
    const std::array<QRgb, 256> colorLut = waterfallHistoryColorLut();
    for (int y = 0; y < m_waterfall.height(); ++y) {
        const int rowIndex = historyRowIndexForAge(m_wfHistoryOffsetRows + y);
        if (rowIndex < 0) {
            break;
        }
        const quint8* src = m_waterfallHistory.row(rowIndex);
        if (!src) {
            continue;
        }
        auto* dst = reinterpret_cast<QRgb*>(m_waterfall.scanLine(y));
        const double rowCenter = haveFrames ? m_wfHistoryRowCenterMhz[rowIndex] : m_centerMhz;
        const double rowBw = haveFrames ? m_wfHistoryRowBwMhz[rowIndex] : m_bandwidthMhz;
        remapHistoryRowInto(dst, src, w, colorLut,
                            rowCenter, rowBw, centerMhz, bandwidthMhz,
                            m_kiwiSdrWaterfallActive);
    }

#ifdef AETHER_GPU_SPECTRUM
    m_wfTexFullUpload = true;
#endif
    if (PerfTelemetry::instance().enabled())
        PerfTelemetry::instance().recordWaterfallRebuild();
    if (!m_wfLive) {
        rebuildDssViewportFromHistoryForFrame(centerMhz, bandwidthMhz);
    }
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
    rebuildDssViewportFromHistory();
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
        const float dssFallback = kiwiStream
            ? kKiwiSdrWaterfallMinDbm
            : m_refLevel - m_dynamicRange;
        if (m_wfLive || m_dss.historyRowCount() <= 0) {
            m_dss.reprojectFrequencyFrame(oldCenterMhz, oldBandwidthMhz,
                                          newCenterMhz, newBandwidthMhz,
                                          dssFallback);
        } else {
            rebuildDssViewportFromHistoryForFrame(newCenterMhz, newBandwidthMhz);
        }
        resetDssUploadState();
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

bool SpectrumWidget::frequencyPreviewAvailable() const
{
#ifdef AETHER_GPU_SPECTRUM
    // The 3D cached-image fallback cannot transform its horizontal sampling;
    // keep its existing exact CPU path. The preferred height-map mesh and the
    // normal 2D waterfall both have the preview uniforms.
    return waterfallGpuFrequencyPreviewRequested()
        && m_wfPipelineMode == WaterfallPipelineMode::RowFrequencyFrames
        && m_wfFrameTexReady
        && (m_spectrumRenderMode != SpectrumRenderMode::Mode3D || m_dssMeshReady);
#else
    return false;
#endif
}

bool SpectrumWidget::updateFrequencyPreview(double oldCenterMhz,
                                            double oldBandwidthMhz,
                                            double newCenterMhz,
                                            double newBandwidthMhz)
{
    if (!frequencyPreviewAvailable()
        || !std::isfinite(oldCenterMhz) || !std::isfinite(oldBandwidthMhz)
        || !std::isfinite(newCenterMhz) || !std::isfinite(newBandwidthMhz)
        || oldBandwidthMhz <= 0.0 || newBandwidthMhz <= 0.0) {
        return false;
    }

    if (!m_frequencyPreviewActive) {
        if (mhzNearlyEqual(oldCenterMhz, newCenterMhz)
            && mhzNearlyEqual(oldBandwidthMhz, newBandwidthMhz)) {
            return false;
        }
        m_frequencyPreviewBaseCenterMhz = oldCenterMhz;
        m_frequencyPreviewBaseBandwidthMhz = oldBandwidthMhz;
        // A clean texture represents the old frame. If it was already dirty,
        // the first preview render will rasterize at the new frame before the
        // dirty flag is frozen, so use that as the overlay transform base.
        bool overlayDirty = false;
#ifdef AETHER_GPU_SPECTRUM
        overlayDirty = m_overlayStaticDirty;
#endif
        m_frequencyPreviewOverlayBaseCenterMhz = overlayDirty
            ? newCenterMhz : oldCenterMhz;
        m_frequencyPreviewOverlayBaseBandwidthMhz = overlayDirty
            ? newBandwidthMhz : oldBandwidthMhz;
        m_frequencyPreviewActive = true;
    }

    m_frequencyPreviewTargetCenterMhz = newCenterMhz;
    m_frequencyPreviewTargetBandwidthMhz = newBandwidthMhz;
    ++m_frequencyPreviewUpdateCount;
    return true;
}

void SpectrumWidget::scheduleFrequencyPreviewFrame()
{
    // The drag-start QPainter overlay remains resident on the GPU. The preview
    // shader remaps its frequency-canvas pixels alongside the waterfall/3D
    // data, avoiding two full-widget rasterizations and texture uploads for
    // every input frame. commitFrequencyPreview() lands one exact CPU overlay.
#ifdef AETHER_GPU_SPECTRUM
    if (!m_ovPreviewPipeline || !m_ovPreviewSrb || !m_ovPreviewUbo) {
        markOverlayDirty("frequencyPreviewFallback", false);
    } else {
        // Unlike the full marker/chrome textures, this is only a narrow strip.
        // Repaint it at presentation cadence so its labels remain exact while
        // the frequency marker layer moves entirely on the GPU.
        m_frequencyScalePreviewNeedsUpload = true;
    }
#endif
    coalescedUpdate();
}

void SpectrumWidget::commitFrequencyPreview()
{
    if (!m_frequencyPreviewActive) {
        return;
    }

    const double baseCenterMhz = m_frequencyPreviewBaseCenterMhz;
    const double baseBandwidthMhz = m_frequencyPreviewBaseBandwidthMhz;
    const double targetCenterMhz = m_frequencyPreviewTargetCenterMhz;
    const double targetBandwidthMhz = m_frequencyPreviewTargetBandwidthMhz;

    // Each RGB row now retains its own frequency frame, so the waterfall shader
    // is already exact at the target center/bandwidth and needs no release-time
    // CPU rebuild. Only the compact 3D stacked-trace surface still uses the
    // uniform preview base and must be landed into the target frame.
    m_frequencyPreviewActive = false;
    QElapsedTimer timer;
    timer.start();
#ifdef AETHER_GPU_SPECTRUM
    const bool rowFrequencyFrames =
        m_wfPipelineMode == WaterfallPipelineMode::RowFrequencyFrames
        && m_wfFrameTexReady;
#else
    constexpr bool rowFrequencyFrames = false;
#endif
    if (!rowFrequencyFrames) {
        handleWaterfallFrequencyFrameChange(baseCenterMhz, baseBandwidthMhz,
                                            targetCenterMhz, targetBandwidthMhz);
    } else if (m_spectrumRenderMode == SpectrumRenderMode::Mode3D) {
        if (!m_wfLive && m_dss.historyRowCount() > 0) {
            rebuildDssViewportFromHistoryForFrame(targetCenterMhz,
                                                  targetBandwidthMhz);
        } else {
            m_dss.reprojectFrequencyFrame(baseCenterMhz, baseBandwidthMhz,
                                          targetCenterMhz, targetBandwidthMhz,
                                          dssHistoryFallbackDbm());
            resetDssUploadState();
        }
    }
    m_frequencyPreviewCommitLastNs = timer.nsecsElapsed();
    m_frequencyPreviewCommitMaxNs = std::max(
        m_frequencyPreviewCommitMaxNs, m_frequencyPreviewCommitLastNs);
    ++m_frequencyPreviewCommitCount;

    m_frequencyPreviewBaseCenterMhz = targetCenterMhz;
    m_frequencyPreviewBaseBandwidthMhz = targetBandwidthMhz;
    m_frequencyPreviewTargetCenterMhz = targetCenterMhz;
    m_frequencyPreviewTargetBandwidthMhz = targetBandwidthMhz;
    m_frequencyPreviewOverlayBaseCenterMhz = targetCenterMhz;
    m_frequencyPreviewOverlayBaseBandwidthMhz = targetBandwidthMhz;
#ifdef AETHER_GPU_SPECTRUM
    m_frequencyScalePreviewNeedsUpload = false;
#endif
    ++m_frequencyPreviewOverlayCommitRefreshCount;
    ++m_panStats.previewOverlayCommitRefreshes;
    markOverlayDirty("frequencyPreviewCommit", false);
    coalescedUpdate();
}

void SpectrumWidget::clearFrequencyPreview()
{
    m_frequencyPreviewActive = false;
    m_frequencyPreviewBaseCenterMhz = m_centerMhz;
    m_frequencyPreviewBaseBandwidthMhz = m_bandwidthMhz;
    m_frequencyPreviewTargetCenterMhz = m_centerMhz;
    m_frequencyPreviewTargetBandwidthMhz = m_bandwidthMhz;
    m_frequencyPreviewOverlayBaseCenterMhz = m_centerMhz;
    m_frequencyPreviewOverlayBaseBandwidthMhz = m_bandwidthMhz;
#ifdef AETHER_GPU_SPECTRUM
    m_frequencyScalePreviewNeedsUpload = false;
#endif
}

void SpectrumWidget::requestFrequencyRangeChange(double centerMhz,
                                                 double bandwidthMhz,
                                                 bool force)
{
    if (!std::isfinite(centerMhz) || !std::isfinite(bandwidthMhz)
        || centerMhz <= 0.0 || bandwidthMhz <= 0.0) {
        return;
    }

    const bool due = !m_frequencyRangeCommandClock.isValid()
        || m_frequencyRangeCommandClock.elapsed() >= kFrequencyRangeCommandMs;
    const std::optional<FrequencyRangeCommand> command =
        m_frequencyRangeCommandThrottle.request(
            FrequencyRangeCommand{centerMhz, bandwidthMhz}, due, force);
    if (command.has_value()) {
        if (m_frequencyRangeCommandTimer) {
            m_frequencyRangeCommandTimer->stop();
        }
        m_frequencyRangeCommandClock.restart();
        ++m_frequencyRangeCommandCount;
        emit frequencyRangeChangeRequested(command->centerMhz,
                                           command->bandwidthMhz);
        return;
    }

    if (m_frequencyRangeCommandTimer
        && !m_frequencyRangeCommandTimer->isActive()) {
        const int remainingMs = std::max(
            1, kFrequencyRangeCommandMs
                - static_cast<int>(m_frequencyRangeCommandClock.elapsed()));
        m_frequencyRangeCommandTimer->start(remainingMs);
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
    clearFrequencyPreview();
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
    if (m_waterfallHistory.isConfigured()) {
        m_waterfallHistory.discardRows();
    }
    std::fill(m_wfHistoryTimestamps.begin(), m_wfHistoryTimestamps.end(), 0);
    std::fill(m_wfHistoryRowCenterMhz.begin(), m_wfHistoryRowCenterMhz.end(), 0.0);
    std::fill(m_wfHistoryRowBwMhz.begin(), m_wfHistoryRowBwMhz.end(), 0.0);
    m_wfWriteRow = 0;
    resetVisibleWaterfallFrequencyFrames(m_centerMhz, m_bandwidthMhz);
    m_wfHistoryWriteRow = 0;
    m_wfHistoryRowCount = 0;
    m_wfHistoryOffsetRows = 0;
    m_wfLive = true;
    m_wfRowsSinceRateChange = 0;
    m_prevTileLevels.clear();
    m_kiwiSdrFftTrace.clear();
    m_kiwiSdrFftFallbackSeedMask.clear();
    m_kiwiSdrLastWaterfallBins.clear();
    m_kiwiSdrLastWaterfallCenterMhz = 0.0;
    m_kiwiSdrLastWaterfallBandwidthMhz = 0.0;
    m_kiwiSdrLastWaterfallFrameValid = false;
    m_kiwiSdrFftTraceFloorDbm = -1000.0f;
    m_kiwiSdrFftTraceFloorValid = false;
    m_dss.clear();
    resetDssUploadState();
#ifdef AETHER_GPU_SPECTRUM
    m_wfTexFullUpload = true;
#endif
}

void SpectrumWidget::resetKiwiSdrWaterfallDisplayRange()
{
    const KiwiSdrProtocol::WaterfallDisplayRange range =
        KiwiSdrProtocol::defaultWaterfallDisplayRange(0, 0, 0);
    m_kiwiSdrDisplayFloorDbm = range.minDbm;
    m_kiwiSdrDisplayCeilDbm = range.maxDbm;
    m_kiwiSdrDisplayRangeValid = false;
    m_kiwiSdrDisplayRangeAutoRange = false;
}

void SpectrumWidget::resetCurrentWaterfallRowsForSize(
    const QSize& waterfallSize,
    const QSize& historySize)
{
    if (!waterfallSize.isEmpty()) {
        m_waterfall = QImage(waterfallSize, QImage::Format_RGB32);
        m_waterfallStreamSizeHint = waterfallSize;
    } else {
        m_waterfall = QImage();
        m_waterfallStreamSizeHint = QSize();
    }

    QSize desiredHistorySize = m_waterfallWriteVisible ? historySize : QSize{};
    if (m_waterfallWriteVisible
        && desiredHistorySize.isEmpty() && !waterfallSize.isEmpty()) {
        desiredHistorySize =
            QSize(waterfallSize.width(), waterfallHistoryCapacityRows());
    }
    if (!desiredHistorySize.isEmpty()) {
        m_waterfallHistory.configure(desiredHistorySize.width(),
                                     desiredHistorySize.height());
        m_waterfallHistoryStreamSizeHint = desiredHistorySize;
        m_wfHistoryTimestamps = QVector<qint64>(desiredHistorySize.height(), 0);
        m_wfHistoryRowCenterMhz = QVector<double>(desiredHistorySize.height(), 0.0);
        m_wfHistoryRowBwMhz = QVector<double>(desiredHistorySize.height(), 0.0);
        const bool retainDssHistory =
            m_spectrumRenderMode == SpectrumRenderMode::Mode3D;
        m_dss.setHistoryCapacityRows(
            retainDssHistory ? desiredHistorySize.height() : 0);
    } else {
        m_waterfallHistory.reset();
        m_waterfallHistoryStreamSizeHint = QSize();
        m_wfHistoryTimestamps.clear();
        m_wfHistoryRowCenterMhz.clear();
        m_wfHistoryRowBwMhz.clear();
        m_dss.setHistoryCapacityRows(0);
    }

    clearCurrentWaterfallRows();
}

void SpectrumWidget::clearWaterfallRows()
{
    clearCurrentWaterfallRows();
    resetKiwiSdrWaterfallDisplayRange();
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

const SpectrumWidget::WaterfallStreamState*
SpectrumWidget::activeKiwiWaterfallStateConst() const
{
    if (!m_kiwiSdrWaterfallProfileId.isEmpty()) {
        auto it = m_kiwiProfileWaterfallStates.constFind(
            m_kiwiSdrWaterfallProfileId);
        return it == m_kiwiProfileWaterfallStates.constEnd()
            ? nullptr
            : &it.value();
    }
    return &m_kiwiWaterfallState;
}

void SpectrumWidget::saveCurrentWaterfallStreamState()
{
    WaterfallStreamState& state = m_kiwiSdrWaterfallActive
        ? activeKiwiWaterfallState()
        : m_nativeWaterfallState;

    if (!m_waterfall.isNull()) {
        m_waterfallStreamSizeHint = m_waterfall.size();
    }
    if (m_waterfallHistory.isConfigured()) {
        m_waterfallHistoryStreamSizeHint = m_waterfallHistory.size();
    }

    // Transfer ownership between the visible stream and saved stream state.
    // Moving the compact history avoids sharing its allocated chunks with the
    // inactive source while keeping source switches allocation-free.
    WaterfallStreamState updated;
    updated.waterfall = std::move(m_waterfall);
    updated.wfWriteRow = m_wfWriteRow;
    updated.visibleRowCenterMhz = std::move(m_wfVisibleRowCenterMhz);
    updated.visibleRowBwMhz = std::move(m_wfVisibleRowBwMhz);
    updated.waterfallHistory = std::move(m_waterfallHistory);
    updated.historyTimestamps = std::move(m_wfHistoryTimestamps);
    updated.historyWriteRow = m_wfHistoryWriteRow;
    updated.historyRowCount = m_wfHistoryRowCount;
    updated.historyOffsetRows = m_wfHistoryOffsetRows;
    updated.historyRowCenterMhz = std::move(m_wfHistoryRowCenterMhz);
    updated.historyRowBwMhz = std::move(m_wfHistoryRowBwMhz);
    updated.live = m_wfLive;
    updated.rowsSinceRateChange = m_wfRowsSinceRateChange;
    updated.prevTileLevels = std::move(m_prevTileLevels);
    updated.kiwiFftTrace = std::move(m_kiwiSdrFftTrace);
    updated.kiwiFftFallbackSeedMask = std::move(m_kiwiSdrFftFallbackSeedMask);
    updated.kiwiLastWaterfallBins = std::move(m_kiwiSdrLastWaterfallBins);
    updated.kiwiLastWaterfallCenterMhz = m_kiwiSdrLastWaterfallCenterMhz;
    updated.kiwiLastWaterfallBandwidthMhz = m_kiwiSdrLastWaterfallBandwidthMhz;
    updated.kiwiLastWaterfallFrameValid = m_kiwiSdrLastWaterfallFrameValid;
    updated.dss = std::move(m_dss);
    updated.kiwiDisplayFloorDbm = m_kiwiSdrDisplayFloorDbm;
    updated.kiwiDisplayCeilDbm = m_kiwiSdrDisplayCeilDbm;
    updated.kiwiDisplayRangeValid = m_kiwiSdrDisplayRangeValid;
    updated.kiwiDisplayRangeAutoRange = m_kiwiSdrDisplayRangeAutoRange;
    updated.kiwiFftTraceFloorDbm = m_kiwiSdrFftTraceFloorDbm;
    updated.kiwiFftTraceFloorValid = m_kiwiSdrFftTraceFloorValid;
    updated.valid = !updated.waterfall.isNull();
#ifdef AETHER_GPU_SPECTRUM
    updated.wfTexFullUpload = m_wfTexFullUpload;
    updated.wfLastUploadedRow = m_wfLastUploadedRow;
    updated.dssTexNeedsUpload = m_dssTexNeedsUpload;
    updated.dssLastUploadedGen = m_dssLastUploadedGen;
    updated.dssMeshHeadUploaded = m_dssMeshHeadUploaded;
    updated.dssMeshRowGenUploaded = m_dssMeshRowGenUploaded;
#endif

    state = std::move(updated);
}

void SpectrumWidget::discardRetainedHistory(WaterfallStreamState& state)
{
    state.waterfallHistory.reset();
    state.historyTimestamps.clear();
    state.historyWriteRow = 0;
    state.historyRowCount = 0;
    state.historyOffsetRows = 0;
    state.historyRowCenterMhz.clear();
    state.historyRowBwMhz.clear();
    state.rowsSinceRateChange = 0;
    state.live = true;
    state.dss.setHistoryCapacityRows(0);
}

void SpectrumWidget::restoreCurrentWaterfallStreamState()
{
    WaterfallStreamState& state = m_kiwiSdrWaterfallActive
        ? activeKiwiWaterfallState()
        : m_nativeWaterfallState;
    const QSize currentSize = !m_waterfall.isNull()
        ? m_waterfall.size()
        : m_waterfallStreamSizeHint;
    const QSize currentHistorySize = m_waterfallHistory.isConfigured()
        ? m_waterfallHistory.size()
        : m_waterfallHistoryStreamSizeHint;

    const bool stateHasWrongWaterfall =
        !currentSize.isEmpty()
        && (state.waterfall.isNull() || state.waterfall.size() != currentSize);
    const bool stateHasWrongHistory =
        !currentHistorySize.isEmpty()
        && state.waterfallHistory.isConfigured()
        && state.waterfallHistory.size() != currentHistorySize;
    if (!state.valid || stateHasWrongWaterfall || stateHasWrongHistory) {
        const bool restoreKiwiDisplayRange =
            m_kiwiSdrWaterfallActive && state.kiwiDisplayRangeValid;
        const float kiwiDisplayFloorDbm = state.kiwiDisplayFloorDbm;
        const float kiwiDisplayCeilDbm = state.kiwiDisplayCeilDbm;
        const bool kiwiDisplayRangeAutoRange =
            state.kiwiDisplayRangeAutoRange;
        state = WaterfallStreamState{};
        resetCurrentWaterfallRowsForSize(currentSize, currentHistorySize);
        if (m_kiwiSdrWaterfallActive) {
            if (restoreKiwiDisplayRange) {
                m_kiwiSdrDisplayFloorDbm = kiwiDisplayFloorDbm;
                m_kiwiSdrDisplayCeilDbm = kiwiDisplayCeilDbm;
                m_kiwiSdrDisplayRangeValid = true;
                m_kiwiSdrDisplayRangeAutoRange = kiwiDisplayRangeAutoRange;
            } else {
                resetKiwiSdrWaterfallDisplayRange();
            }
        }
        return;
    }

    WaterfallStreamState restored = std::move(state);
    state = WaterfallStreamState{};

    m_waterfall = std::move(restored.waterfall);
    if (!m_waterfall.isNull()) {
        m_waterfallStreamSizeHint = m_waterfall.size();
    }
    m_wfWriteRow = restored.wfWriteRow;
    m_wfVisibleRowCenterMhz = std::move(restored.visibleRowCenterMhz);
    m_wfVisibleRowBwMhz = std::move(restored.visibleRowBwMhz);
    if (m_wfVisibleRowCenterMhz.size() != m_waterfall.height()
        || m_wfVisibleRowBwMhz.size() != m_waterfall.height()) {
        resetVisibleWaterfallFrequencyFrames(m_centerMhz, m_bandwidthMhz);
    }
    m_waterfallHistory = std::move(restored.waterfallHistory);
    if (m_waterfallHistory.isConfigured()) {
        m_waterfallHistoryStreamSizeHint = m_waterfallHistory.size();
    }
    m_wfHistoryTimestamps = std::move(restored.historyTimestamps);
    m_wfHistoryWriteRow = restored.historyWriteRow;
    m_wfHistoryRowCount = restored.historyRowCount;
    m_wfHistoryOffsetRows = restored.historyOffsetRows;
    m_wfHistoryRowCenterMhz = std::move(restored.historyRowCenterMhz);
    m_wfHistoryRowBwMhz = std::move(restored.historyRowBwMhz);
    m_wfLive = restored.live;
    m_wfRowsSinceRateChange = restored.rowsSinceRateChange;
    m_prevTileLevels = std::move(restored.prevTileLevels);
    m_kiwiSdrFftTrace = std::move(restored.kiwiFftTrace);
    m_kiwiSdrFftFallbackSeedMask = std::move(restored.kiwiFftFallbackSeedMask);
    m_kiwiSdrLastWaterfallBins = std::move(restored.kiwiLastWaterfallBins);
    m_kiwiSdrLastWaterfallCenterMhz = restored.kiwiLastWaterfallCenterMhz;
    m_kiwiSdrLastWaterfallBandwidthMhz = restored.kiwiLastWaterfallBandwidthMhz;
    m_kiwiSdrLastWaterfallFrameValid = restored.kiwiLastWaterfallFrameValid;
    m_dss = std::move(restored.dss);
    m_kiwiSdrDisplayFloorDbm = restored.kiwiDisplayFloorDbm;
    m_kiwiSdrDisplayCeilDbm = restored.kiwiDisplayCeilDbm;
    m_kiwiSdrDisplayRangeValid = restored.kiwiDisplayRangeValid;
    m_kiwiSdrDisplayRangeAutoRange = restored.kiwiDisplayRangeAutoRange;
    m_kiwiSdrFftTraceFloorDbm = restored.kiwiFftTraceFloorDbm;
    m_kiwiSdrFftTraceFloorValid = restored.kiwiFftTraceFloorValid;
#ifdef AETHER_GPU_SPECTRUM
    m_wfTexFullUpload = restored.wfTexFullUpload;
    m_wfLastUploadedRow = restored.wfLastUploadedRow;
    m_wfFrameTexDirty = true;
    m_dssTexNeedsUpload = restored.dssTexNeedsUpload;
    m_dssLastUploadedGen = restored.dssLastUploadedGen;
    m_dssMeshHeadUploaded = restored.dssMeshHeadUploaded;
    m_dssMeshRowGenUploaded = restored.dssMeshRowGenUploaded;
#endif
    if (m_waterfallWriteVisible) {
        ensureWaterfallHistory();
    }
}

bool SpectrumWidget::beginWaterfallStreamWrite(bool kiwiStream)
{
    const bool visibleStream = m_kiwiSdrWaterfallActive == kiwiStream;
    m_waterfallWriteVisible = visibleStream;
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
    m_waterfallWriteVisible = true;
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

    if (!visible) {
        removeOverlayMessage(QStringLiteral("kiwi.connection"));
        return;
    }

    PanadapterOverlayMessage message;
    message.id = QStringLiteral("kiwi.connection");
    message.title = trimmedTitle.isEmpty()
        ? QStringLiteral("Not connected to KiwiSDR")
        : trimmedTitle;
    message.detail = trimmedDetail.isEmpty()
        ? QStringLiteral("Disconnected")
        : trimmedDetail;
    message.timeoutMs = 0;
    // Owner-managed status: syncKiwiSdrPanadapterUiState re-asserts this card
    // on every state/slice/waterfall event, so a user dismissal either lies
    // (the card resurrects seconds later, even mid-fade) or — in a quiet
    // Waiting state that never re-syncs — permanently hides the only
    // disconnected-pan indicator while the pan looks healthy. Not
    // user-dismissible; setKiwiSdrConnectionOverlay(false) is the sole owner
    // of its lifecycle. (#3999 review)
    message.dismissible = false;
    upsertOverlayMessage(std::move(message));
}

void SpectrumWidget::upsertOverlayMessage(PanadapterOverlayMessage message)
{
    if (!m_panadapterMessageOverlay) {
        return;
    }
    positionPanadapterMessageOverlay();
    m_panadapterMessageOverlay->upsertMessage(std::move(message));
    raisePanadapterMessageOverlay();
}

bool SpectrumWidget::removeOverlayMessage(const QString& id)
{
    if (!m_panadapterMessageOverlay) {
        return false;
    }
    return m_panadapterMessageOverlay->removeMessage(id);
}

void SpectrumWidget::clearOverlayMessages()
{
    if (m_panadapterMessageOverlay) {
        m_panadapterMessageOverlay->clearMessages();
    }
}

bool SpectrumWidget::automationUpsertOverlayMessage(const QString& id,
                                                    const QString& title,
                                                    const QString& detail,
                                                    int timeoutMs,
                                                    const QString& toneName)
{
    PanadapterOverlayMessage message;
    message.id = id;
    message.title = title;
    message.detail = detail;
    message.timeoutMs = timeoutMs;
    message.dismissible = true;
    message.tone = toneName.compare(QStringLiteral("warning"), Qt::CaseInsensitive) == 0
        ? PanadapterOverlayMessageTone::Warning
        : PanadapterOverlayMessageTone::Info;
    upsertOverlayMessage(std::move(message));
    return true;
}

bool SpectrumWidget::automationRemoveOverlayMessage(const QString& id)
{
    return removeOverlayMessage(id);
}

void SpectrumWidget::automationClearOverlayMessages()
{
    clearOverlayMessages();
}

QVariantList SpectrumWidget::overlayMessageSnapshot() const
{
    if (!m_panadapterMessageOverlay) {
        return {};
    }
    return m_panadapterMessageOverlay->messageSnapshot();
}

void SpectrumWidget::showInterlockNotification(const QString& message,
                                               const QString& key,
                                               int durationMs)
{
    const QString text = message.trimmed();
    if (text.isEmpty()) {
        return;
    }

    PanadapterOverlayMessage overlay;
    // Fixed id → latest-wins: the radio's authoritative denial supersedes an
    // earlier local preflight message in place, instead of the two stacking
    // side-by-side for the full 5s (the pre-PR single QLabel showed latest
    // only, and the operator could otherwise act on the stale card). (#3999 review)
    overlay.id = QStringLiteral("interlock.active");
    // Classify by the producer's stable, translation-invariant key, not by
    // sniffing the localized message text: the old startsWith("Transmit is
    // disabled") match broke in every non-English locale, and the radio's most
    // authoritative reasons ("radio:...") never matched the prefix at all and
    // degraded to the low-salience "Notice" heading. (#3999 review)
    const bool txBlock = key.startsWith(QStringLiteral("radio:"))
        || key.startsWith(QStringLiteral("pan-tx-inhibit:"))
        || key.startsWith(QStringLiteral("local-ptt:"))
        || key.startsWith(QStringLiteral("tx-filter:"));
    overlay.title = txBlock ? tr("Transmit disabled") : tr("Notice");
    overlay.detail = text;
    overlay.timeoutMs = qMax(1, durationMs);
    overlay.dismissible = true;
    overlay.tone = PanadapterOverlayMessageTone::Warning;
    upsertOverlayMessage(std::move(overlay));
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

void SpectrumWidget::resetGpuResources()
{
    if (m_shutdownPrepared) {
        clearFrequencyPreview();
    } else {
        commitFrequencyPreview();
    }
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
    if (m_waterfallHistory.isConfigured() && m_wfHistoryRowCount > 0) {
        rebuildWaterfallViewportForFrame(newCenterMhz, newBandwidthMhz);
    } else {
        reprojectWaterfallImage(m_waterfall, oldCenterMhz, oldBandwidthMhz,
                                newCenterMhz, newBandwidthMhz);
        resetVisibleWaterfallFrequencyFrames(newCenterMhz, newBandwidthMhz);
    }
    m_prevTileLevels.clear();
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
    const bool hadSpectrum = !m_bins.isEmpty() || !m_smoothed.isEmpty()
        || !m_kiwiSdrFftTrace.isEmpty();
    if (oldBandwidthMhz <= 0.0 || newBandwidthMhz <= 0.0) {
        m_resetFftSmoothingOnNextFrame = m_resetFftSmoothingOnNextFrame || hadSpectrum;
        m_fftFallbackSeedMask.clear();
        m_kiwiSdrFftFallbackSeedMask.clear();
        return hadSpectrum;
    }

    const double oldStartMhz = oldCenterMhz - oldBandwidthMhz / 2.0;
    const double oldEndMhz = oldCenterMhz + oldBandwidthMhz / 2.0;
    const double newStartMhz = newCenterMhz - newBandwidthMhz / 2.0;
    const double newEndMhz = newCenterMhz + newBandwidthMhz / 2.0;
    const double overlapStartMhz = std::max(oldStartMhz, newStartMhz);
    const double overlapEndMhz = std::min(oldEndMhz, newEndMhz);
    if (overlapEndMhz <= overlapStartMhz) {
        // Large gesture jumps can have no overlap with the previous spectrum
        // frame. Keep the last trace visible until the next real FFT row instead
        // of flashing the line off for one frame.
        m_resetFftSmoothingOnNextFrame = m_resetFftSmoothingOnNextFrame || hadSpectrum;
        m_fftFallbackSeedMask.clear();
        m_kiwiSdrFftFallbackSeedMask.clear();
        return hadSpectrum;
    }

    auto reprojectBins = [&](QVector<float>& bins, float fallback,
                             QVector<quint8>* fallbackSeedMask = nullptr,
                             bool extendFallbackEdges = false) {
        const int binCount = bins.size();
        if (binCount <= 0) {
            if (fallbackSeedMask) {
                fallbackSeedMask->clear();
            }
            return;
        }

        QVector<quint8> oldFallbackSeedMask;
        if (fallbackSeedMask) {
            oldFallbackSeedMask = std::move(*fallbackSeedMask);
        }
        const bool hadFallbackSeedMask = oldFallbackSeedMask.size() == binCount;

        const QVector<float> oldBins = std::move(bins);
        QVector<float> reprojected(binCount, fallback);
        QVector<quint8> reprojectedFallbackSeedMask;
        if (fallbackSeedMask) {
            reprojectedFallbackSeedMask = QVector<quint8>(binCount, quint8(1));
        }

        int firstProjected = -1;
        int lastProjected = -1;

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
                if (fallbackSeedMask) {
                    reprojectedFallbackSeedMask[dst] =
                        hadFallbackSeedMask ? oldFallbackSeedMask[src] : quint8(0);
                }
                if (firstProjected < 0) {
                    firstProjected = dst;
                }
                lastProjected = dst;
                continue;
            }

            const float t = static_cast<float>(srcPos - srcLeft);
            reprojected[dst] = oldBins[srcLeft] * (1.0f - t) + oldBins[srcRight] * t;
            if (fallbackSeedMask) {
                reprojectedFallbackSeedMask[dst] =
                    hadFallbackSeedMask
                    ? quint8(oldFallbackSeedMask[srcLeft] || oldFallbackSeedMask[srcRight])
                    : quint8(0);
            }
            if (firstProjected < 0) {
                firstProjected = dst;
            }
            lastProjected = dst;
        }

        if (extendFallbackEdges && firstProjected >= 0) {
            for (int dst = 0; dst < firstProjected; ++dst) {
                reprojected[dst] = reprojected[firstProjected];
            }
            for (int dst = lastProjected + 1; dst < binCount; ++dst) {
                reprojected[dst] = reprojected[lastProjected];
            }
        }

        bins = std::move(reprojected);
        if (fallbackSeedMask) {
            *fallbackSeedMask = std::move(reprojectedFallbackSeedMask);
        }
    };

    reprojectBins(m_bins, m_refLevel - m_dynamicRange);
    reprojectBins(m_smoothed, m_refLevel - m_dynamicRange,
                  &m_fftFallbackSeedMask, true);
    reprojectBins(m_kiwiSdrFftTrace, kKiwiSdrWaterfallMinDbm,
                  &m_kiwiSdrFftFallbackSeedMask, true);
    return !m_bins.isEmpty() || !m_smoothed.isEmpty()
        || !m_kiwiSdrFftTrace.isEmpty();
}

void SpectrumWidget::setFrequencyRange(double centerMhz, double bandwidthMhz)
{
    setFrequencyRangeInternal(centerMhz, bandwidthMhz, true);
}

void SpectrumWidget::setFrequencyRangeImmediate(double centerMhz, double bandwidthMhz)
{
    setFrequencyRangeInternal(centerMhz, bandwidthMhz, false);
}

void SpectrumWidget::setFrequencyRangeInternal(double centerMhz, double bandwidthMhz,
                                               bool animateSmallNudges)
{
    if (centerMhz == m_centerMhz && bandwidthMhz == m_bandwidthMhz)
        return;

    // While the user is actively dragging the pan or a VFO/slice, the local
    // drag path owns the visual center. Radio echoes for intermediate drag
    // positions are stale by the time they arrive and would force the flag and
    // waterfall back through old frames.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool vfoDragPanEchoHold =
        m_vfoDragPanEchoHoldUntilMs > 0 && nowMs < m_vfoDragPanEchoHoldUntilMs;
    if ((m_draggingPan || m_draggingVfo || vfoDragPanEchoHold)
        && mhzNearlyEqual(bandwidthMhz, m_bandwidthMhz)) {
        return;
    }

    // While a local zoom/range gesture is settling, the widget owns the visual
    // center and bandwidth. Flex can echo older center-only statuses after a
    // combined center+bandwidth command; accepting those stale centers retargets
    // the local view and can churn remote Kiwi W/F zoom/start requests.
    if (m_frequencyRangeSettlePending
        && m_frequencyRangePendingValid
        && !mhzNearlyEqual(centerMhz, m_centerMhz)
        && !mhzNearlyEqual(centerMhz, m_frequencyRangePendingCenterMhz)) {
        return;
    }

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

    if (largeShift || !animateSmallNudges) {
        // Large jumps and explicit immediate updates snap without pan-follow
        // animation, but still share the same stale-echo and waterfall guards.
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
            m_resetFftSmoothingOnNextFrame = true;
            if (!bwChanged) {
                m_wfWriteRow = 0;
            }
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

void SpectrumWidget::cancelPendingDbmRangeChange()
{
    m_pendingDbmRangeEcho = false;
    m_pendingDbmRangeEchoFromAutoFloor = false;
    m_pendingDbmRangeEchoStartMs = 0;
    clearDbmReleaseRebase();
    m_resetFftSmoothingOnNextFrame = true;
}

void SpectrumWidget::applyDbmRangeImmediate(float minDbm, float maxDbm)
{
    if (!clampDbmRange(minDbm, maxDbm)) {
        return;
    }
    const float clampedMinDbm = minDbm;
    float ref = maxDbm;
    float dyn = maxDbm - clampedMinDbm;
    if (ref == m_refLevel && dyn == m_dynamicRange) {
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
        m_wfLastGoodLevels.clear();                       // forget any TX-era last-good row
        // Drop the FFT trace's client-side EMA so the first clean RX frame is
        // taken raw instead of weighted against TX-contaminated history. During
        // the UNKEY_REQUESTED window the radio keeps streaming TX-contaminated
        // FFT frames (#1927); those poison m_smoothed, and at SMOOTH_ALPHA=0.35
        // the EMA otherwise takes ~5-7 frames (~200-300 ms at 25 fps) to wash
        // out, leaving the displayed floor visibly elevated after key-up (#3804).
        // This does not address the radio firmware's own averaging buffer, which
        // is the dominant slow-decay source and is not client-fixable.
        m_resetFftSmoothingOnNextFrame = true;
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
                                     bool xitOn, int xitFreq,
                                     bool diversity, bool diversityParent,
                                     bool diversityChild, int diversityIndex)
{
    int idx = overlayIndex(sliceId);
    if (idx < 0) {
        SliceOverlay o;
        o.sliceId = sliceId; o.freqMhz = freq;
        o.filterLowHz = fLow; o.filterHighHz = fHigh;
        o.isTxSlice = tx; o.isActive = active;
        o.diversity = diversity;
        o.diversityParent = diversityParent;
        o.diversityChild = diversityChild;
        o.diversityIndex = diversityIndex;
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
            o.xitOn == xitOn && o.xitFreq == xitFreq &&
            o.diversity == diversity && o.diversityParent == diversityParent &&
            o.diversityChild == diversityChild && o.diversityIndex == diversityIndex)
            return;
        o.freqMhz = freq; o.filterLowHz = fLow; o.filterHighHz = fHigh;
        o.isTxSlice = tx; o.isActive = active;
        o.diversity = diversity;
        o.diversityParent = diversityParent;
        o.diversityChild = diversityChild;
        o.diversityIndex = diversityIndex;
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

void SpectrumWidget::setSliceOverlayAdaptive(int sliceId, bool enabled)
{
    int idx = overlayIndex(sliceId);
    if (idx < 0) return;
    auto& o = m_sliceOverlays[idx];
    if (o.adaptiveEnabled == enabled) return;
    o.adaptiveEnabled = enabled;
    markOverlayDirty();
}

void SpectrumWidget::setSliceOverlayAdaptiveActive(int sliceId, bool active)
{
    int idx = overlayIndex(sliceId);
    if (idx < 0) return;
    auto& o = m_sliceOverlays[idx];
    if (o.adaptiveActive == active) return;
    o.adaptiveActive = active;
    markOverlayDirty();
}

void SpectrumWidget::setCenterLockSliceId(int sliceId)
{
    if (m_centerLockSliceId == sliceId) {
        return;
    }
    m_centerLockSliceId = sliceId;
    QString sliceName = QString::number(sliceId);
    const int overlay = overlayIndex(sliceId);
    if (overlay >= 0) {
        sliceName = SliceLabel::unicodeForm(
            m_sliceOverlays.at(overlay).sliceId,
            m_sliceOverlays.at(overlay).perClientLetter);
    }
    setAccessibleDescription(
        sliceId >= 0
            ? tr("Center Lock enabled for Slice %1").arg(sliceName)
            : tr("Center Lock is off"));
    QAccessibleValueChangeEvent event(this, sliceId);
    QAccessible::updateAccessibility(&event);
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
    m_panStats.updateSpectrumCalls++;
    struct IngestCost {
        quint64& acc;
        QElapsedTimer t;
        explicit IngestCost(quint64& a) : acc(a) { t.start(); }
        ~IngestCost() { acc += static_cast<quint64>(t.nsecsElapsed() / 1000); }
    } panStatsIngestCost(m_panStats.updateSpectrumUs);
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
    // the radio can continue sending FFT frames encoded with the old range for
    // several hundred milliseconds. Compare both interpretations with the last
    // corrected frame so arrows and drags never feed the transition glitch into
    // the trace, noise-floor tracker, waterfall fallback, or 3D history.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_dbmReleaseRebaseUntilMs > 0 && nowMs > m_dbmReleaseRebaseUntilMs) {
        clearDbmReleaseRebase();
    }
    if (m_dbmReleaseRebaseUntilMs > 0) {
        const QVector<float>& sourceBins = *spectrumBins;
        const float oldRange =
            m_dbmReleasePreviewOldMaxDbm - m_dbmReleasePreviewOldMinDbm;
        const float newRange =
            m_dbmReleasePreviewNewMaxDbm - m_dbmReleasePreviewNewMinDbm;
        if (oldRange <= 0.0f || newRange <= 0.0f) {
            clearDbmReleaseRebase();
        } else if (!m_bins.isEmpty() && m_bins.size() == sourceBins.size()) {
            DbmRangeTransition::Evaluation evaluation =
                DbmRangeTransition::evaluate(
                    sourceBins, m_bins,
                    m_dbmReleasePreviewOldMinDbm,
                    m_dbmReleasePreviewOldMaxDbm,
                    m_dbmReleasePreviewNewMinDbm,
                    m_dbmReleasePreviewNewMaxDbm,
                    kDbmReleaseRebaseMinImprovementDb);
            if (evaluation.useRebasedBins) {
                adjustedBins = std::move(evaluation.rebasedBins);
                spectrumBins = &adjustedBins;
            } else if (evaluation.newEncodingObserved) {
                clearDbmReleaseRebase();
            }
        }
    }

    if (m_resetFftSmoothingOnNextFrame) {
        m_smoothed = *spectrumBins;
        m_resetFftSmoothingOnNextFrame = false;
        m_fftFallbackSeedMask.clear();
    } else if (m_smoothed.size() != spectrumBins->size()) {
        m_smoothed = *spectrumBins;
        m_fftFallbackSeedMask.clear();
    } else {
        const bool seedFallbackBins =
            m_fftFallbackSeedMask.size() == spectrumBins->size();
        for (int i = 0; i < spectrumBins->size(); ++i) {
            if (seedFallbackBins && m_fftFallbackSeedMask[i]) {
                m_smoothed[i] = (*spectrumBins)[i];
            } else {
                m_smoothed[i] = SMOOTH_ALPHA * (*spectrumBins)[i]
                    + (1.0f - SMOOTH_ALPHA) * m_smoothed[i];
            }
        }
        m_fftFallbackSeedMask.clear();
    }
    m_bins = *spectrumBins;

    if (m_kiwiSdrWaterfallActive) {
        // The cached Flex FFT trace (m_bins, updated above) stays current for an
        // immediate source switch. Warm the small hidden-Flex DSS live ring too,
        // so switching back to Flex shows current 3D history instead of an old
        // surface that refills over ~96 frames (#4081). The deep retained DSS
        // history is already released for hidden sources (capacity 0), so
        // pushDssRowForWaterfallStream() keeps only the small live surface warm
        // and retains no scrollback (#4083). Then stop: hidden Flex frames must
        // not drive fallback waterfall rows or schedule QRhi redraws for the
        // visible Kiwi view.
        pushDssRowForWaterfallStream(false, m_bins);
        return;
    }

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

    if (!m_kiwiSdrWaterfallActive) {
        updateAutoSquelchFromBins(*spectrumBins);
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

    coalescedUpdate();
}

void SpectrumWidget::updateWaterfallRow(const QVector<float>& binsIntensity,
                                        double lowFreqMhz, double highFreqMhz,
                                        quint32 timecode)
{
    PerfUpdateScope perfScope(PerfUpdateScope::Kind::Waterfall);
    // Native waterfall tiles carry intensity values (int16/128.0f, ~96-120 on HF).
    if (binsIntensity.isEmpty()) return;
    ++m_panStats.nativeWaterfallCalls;
    if (m_kiwiSdrWaterfallActive) {
        ++m_panStats.nativeWaterfallHiddenCalls;
    }
    struct WaterfallIngestCost {
        quint64& acc;
        QElapsedTimer timer;
        explicit WaterfallIngestCost(quint64& value) : acc(value) { timer.start(); }
        ~WaterfallIngestCost() {
            acc += static_cast<quint64>(timer.nsecsElapsed() / 1000);
        }
    } ingestCost(m_panStats.nativeWaterfallUs);
    if (m_kiwiSdrWaterfallActive) {
        // The cached Flex viewport remains available for an immediate source
        // switch. Do not spend a full state swap, color conversion, viewport
        // write, and DSS update on data that cannot be displayed; the first
        // fresh Flex tile replaces the snapshot within one radio frame.
        return;
    }
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

    QVector<quint8> levels(destWidth, 0);
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
                levels[x] = encodeWaterfallLevel(
                    intensityToWaterfallLevel(i0 + frac * (i1 - i0)));
            }
        }
    }

    // NB Waterfall Blanker (#277) — suppress impulse rows.
    // Skip entirely during TX: with show-tx-in-waterfall enabled, TX-era tiles
    // flow through and would otherwise poison the rolling baseline.  Post-TX,
    // real RX rows would then read as huge "impulses" against the suppressed
    // TX-era baseline, causing the blanker to substitute m_wfLastGoodLevels (a
    // TX-era row) for ~10–18 s while baseline slowly re-converges —
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
            if (m_wfLastGoodLevels.size() == destWidth) {
                levels = m_wfLastGoodLevels;
            } else {
                // No previous good row yet — fill with noise floor color
                const quint8 floorLevel = encodeWaterfallLevel(
                    intensityToWaterfallLevel(baseline));
                std::fill(levels.begin(), levels.end(), floorLevel);
            }
            m_wfBlankerRing[m_wfBlankerRingIdx] = std::min(rowMean, baseline * 1.05f);
        } else {
            m_wfLastGoodLevels = levels;
            m_wfBlankerRing[m_wfBlankerRingIdx] = rowMean;
        }
        m_wfBlankerRingIdx = (m_wfBlankerRingIdx + 1) % WF_BLANKER_N;
        if (m_wfBlankerRingCount < WF_BLANKER_N)
            ++m_wfBlankerRingCount;
    }

    // Write rows into history + visible viewport.
    const bool canInterp = (m_prevTileLevels.size() == destWidth && rowsToPush > 1);
    const std::array<QRgb, 256> colorLut = waterfallHistoryColorLut();
    for (int r = 0; r < rowsToPush; ++r) {
        QVector<quint8> interpolatedLevels(destWidth, 0);
        QVector<QRgb> interpolatedRow(destWidth, qRgb(0, 0, 0));
        if (canInterp) {
            // t=0 at row 0 (current), t=1 at last row (previous)
            const float t = static_cast<float>(r) / rowsToPush;
            for (int x = 0; x < destWidth; ++x) {
                const int current = levels[x];
                const int previous = m_prevTileLevels[x];
                interpolatedLevels[x] = static_cast<quint8>(std::clamp(
                    current + static_cast<int>(
                        std::lround(t * (previous - current))),
                    0, 255));
            }
        } else {
            interpolatedLevels = levels;
        }
        for (int x = 0; x < destWidth; ++x) {
            interpolatedRow[x] = colorLut[interpolatedLevels[x]];
        }

        appendHistoryRow(interpolatedLevels.constData(), nowMs);
        appendLatestDssWaterfallRow();
        if (m_wfLive) {
            appendVisibleRow(interpolatedRow.constData());
        } else {
            rebuildWaterfallViewport();
        }
    }
    m_prevTileLevels = levels;
    recordWaterfallFrame(rowsToPush);
    if (PerfTelemetry::instance().enabled())
        PerfTelemetry::instance().recordWaterfallNativeRows(rowsToPush);

    if (visibleStream) {
        coalescedUpdate();
    }
}

void SpectrumWidget::setKiwiSdrWaterfallActive(bool active)
{
    if (m_kiwiSdrWaterfallActive == active) {
        return;
    }

    commitFrequencyPreview();
    setNoiseFloorPositionForSource(m_kiwiSdrWaterfallActive,
                                   m_noiseFloorPosition,
                                   false);
    setDssFloorDepthForSource(m_kiwiSdrWaterfallActive,
                              DbmRangeTransition::floorDepthFromOffsetDb(
                                  m_dssFloorOffsetDb),
                              false);
    saveCurrentWaterfallStreamState();
    // Retained intensity/DSS scrollback follows the visible source. Keep the
    // small live viewport/DSS rings in every cached state for an immediate
    // toggle, but release all deep histories before choosing the new owner.
    discardRetainedHistory(m_nativeWaterfallState);
    discardRetainedHistory(m_kiwiWaterfallState);
    for (auto it = m_kiwiProfileWaterfallStates.begin();
         it != m_kiwiProfileWaterfallStates.end(); ++it) {
        discardRetainedHistory(it.value());
    }
    m_kiwiSdrWaterfallActive = active;
    m_lastAutoSquelchLevel = -1;
    if (!active) {
        m_sqlNoiseFloorDbm = -999.0f;
    }
    restoreCurrentWaterfallStreamState();
    resetDssUploadState();
    restoreNoiseFloorPositionForCurrentSource(true);
    restoreDssFloorDepthForCurrentSource(true);
    // The newly-active stream is no longer reprojected on every pan step
    // (handleWaterfallFrequencyFrameChange now touches only the active stream),
    // so its restored viewport can lag the current frequency frame. Remap it now
    // that it is visible -- exact, since each history row keeps its own capture
    // frame (#3578). This is the lazy counterpart to the per-pan reproject the
    // inactive stream used to receive.
    rebuildWaterfallViewportForFrame(m_centerMhz, m_bandwidthMhz);
    m_pendingDbmRangeEcho = false;
    m_pendingDbmRangeEchoFromAutoFloor = false;
    m_pendingDbmRangeEchoStartMs = 0;
    clearDbmReleaseRebase();
    resetNoiseFloorBaseline();
    if (!active) {
        clearKiwiSdrSquelchLine();
        m_resetFftSmoothingOnNextFrame = true;
    }
    reacquireNoiseFloorLockFromVisibleSource();
    updateFpsMeterLabels();
#ifdef AETHER_GPU_SPECTRUM
    m_wfTexFullUpload = true;
#endif
    coalescedUpdate();
}

void SpectrumWidget::setKiwiSdrDisplaySourceControlVisible(bool visible)
{
    if (!m_kiwiSdrDisplaySourceBtn
        || m_kiwiSdrDisplaySourceBtn->isHidden() != visible) {
        return;
    }

    m_kiwiSdrDisplaySourceBtn->setVisible(visible);
    if (visible) {
        m_kiwiSdrDisplaySourceBtn->raise();
    }
}

void SpectrumWidget::setKiwiSdrDisplaySourceKiwi(bool kiwi)
{
    m_kiwiSdrDisplaySourceKiwi = kiwi;
    if (m_kiwiSdrDisplaySourceBtn) {
        const QSignalBlocker blocker(m_kiwiSdrDisplaySourceBtn);
        m_kiwiSdrDisplaySourceBtn->setChecked(kiwi);
        m_kiwiSdrDisplaySourceBtn->setText(
            kiwi ? QStringLiteral("Kiwi") : QStringLiteral("Flex"));
        m_kiwiSdrDisplaySourceBtn->setToolTip(
            kiwi
                ? tr("Showing KiwiSDR spectrum/waterfall. Audio and meters are unchanged.")
                : tr("Showing Flex spectrum/waterfall. Audio and meters are unchanged."));
    }
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
        commitFrequencyPreview();
        saveCurrentWaterfallStreamState();
        discardRetainedHistory(m_nativeWaterfallState);
        discardRetainedHistory(m_kiwiWaterfallState);
        for (auto it = m_kiwiProfileWaterfallStates.begin();
             it != m_kiwiProfileWaterfallStates.end(); ++it) {
            discardRetainedHistory(it.value());
        }
    }
    m_kiwiSdrWaterfallProfileId = normalized;
    if (m_kiwiSdrWaterfallActive) {
        restoreCurrentWaterfallStreamState();
        resetDssUploadState();
#ifdef AETHER_GPU_SPECTRUM
        m_wfTexFullUpload = true;
#endif
        reacquireNoiseFloorLockFromVisibleSource();
        coalescedUpdate();
    }
}

void SpectrumWidget::clearKiwiSdrWaterfallRows()
{
    m_kiwiWaterfallState = WaterfallStreamState{};
    m_kiwiProfileWaterfallStates.clear();
    resetKiwiSdrWaterfallDisplayRange();
    m_kiwiSdrFftTrace.clear();
    m_kiwiSdrFftFallbackSeedMask.clear();
    m_kiwiSdrFftTraceFloorDbm = -1000.0f;
    m_kiwiSdrFftTraceFloorValid = false;
    if (m_kiwiSdrWaterfallActive) {
        clearCurrentWaterfallRows();
        coalescedUpdate();
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
        resetKiwiSdrWaterfallDisplayRange();
        clearCurrentWaterfallRows();
        coalescedUpdate();
    }
}

const QVector<float>& SpectrumWidget::displaySpectrumBins() const
{
    return m_kiwiSdrWaterfallActive ? m_kiwiSdrFftTrace : m_smoothed;
}

const QVector<float>& SpectrumWidget::buildFftDisplayTrace(const QVector<float>& bins,
                                                           int targetPoints) const
{
    const int srcCount = bins.size();
    if (srcCount < 2) {
        return bins;
    }

    // Display-only spatial smoothing: m_smoothed is temporal, so it reduces
    // frame shimmer but leaves adjacent-bin stair steps intact.
    //
    // #3932/#3967: the 5-tap blend exists to melt the radio's RBW stair-steps
    // when zoomed IN (bins repeat as plateaus once the span drops below the
    // FFT's resolution). Applied unconditionally (#3836) it low-passes every
    // trace — rounding off narrow carriers and defocusing the noise floor at
    // wide spans ("out of focus", 26.6.5). Gate the blend on the measured
    // plateau fraction so it only engages when stair-steps actually exist:
    // zoomed-in plateaus (long equal runs, frac >~0.65) get the full blend,
    // a busy wide span (frac <~0.35, adjacent noise bins rarely equal) gets
    // none, and the ramp between avoids a visible mode flip while zooming.
    int plateauPairs = 0;
    for (int i = 1; i < srcCount; ++i) {
        if (std::abs(bins[i] - bins[i - 1]) < 0.01f) {
            ++plateauPairs;
        }
    }
    const float plateauFrac =
        static_cast<float>(plateauPairs) / static_cast<float>(srcCount - 1);
    const float smoothBlend = kFftDisplaySpatialSmoothBlend
        * std::clamp((plateauFrac - 0.35f) / 0.30f, 0.0f, 1.0f);

    QVector<float>& displayBins = m_fftDisplaySmoothScratch;
    if (smoothBlend <= 0.0f) {
        displayBins = bins;
    } else {
        displayBins.resize(srcCount);
        displayBins[0] = bins[0];
        displayBins[srcCount - 1] = bins[srcCount - 1];
        for (int i = 1; i < srcCount - 1; ++i) {
            const float localSmooth = (i >= 2 && i + 2 < srcCount)
                ? (bins[i - 2] + 4.0f * bins[i - 1] + 6.0f * bins[i]
                   + 4.0f * bins[i + 1] + bins[i + 2]) * 0.0625f
                : (bins[i - 1] + 2.0f * bins[i] + bins[i + 1]) * 0.25f;
            displayBins[i] = bins[i] * (1.0f - smoothBlend)
                + localSmooth * smoothBlend;
        }
    }

    int dstCount = std::max(targetPoints, srcCount);
    dstCount = std::clamp(dstCount, 2, kMaxFftDisplayTracePoints);
    if (dstCount == srcCount) {
        return displayBins;
    }

    QVector<float>& trace = m_fftDisplayTraceScratch;
    trace.resize(dstCount);
    const double srcLast = static_cast<double>(srcCount - 1);
    const double dstLast = static_cast<double>(dstCount - 1);
    for (int dst = 0; dst < dstCount; ++dst) {
        const double srcPos = static_cast<double>(dst) * srcLast / dstLast;
        const int i1 = std::min(static_cast<int>(std::floor(srcPos)), srcCount - 1);
        if (i1 >= srcCount - 1) {
            trace[dst] = displayBins.constLast();
            continue;
        }

        const float t = static_cast<float>(srcPos - static_cast<double>(i1));
        const float p0 = displayBins[std::max(i1 - 1, 0)];
        const float p1 = displayBins[i1];
        const float p2 = displayBins[i1 + 1];
        const float p3 = displayBins[std::min(i1 + 2, srcCount - 1)];
        trace[dst] = clampedCatmullRom(p0, p1, p2, p3, t);
    }
    return trace;
}

const QVector<float>& SpectrumWidget::noiseFloorAutoLevelBins() const
{
    if (m_kiwiSdrWaterfallActive) {
        return m_kiwiSdrFftTrace;
    }
    return !m_smoothed.isEmpty() ? m_smoothed : m_bins;
}

void SpectrumWidget::setKiwiSdrWaterfallDisplayRange(float minDbm,
                                                     float maxDbm,
                                                     bool autoRange)
{
    if (!std::isfinite(minDbm) || !std::isfinite(maxDbm)) {
        return;
    }

    const float clampedCeil = qMax(minDbm + 1.0f, maxDbm);
    if (m_kiwiSdrDisplayRangeValid
        && m_kiwiSdrDisplayRangeAutoRange == autoRange
        && std::abs(m_kiwiSdrDisplayFloorDbm - minDbm) < 0.05f
        && std::abs(m_kiwiSdrDisplayCeilDbm - clampedCeil) < 0.05f) {
        return;
    }

    m_kiwiSdrDisplayFloorDbm = minDbm;
    m_kiwiSdrDisplayCeilDbm = clampedCeil;
    m_kiwiSdrDisplayRangeValid = true;
    m_kiwiSdrDisplayRangeAutoRange = autoRange;
    resetDssUploadState();
    if (m_kiwiSdrWaterfallActive) {
        coalescedUpdate();
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

    ++m_panStats.kiwiWaterfallCalls;
    if (!m_kiwiSdrWaterfallActive) {
        ++m_panStats.kiwiWaterfallHiddenCalls;
    }
    struct KiwiWaterfallIngestCost {
        quint64& acc;
        QElapsedTimer timer;
        explicit KiwiWaterfallIngestCost(quint64& value) : acc(value) { timer.start(); }
        ~KiwiWaterfallIngestCost() {
            acc += static_cast<quint64>(timer.nsecsElapsed() / 1000);
        }
    } ingestCost(m_panStats.kiwiWaterfallUs);
    if (!m_kiwiSdrWaterfallActive) {
        // Mirror the native path: retain the last Kiwi viewport snapshot while
        // hidden and resume from the first fresh row after the operator selects
        // Kiwi. Background rows must not consume GUI-thread render budget.
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
    const int destHeight = m_waterfall.height();
    if (destHeight <= 1) {
        return;
    }

    double rowLowMhz = lowFreqMhz;
    double rowHighMhz = highFreqMhz;
    if (rowHighMhz <= rowLowMhz || rowLowMhz < 0.0 || m_bandwidthMhz <= 0.0) {
        return;
    }
    const double rowCenterMhz = (rowLowMhz + rowHighMhz) * 0.5;
    const double rowBandwidthMhz = rowHighMhz - rowLowMhz;
    const double centerToleranceMhz = std::max(1.0e-9, rowBandwidthMhz * 1.0e-6);
    const double bandwidthToleranceMhz = std::max(1.0e-9, rowBandwidthMhz * 1.0e-6);
    if (m_kiwiSdrWaterfallActive) {
        ensureWaterfallHistory();
    }
    const bool sameWaterfallFrame = m_kiwiSdrLastWaterfallFrameValid
        && std::abs(rowCenterMhz - m_kiwiSdrLastWaterfallCenterMhz) <= centerToleranceMhz
        && std::abs(rowBandwidthMhz - m_kiwiSdrLastWaterfallBandwidthMhz) <= bandwidthToleranceMhz;
    const double rowLowBoundMhz = rowCenterMhz - rowBandwidthMhz * 0.5;
    const double rowHighBoundMhz = rowCenterMhz + rowBandwidthMhz * 0.5;
    const double viewLowBoundMhz = m_centerMhz - m_bandwidthMhz * 0.5;
    const double viewHighBoundMhz = m_centerMhz + m_bandwidthMhz * 0.5;
    const double overlapMhz = std::max(
        0.0,
        std::min(rowHighBoundMhz, viewHighBoundMhz)
            - std::max(rowLowBoundMhz, viewLowBoundMhz));
    const double viewCoverage = (m_bandwidthMhz > 0.0)
        ? overlapMhz / m_bandwidthMhz
        : 0.0;
    const bool rowHasUsableTraceCoverage = viewCoverage >= 0.05;
    const bool rowCanDriveAutoLevel =
        viewCoverage >= 0.98
        && !m_draggingPan
        && !m_draggingBandwidth
        && !m_frequencyRangeSettlePending;
    if (!sameWaterfallFrame) {
        m_kiwiSdrLastWaterfallBins.clear();
        m_kiwiSdrLastWaterfallCenterMhz = rowCenterMhz;
        m_kiwiSdrLastWaterfallBandwidthMhz = rowBandwidthMhz;
        m_kiwiSdrLastWaterfallFrameValid = true;
    }

    // Keep the trace/3DSS smoothing local to those consumers; the scrolling
    // waterfall below uses decoded bins so narrow Kiwi carriers stay sharp.
    const QVector<float> smoothedBins = smoothKiwiSdrWaterfallBins(binsDbm);
    bool pushedKiwiDssHistory = false;
    if (rowHasUsableTraceCoverage) {
        // 3DSS rows must be in the visible panadapter frequency frame. Raw Kiwi
        // rows often span a wider quantized server window, so feeding them
        // directly makes the stacked trace drift away from the remapped waterfall.
        const QVector<float> kiwiDssTrace = KiwiSdrTraceMath::mapRowToTrace(
            smoothedBins, DssRenderer::kCols, rowCenterMhz, rowBandwidthMhz,
            m_centerMhz, m_bandwidthMhz, kKiwiSdrWaterfallMinDbm);
        if (!kiwiDssTrace.isEmpty()) {
            pushDssRowForWaterfallStream(true, kiwiDssTrace);
            pushedKiwiDssHistory = true;
        }
    }
    if (destWidth > 0 && rowHasUsableTraceCoverage) {
        // The waterfall preserves narrow peaks, but the FFT line must not rise
        // just because a pan/zoom step changes the sampled row window. Average
        // the covered bins and normalize the row below.
        QVector<float> kiwiTrace = KiwiSdrTraceMath::mapRowToTrace(
            smoothedBins, destWidth, rowCenterMhz, rowBandwidthMhz,
            m_centerMhz, m_bandwidthMhz, kKiwiSdrWaterfallMinDbm);
        const QVector<quint8> kiwiTraceCoverage =
            KiwiSdrTraceMath::mapRowCoverageMask(
                smoothedBins.size(), destWidth, rowCenterMhz, rowBandwidthMhz,
                m_centerMhz, m_bandwidthMhz);
        stabilizeKiwiSdrFftTrace(kiwiTrace, rowCanDriveAutoLevel);
        if (m_kiwiSdrFftTrace.size() != kiwiTrace.size()) {
            m_kiwiSdrFftTrace = kiwiTrace;
            m_kiwiSdrFftFallbackSeedMask.clear();
        } else {
            const bool seedFallbackBins =
                m_kiwiSdrFftFallbackSeedMask.size() == kiwiTrace.size();
            const bool hasCoverageMask = kiwiTraceCoverage.size() == kiwiTrace.size();
            QVector<quint8> nextFallbackSeedMask;
            if (seedFallbackBins) {
                nextFallbackSeedMask = m_kiwiSdrFftFallbackSeedMask;
            }
            for (int i = 0; i < kiwiTrace.size(); ++i) {
                const bool pendingFallback =
                    seedFallbackBins && m_kiwiSdrFftFallbackSeedMask[i];
                if (hasCoverageMask && !kiwiTraceCoverage[i] && pendingFallback) {
                    continue;
                }
                if (pendingFallback) {
                    m_kiwiSdrFftTrace[i] = kiwiTrace[i];
                    nextFallbackSeedMask[i] = quint8(0);
                } else {
                    m_kiwiSdrFftTrace[i] = SMOOTH_ALPHA * kiwiTrace[i]
                        + (1.0f - SMOOTH_ALPHA) * m_kiwiSdrFftTrace[i];
                }
            }
            if (seedFallbackBins) {
                bool hasPendingFallback = false;
                for (quint8 pending : nextFallbackSeedMask) {
                    if (pending) {
                        hasPendingFallback = true;
                        break;
                    }
                }
                if (hasPendingFallback) {
                    m_kiwiSdrFftFallbackSeedMask = std::move(nextFallbackSeedMask);
                } else {
                    m_kiwiSdrFftFallbackSeedMask.clear();
                }
            } else {
                m_kiwiSdrFftFallbackSeedMask.clear();
            }
        }
        if (visibleStream && !m_kiwiSdrFftTrace.isEmpty()) {
            if (rowCanDriveAutoLevel) {
                const float previousMeasuredNoiseFloorDbm = m_measuredNoiseFloorDbm;
                const float frameFloor = estimateNoiseFloorDbm(m_kiwiSdrFftTrace);
                const float visualFloor =
                    estimateKiwiSdrVisualNoiseFloorDbm(m_kiwiSdrFftTrace);
                updateKiwiSdrSquelchVisualFloor(visualFloor);
                constexpr float kAlpha = 0.05f;
                m_measuredNoiseFloorDbm = (m_measuredNoiseFloorDbm <= -500.0f)
                    ? frameFloor
                    : m_measuredNoiseFloorDbm * (1.0f - kAlpha)
                        + frameFloor * kAlpha;
                if (m_kiwiSdrSquelchLineVisible
                    && (previousMeasuredNoiseFloorDbm <= -500.0f
                        || std::abs(previousMeasuredNoiseFloorDbm
                                    - m_measuredNoiseFloorDbm) > 0.25f)) {
                    markOverlayDirty();
                }

                const bool useFreshLockFrame = m_noiseFloorFreshFrameCount > 0;
                const bool noiseFloorFrameConsumed =
                    updateNoiseFloorBaseline(m_kiwiSdrFftTrace, useFreshLockFrame);
                if (useFreshLockFrame && noiseFloorFrameConsumed) {
                    --m_noiseFloorFreshFrameCount;
                }
                updateAutoSquelchFromBins(m_kiwiSdrFftTrace);
            }
        }
    }
    if (!pushedKiwiDssHistory) {
        pushDssRowForWaterfallStream(true, QVector<float>{}, -1.0, -1.0, false);
    }
    pushKiwiSdrWaterfallRow(binsDbm, destWidth,
                            rowCenterMhz, rowBandwidthMhz);
    if (visibleStream) {
        coalescedUpdate();
    }
}

// ─── Layout helpers ────────────────────────────────────────────────────────────

int SpectrumWidget::contentWidth() const
{
    // The right DBM_STRIP_W pixels are covered by the dBm scale (spectrum) and
    // the waterfall time-scale (both DBM_STRIP_W wide while live), so the usable
    // frequency canvas ends there.  Mapping frequency across this width — and
    // painting the trace/waterfall into it — keeps the band from hiding under
    // the tape and makes the pan-follow trigger symmetric in pixels (#3482).
    return std::max(1, width() - DBM_STRIP_W);
}

double SpectrumWidget::frequencyCanvasFractionAtGlobal(
    const QPointF& globalPosition) const
{
    // Native gestures are delivered through QWidget::event() rather than the
    // mouse handlers. Normalize every zoom path from the stable global point
    // so its anchor does not depend on how event propagation translated the
    // local position for a composited QRhiWidget.
    const QPointF localPosition = mapFromGlobal(globalPosition);
    return frequencyCanvasFraction(localPosition.x(), contentWidth());
}

int SpectrumWidget::mhzToX(double mhz) const
{
    if (m_bandwidthMhz <= 0.0) return -1;
    const double startMhz = m_centerMhz - m_bandwidthMhz / 2.0;
    const double px = (mhz - startMhz) / m_bandwidthMhz * contentWidth();
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
    return startMhz + (static_cast<double>(x) / contentWidth()) * m_bandwidthMhz;
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

    // A bare cursor-position change only changes the static overlay when the
    // cursor-frequency readout (#726) or a tune guide is actually drawn there.
    // With both off (the default) re-baking on every mouse-move produces a
    // byte-identical overlay and needlessly defeats the GPU overlay-upload cache
    // (#719) — measured ~6 ms/frame of pure waste while the cursor moves over the
    // panadapter. TNF-hover and tune-guide-visibility changes are genuine content
    // changes and still invalidate unconditionally.
    const bool cursorReadoutShown = m_showCursorFreq || m_tuneGuideVisible;
    if ((m_cursorPos != oldCursorPos && cursorReadoutShown)
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

    const SliceOverlay* ao = activeOverlay();
    for (const auto& so : m_sliceOverlays) {
        if (so.isActive) {
            continue;
        }
        if (overlaysAreAttachedDiversityPair(ao, so)) {
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

    if (ao) {
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
        const QPointF localPosition = mapFromGlobal(ev->globalPosition());
        m_bwDragStartX = static_cast<int>(std::lround(localPosition.x()));
        m_bwDragStartBw = m_bandwidthMhz;
        // Clamp to the canvas: the cursor can sit over the strip, and at
        // degenerate widths contentWidth()=1 would extrapolate the anchor
        // wildly off-canvas (#3482 review).
        m_bwDragAnchorFraction = frequencyCanvasFractionAtGlobal(
            ev->globalPosition());
        m_bwDragAnchorMhz = frequencyAtFraction(
            FrequencyFrame{m_centerMhz, m_bandwidthMhz},
            m_bwDragAnchorFraction);
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

            beginPanDrag(static_cast<int>(ev->position().x()));
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
            const bool is3D = (m_spectrumRenderMode == SpectrumRenderMode::Mode3D);
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
                float startMinDbm = m_refLevel - m_dynamicRange;
                float startMaxDbm = m_refLevel;
                if (clampDbmRange(startMinDbm, startMaxDbm)) {
                    m_refLevel = startMaxDbm;
                    m_dynamicRange = startMaxDbm - startMinDbm;
                }
                m_dbmDragStartRef = m_refLevel;
                m_dbmDragStartRange = m_dynamicRange;
                m_dbmDragStartBottom =
                    clampDbmBottom(m_refLevel - m_dynamicRange);
                setSpectrumCursor(Qt::SizeVerCursor);
                ev->accept();
                return;
            }

            if (primaryClick) {
                // Arrow row (side by side: left = up, right = down)
                if (y < DBM_ARROW_H) {
                    const float bottom =
                        clampDbmBottom(m_refLevel - m_dynamicRange);
                    const float oldRefLevel = m_refLevel;
                    const bool flex3dActive = is3D && !m_kiwiSdrWaterfallActive;
                    const DbmRangeTransition::Range oldRequestRange =
                        DbmRangeTransition::manualRequestRange(
                            bottom, oldRefLevel, flex3dActive,
                            peekDssFloorDbm(), oldRefLevel - bottom);
                    const float requestedRef =
                        m_refLevel + ((mx < stripX + DBM_STRIP_W / 2) ? 10.0f : -10.0f);
                    m_dynamicRange =
                        clampDbmRangeForBottom(bottom, requestedRef - bottom);
                    m_refLevel = bottom + m_dynamicRange;
                    const DbmRangeTransition::Range requestRange =
                        DbmRangeTransition::manualRequestRange(
                            bottom, m_refLevel, flex3dActive,
                            peekDssFloorDbm(), m_dynamicRange);
                    beginDbmRangeTransition(
                        oldRequestRange.minDbm, oldRequestRange.maxDbm,
                        requestRange.minDbm, requestRange.maxDbm);
                    markOverlayDirty();
                    refreshNoiseFloorTarget(true, true);
                    emit dbmRangeChangeRequested(
                        requestRange.minDbm, requestRange.maxDbm);
                    ev->accept();
                    return;
                }
                if (is3D) {
                    m_draggingDssFloor = true;
                    m_dbmDragStartY = y;
                    m_dssFloorDragStartDepth = -m_dssFloorOffsetDb;
                    setSpectrumCursor(Qt::SizeVerCursor);
                    ev->accept();
                    return;
                }
                // Below arrows: start dBm drag (pan reference)
                m_draggingDbm = true;
                m_dbmDragStartY = y;
                float startMinDbm = m_refLevel - m_dynamicRange;
                float startMaxDbm = m_refLevel;
                if (clampDbmRange(startMinDbm, startMaxDbm)) {
                    m_refLevel = startMaxDbm;
                    m_dynamicRange = startMaxDbm - startMinDbm;
                }
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

        auto sliceDisplayName = [](const SliceOverlay& so) {
            return SliceLabel::unicodeForm(so.sliceId, so.perClientLetter);
        };
        auto addCenterLockAction =
            [this, sliceDisplayName](QMenu* targetMenu, const SliceOverlay& so,
                                     bool includeCommandName) {
            const QString sliceName = QStringLiteral("Slice %1").arg(sliceDisplayName(so));
            QAction* action = targetMenu->addAction(
                includeCommandName
                    ? QStringLiteral("Center Lock %1").arg(sliceName)
                    : sliceName);
            action->setCheckable(true);
            action->setChecked(m_centerLockSliceId == so.sliceId);
            connect(action, &QAction::triggered, this,
                    [this, sliceId = so.sliceId]() {
                emit centerLockRequested(sliceId, m_centerLockSliceId != sliceId);
            });
        };
        auto addCenterLockControls = [this, addCenterLockAction](QMenu& targetMenu) {
            if (m_sliceOverlays.isEmpty()) {
                return;
            }

            targetMenu.addSeparator();
            if (m_sliceOverlays.size() == 1) {
                addCenterLockAction(&targetMenu, m_sliceOverlays.first(), true);
                return;
            }

            QMenu* centerLockMenu = targetMenu.addMenu(QStringLiteral("Center Lock"));
            const SliceOverlay* active = activeOverlay();
            if (active) {
                addCenterLockAction(centerLockMenu, *active, false);
            }
            for (const SliceOverlay& so : m_sliceOverlays) {
                if (active && so.sliceId == active->sliceId) {
                    continue;
                }
                addCenterLockAction(centerLockMenu, so, false);
            }
        };

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
                menu.addSeparator();
                addCenterLockAction(&menu, so, true);
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
            addCenterLockControls(menu);

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

            QAction* extendedPassbandAction = menu.addAction("Extended Passband");
            extendedPassbandAction->setCheckable(true);
            extendedPassbandAction->setChecked(m_extendedPassband);
            connect(extendedPassbandAction, &QAction::toggled, this, &SpectrumWidget::setExtendedPassband);

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
        const SliceOverlay* ao = activeOverlay();
        for (const auto& so : m_sliceOverlays) {
            if (so.isActive) {
                continue;
            }
            if (overlaysAreAttachedDiversityPair(ao, so)) {
                continue;
            }
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
                m_vfoDragPanEchoHoldUntilMs = 0;
                emit sliceDragActiveChanged(true);
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
            m_vfoDragPanEchoHoldUntilMs = 0;
            emit sliceDragActiveChanged(true);
            m_vfoDragLastX = mx;
            m_vfoDragOffsetHz = static_cast<int>(std::round((xToMhz(mx) - ao->freqMhz) * 1.0e6));
            setSpectrumCursor(Qt::ClosedHandCursor);
            ev->accept();
            return;
        }
    }

    // Click in FFT area → start pan drag (tune on double-click only)
    beginPanDrag(static_cast<int>(ev->position().x()));
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
    // contentWidth(): the right zone must end at the tape, not the widget edge
    // — otherwise its first DBM_STRIP_W px sit under the opaque strip and the
    // drag-to-edge feel is asymmetric vs the left (#3482).
    const int zonePx = std::max(1, static_cast<int>(contentWidth() * kVfoDragEdgeZoneFrac));
    const bool inEdgeZone = !m_vfoDragEdgePanDisabled
        && ((mx <= zonePx) || (mx >= contentWidth() - zonePx));
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
    const int w = contentWidth();  // zones frame the frequency canvas (#3482)
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

void SpectrumWidget::schedulePanDragDeferredUpdate()
{
    if (m_panDragDeferredUpdateScheduled) {
        return;
    }

    m_panDragDeferredUpdateScheduled = true;
    QTimer::singleShot(kPanDragFrameMs, this, [this]() {
        m_panDragDeferredUpdateScheduled = false;
        if (!m_draggingPan || !m_panDragPendingCenterValid) {
            return;
        }
        applyPanDragCenter(m_panDragPendingCenterMhz, false);
    });
}

void SpectrumWidget::schedulePanDragSettleUpdate()
{
    if (m_panDragSettleTimer) {
        m_panDragSettleTimer->start(kPanDragSettleMs);
    }
}

void SpectrumWidget::scheduleFrequencyRangeSettleUpdate(double centerMhz,
                                                        double bandwidthMhz)
{
    m_frequencyRangeSettlePending = true;
    if (std::isfinite(centerMhz) && std::isfinite(bandwidthMhz)
        && centerMhz > 0.0 && bandwidthMhz > 0.0) {
        m_frequencyRangePendingValid = true;
        m_frequencyRangePendingCenterMhz = centerMhz;
    }
    if (m_frequencyRangeSettleTimer) {
        m_frequencyRangeSettleTimer->start(kFrequencyRangeSettleMs);
    }
}

void SpectrumWidget::finishFrequencyRangeSettleUpdate()
{
    if (m_frequencyRangeSettleTimer) {
        m_frequencyRangeSettleTimer->stop();
    }
    if (!m_frequencyRangeSettlePending && !m_frequencyPreviewActive
        && !m_frequencyRangeCommandThrottle.hasPending()) {
        return;
    }

    commitFrequencyPreview();
    // Land the radio on the exact final center/bandwidth pair even when the
    // last high-rate gesture event fell inside the command throttle window.
    requestFrequencyRangeChange(m_centerMhz, m_bandwidthMhz, true);
    m_frequencyRangeSettlePending = false;
    m_frequencyRangePendingValid = false;
    emit frequencyRangeSettled(m_centerMhz, m_bandwidthMhz);
}

void SpectrumWidget::beginPanDrag(int startX)
{
    m_draggingPan = true;
    m_panDragStartX = startX;
    m_panDragStartCenter = m_centerMhz;
    m_panDragWaterfallFrameCenterMhz = m_centerMhz;
    m_panDragLastCommandCenterMhz = m_centerMhz;
    m_panDragPendingCenterMhz = m_centerMhz;
    m_panDragPendingCenterValid = false;
    m_panDragDeferredUpdateScheduled = false;
    m_panDragWaterfallClock.invalidate();
    m_panDragCommandClock.invalidate();
    if (m_panDragSettleTimer) {
        m_panDragSettleTimer->stop();
    }
}

void SpectrumWidget::applyPanDragCenter(double newCenterMhz, bool force)
{
    if (!std::isfinite(newCenterMhz)) {
        return;
    }

    bool needsDeferredFlush = false;
    const bool waterfallChanged =
        !mhzNearlyEqual(newCenterMhz, m_panDragWaterfallFrameCenterMhz);
    const bool previewed = waterfallChanged && updateFrequencyPreview(
        m_panDragWaterfallFrameCenterMhz, m_bandwidthMhz,
        newCenterMhz, m_bandwidthMhz);
    if (!previewed) {
        const bool waterfallDue = force || !m_panDragWaterfallClock.isValid()
            || m_panDragWaterfallClock.elapsed() >= kPanDragFrameMs;
        if (waterfallChanged && waterfallDue) {
            handleWaterfallFrequencyFrameChange(m_panDragWaterfallFrameCenterMhz,
                                                m_bandwidthMhz,
                                                newCenterMhz,
                                                m_bandwidthMhz);
            m_panDragWaterfallFrameCenterMhz = newCenterMhz;
            m_panDragWaterfallClock.restart();
        } else if (waterfallChanged) {
            needsDeferredFlush = true;
        }
    }
    if (waterfallChanged) {
        reprojectSpectrum(m_centerMhz, m_bandwidthMhz,
                          newCenterMhz, m_bandwidthMhz);
    }

    m_centerMhz = newCenterMhz;
    if (previewed) {
        scheduleFrequencyPreviewFrame();
    } else {
        markOverlayDirty();
    }

    const bool commandChanged =
        !mhzNearlyEqual(newCenterMhz, m_panDragLastCommandCenterMhz);
    const bool commandDue = force || !m_panDragCommandClock.isValid()
        || m_panDragCommandClock.elapsed() >= kPanDragCommandMs;
    if (commandChanged && commandDue) {
        emit centerChangeRequested(newCenterMhz);
        m_panDragLastCommandCenterMhz = newCenterMhz;
        m_panDragCommandClock.restart();
    } else if (commandChanged) {
        needsDeferredFlush = true;
    }

    if (force || !needsDeferredFlush) {
        m_panDragPendingCenterValid = false;
        return;
    }

    schedulePanDragDeferredUpdate();
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
        // Rebuild waterfall image for new size. Uses the shared helper (which
        // reads the m_spectrumFrac just assigned above) so the row count matches
        // paintEvent's destination rect exactly — truncating
        // contentH * (1 - frac) here instead would reintroduce the 1px
        // mismatch/static band on every divider drag.
        const int wfHeight = waterfallPixelHeight();
        // contentWidth(): rows rasterize at the displayed column count so the
        // blit into wfContentRect / the wf viewport is 1:1 — a full-width image
        // squeezed by DBM_STRIP_W nearest-drops ~36 columns per frame and a
        // narrow carrier on a dropped column vanishes from the waterfall while
        // still visible in the FFT (#3482).
        if (wfHeight > 0 && contentWidth() > 0) {
            QImage newWf(contentWidth(), wfHeight, QImage::Format_RGB32);
            newWf.fill(Qt::black);
            if (!m_waterfall.isNull()) {
                QImage scaled = m_waterfall.scaled(contentWidth(), wfHeight, Qt::IgnoreAspectRatio, Qt::FastTransformation);
                if (!scaled.isNull())
                    newWf = std::move(scaled);
            }
            m_waterfall = std::move(newWf);
            m_wfWriteRow = 0;
            ensureWaterfallHistory();
            if (m_wfHistoryRowCount > 0) {
                rebuildWaterfallViewport();
            } else {
                resetVisibleWaterfallFrequencyFrames(m_centerMhz,
                                                     m_bandwidthMhz);
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
        m_dynamicRange =
            clampDbmRangeForBottom(m_dbmDragStartBottom,
                                   m_dbmDragStartRange + deltaDb);
        m_refLevel = m_dbmDragStartBottom + m_dynamicRange;
        markOverlayDirty();
        ev->accept();
        return;
    }

    if (m_draggingDssFloor) {
        const int dragHeight = std::max(1, specH);
        const float previewDepth = DbmRangeTransition::floorDepthForDrag(
            m_dssFloorDragStartDepth, m_dbmDragStartY, y, dragHeight);
        // Keep the pointer path continuous and defer the AppSettings write
        // until release; the menu's integer slider still resolves to the
        // nearest dB while the rendered surface follows the exact preview.
        setDssFloorDepthForSource(
            m_kiwiSdrWaterfallActive, previewDepth, false);
        setSpectrumCursor(Qt::SizeVerCursor);
        ev->accept();
        return;
    }

    if (m_draggingDbm) {
        const int dragHeight = std::max(1, specH);
        const int dy = y - m_dbmDragStartY;
        // Convert pixel drag to dB: full FFT height = full dynamic range
        const float deltaDb = (static_cast<float>(dy) / dragHeight) * m_dynamicRange;
        m_refLevel = m_dbmDragStartRef + deltaDb;
        m_refLevel = clampDbmRefForRange(m_refLevel, m_dynamicRange);
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
        const QPointF localPosition = mapFromGlobal(ev->globalPosition());
        const int dx = static_cast<int>(std::lround(localPosition.x()))
            - m_bwDragStartX;
        // 4x multiplier: dragging 1/4 of widget width doubles/halves bandwidth
        const double scale = std::pow(2.0, static_cast<double>(-dx) / (width() / 4.0));
        const double newBw = std::clamp(m_bwDragStartBw * scale, m_minBwMhz, m_maxBwMhz);
        const double zoomCenter = std::max(
            centerForAnchoredBandwidth(
                m_bwDragAnchorMhz, m_bwDragAnchorFraction, newBw),
            newBw / 2.0);
        const bool previewed = updateFrequencyPreview(
            m_centerMhz, m_bandwidthMhz, zoomCenter, newBw);
        if (!previewed) {
            handleWaterfallFrequencyFrameChange(m_centerMhz, m_bandwidthMhz,
                                                zoomCenter, newBw);
        }
        if (!reprojectSpectrum(m_centerMhz, m_bandwidthMhz, zoomCenter, newBw)) {
            m_bins.clear();
            m_smoothed.clear();
            m_resetFftSmoothingOnNextFrame = true;
        }
        m_bandwidthMhz = newBw;
        m_centerMhz = zoomCenter;
        resetNoiseFloorBaseline();
        if (previewed) {
            scheduleFrequencyPreviewFrame();
        } else {
            markOverlayDirty();
        }
        // Keep center and bandwidth coupled while dragging. Sending only the
        // bandwidth and waiting to send center on release caused the radio and
        // client waterfall to diverge under trackpad-heavy zoom workflows.
        scheduleFrequencyRangeSettleUpdate(zoomCenter, newBw);
        requestFrequencyRangeChange(zoomCenter, newBw);
        ev->accept();
        return;
    }

    if (m_draggingFilter != FilterEdge::None) {
        auto* ao = const_cast<SliceOverlay*>(activeOverlay());
        if (!ao) { m_draggingFilter = FilterEdge::None; return; }
        const int mx = static_cast<int>(ev->position().x());
        // Compute Hz delta from pixel delta — immune to freq/overlay changes (#764)
        const double hzPerPx = (m_bandwidthMhz * 1.0e6) / contentWidth();
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
        const double deltaMhz = -(static_cast<double>(dx) / contentWidth()) * m_bandwidthMhz;
        const double newCenter = std::max(m_panDragStartCenter + deltaMhz,
                                          m_bandwidthMhz / 2.0);
        m_panDragPendingCenterMhz = newCenter;
        m_panDragPendingCenterValid = true;
        applyPanDragCenter(newCenter, false);
        schedulePanDragSettleUpdate();
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
                const bool is3D = (m_spectrumRenderMode == SpectrumRenderMode::Mode3D);
#ifdef Q_OS_MAC
                const QString rangeDragTip =
                    "Ctrl-drag or &#8984;-drag &mdash; zoom span (anchor at bottom)<br>";
#else
                const QString rangeDragTip =
                    "Ctrl-drag &mdash; zoom span (anchor at bottom)<br>";
#endif
                const QString tip =
                    QString(is3D
                        ? "<b>3D dBm scale</b><br>"
                          "Drag &mdash; adjust 3D Floor<br>%1"
                          "&#9650; / &#9660; &mdash; &plusmn;10 dB steps"
                        : "<b>dBm scale</b><br>"
                          "Drag &mdash; pan reference level<br>%1"
                          "&#9650; / &#9660; &mdash; &plusmn;10 dB steps")
                    .arg(rangeDragTip);
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
                if (!foundCursor) {
                    Qt::CursorShape cursorShape = Qt::ArrowCursor;
                    if (sliceCursorShapeAt(pos, cursorShape)) {
                        setSpectrumCursor(cursorShape);
                        foundCursor = true;
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
    if (m_draggingDssFloor) {
        m_draggingDssFloor = false;
        setDssFloorDepthForSource(
            m_kiwiSdrWaterfallActive, -m_dssFloorOffsetDb, true);
        setSpectrumCursor(Qt::CrossCursor);

        if (!m_kiwiSdrWaterfallActive
            && std::abs((-m_dssFloorOffsetDb) - m_dssFloorDragStartDepth)
                > kDbmReleasePreviewChangeThresholdDb) {
            const DbmRangeTransition::Range oldRange{
                m_refLevel - m_dynamicRange, m_refLevel};
            float requestedMinDbm = peekDssFloorDbm();
            float requestedMaxDbm = requestedMinDbm + dssSpanDb();
            if (clampDbmRange(requestedMinDbm, requestedMaxDbm)) {
                const DbmRangeTransition::Range requestedRange{
                    requestedMinDbm, requestedMaxDbm};
                if (DbmRangeTransition::materiallyDifferent(
                        oldRange, requestedRange,
                        kDbmReleasePreviewChangeThresholdDb)) {
                    beginDbmRangeTransition(
                        oldRange.minDbm, oldRange.maxDbm,
                        requestedRange.minDbm, requestedRange.maxDbm);
                    m_refLevel = requestedRange.maxDbm;
                    m_dynamicRange =
                        requestedRange.maxDbm - requestedRange.minDbm;
                    markOverlayDirty();
                    refreshNoiseFloorTarget(true, true);
                    emit dbmRangeDragFinished(
                        requestedRange.minDbm, requestedRange.maxDbm);
                }
            }
        }
        ev->accept();
        return;
    }
    if (m_draggingDbm || m_draggingDbmRange) {
        const float oldMinDbm = m_draggingDbmRange
            ? m_dbmDragStartBottom
            : clampDbmBottom(m_dbmDragStartRef - m_dynamicRange);
        const float oldMaxDbm = m_dbmDragStartRef;
        float pendingMinDbm = m_refLevel - m_dynamicRange;
        float pendingMaxDbm = m_refLevel;
        if (!clampDbmRange(pendingMinDbm, pendingMaxDbm)) {
            m_draggingDbm = false;
            m_draggingDbmRange = false;
            setSpectrumCursor(Qt::CrossCursor);
            ev->accept();
            return;
        }
        const bool flex3dActive =
            m_spectrumRenderMode == SpectrumRenderMode::Mode3D
            && !m_kiwiSdrWaterfallActive;
        const DbmRangeTransition::Range oldRequestRange =
            DbmRangeTransition::manualRequestRange(
                oldMinDbm, oldMaxDbm, flex3dActive,
                peekDssFloorDbm(),
                oldMaxDbm - oldMinDbm);
        const DbmRangeTransition::Range requestRange =
            DbmRangeTransition::manualRequestRange(
                pendingMinDbm, pendingMaxDbm, flex3dActive,
                peekDssFloorDbm(),
                pendingMaxDbm - pendingMinDbm);
        m_refLevel = pendingMaxDbm;
        m_dynamicRange = pendingMaxDbm - pendingMinDbm;
        beginDbmRangeTransition(
            oldRequestRange.minDbm, oldRequestRange.maxDbm,
            requestRange.minDbm, requestRange.maxDbm);
        m_draggingDbm = false;
        m_draggingDbmRange = false;
        setSpectrumCursor(Qt::CrossCursor);
        refreshNoiseFloorTarget(true, true);
        emit dbmRangeDragFinished(requestRange.minDbm, requestRange.maxDbm);
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
        scheduleFrequencyRangeSettleUpdate(m_centerMhz, m_bandwidthMhz);
        finishFrequencyRangeSettleUpdate();
        ev->accept();
        return;
    }
    if (m_draggingVfo) {
        m_draggingVfo = false;
        m_vfoDragPanEchoHoldUntilMs = QDateTime::currentMSecsSinceEpoch() + 350;
        emit sliceDragActiveChanged(false);
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
        if (m_panDragSettleTimer) {
            m_panDragSettleTimer->stop();
        }
        applyPanDragCenter(m_centerMhz, true);
        commitFrequencyPreview();
        m_panDragWaterfallFrameCenterMhz = m_centerMhz;
        emit panDragSettled(m_centerMhz, m_bandwidthMhz);
        m_panDragPendingCenterValid = false;
        m_panDragDeferredUpdateScheduled = false;
        m_panDragWaterfallClock.invalidate();
        m_panDragCommandClock.invalidate();
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
        const double clickX = std::clamp(ev->position().x(), 0.0,
                                         static_cast<double>(contentWidth()));
        double rawMhz = startMhz + (clickX / contentWidth()) * m_bandwidthMhz;

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

void SpectrumWidget::coalescedUpdate()
{
    // Data-driven repaints (FFT frames, waterfall rows) coalesce into one
    // present per slot; interactive paths still call update() directly so
    // input latency is unaffected. No frame is dropped — a trailing update
    // presents whatever arrived inside the slot.
    const int slotMs = kPresentCoalesceMs;
    if (m_renderScheduler) {
        // Delegate to the shared cross-pan scheduler, which coalesces update()
        // across all panes into batched flushes instead of each pane pacing its
        // own present. Note the scheduler shares one present clock across panes,
        // so a pane's first frame after idle can trail another pane's present by
        // up to one slot (bounded, no frame lost). (#4139)
        m_renderScheduler->requestDataFrame(this, slotMs);
        return;
    }
    if (!m_presentCoalesceClock.isValid()) {
        m_presentCoalesceClock.start();
        update();
        return;
    }
    const qint64 sinceMs = m_presentCoalesceClock.elapsed();
    if (sinceMs >= slotMs) {
        m_presentCoalesceClock.restart();
        update();
        return;
    }
    if (!m_presentPending) {
        m_presentPending = true;
        QTimer::singleShot(static_cast<int>(slotMs - sinceMs), this, [this]() {
            m_presentPending = false;
            m_presentCoalesceClock.restart();
            update();
        });
    }
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

#ifdef AETHER_GPU_SPECTRUM
    // Moving between monitors changes the DPR without a resize event; keep
    // the pinned color-buffer size in device pixels current (#4091).
    if (ev->type() == QEvent::DevicePixelRatioChange) {
        updateFixedColorBufferSize();
    }
#endif

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
            const double mouseXFrac = frequencyCanvasFractionAtGlobal(
                ge->globalPosition());
            const double anchorMhz = frequencyAtFraction(
                FrequencyFrame{m_centerMhz, m_bandwidthMhz}, mouseXFrac);
            const double newCenter = std::max(
                centerForAnchoredBandwidth(anchorMhz, mouseXFrac, newBw),
                newBw / 2.0);
            const bool previewed = updateFrequencyPreview(
                m_centerMhz, m_bandwidthMhz, newCenter, newBw);
            if (!previewed) {
                handleWaterfallFrequencyFrameChange(m_centerMhz, m_bandwidthMhz,
                                                    newCenter, newBw);
            }
            if (!reprojectSpectrum(m_centerMhz, m_bandwidthMhz, newCenter, newBw)) {
                m_bins.clear();
                m_smoothed.clear();
                m_resetFftSmoothingOnNextFrame = true;
            }
            m_bandwidthMhz = newBw;
            m_centerMhz = newCenter;
            resetNoiseFloorBaseline();
            if (previewed) {
                scheduleFrequencyPreviewFrame();
            } else {
                markOverlayDirty();
            }
            scheduleFrequencyRangeSettleUpdate(newCenter, newBw);
            requestFrequencyRangeChange(newCenter, newBw);
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
        const double mouseXFrac = frequencyCanvasFractionAtGlobal(
            ev->globalPosition());
        const double anchorMhz = frequencyAtFraction(
            FrequencyFrame{m_centerMhz, m_bandwidthMhz}, mouseXFrac);
        const double newCenter = std::max(
            centerForAnchoredBandwidth(anchorMhz, mouseXFrac, newBw),
            newBw / 2.0);
        const bool previewed = updateFrequencyPreview(
            m_centerMhz, m_bandwidthMhz, newCenter, newBw);
        if (!previewed) {
            handleWaterfallFrequencyFrameChange(m_centerMhz, m_bandwidthMhz,
                                                newCenter, newBw);
        }
        if (!reprojectSpectrum(m_centerMhz, m_bandwidthMhz, newCenter, newBw)) {
            m_bins.clear();
            m_smoothed.clear();
            m_resetFftSmoothingOnNextFrame = true;
        }
        m_centerMhz    = newCenter;
        m_bandwidthMhz = newBw;
        resetNoiseFloorBaseline();
        if (previewed) {
            scheduleFrequencyPreviewFrame();
        } else {
            markOverlayDirty();
        }
        scheduleFrequencyRangeSettleUpdate(newCenter, newBw);
        requestFrequencyRangeChange(newCenter, newBw);
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

#ifdef AETHER_GPU_SPECTRUM
void SpectrumWidget::updateFixedColorBufferSize()
{
    // QRhiWidget's automatic color buffer is widget-size × dpr. A fractional
    // QT_SCALE_FACTOR (UiScalePercent ≠ 100, e.g. 0.85) turns that into
    // odd-sized texture extents on most resizes, and the 2016-era Intel HD
    // Graphics D3D11 UMD (igd10iumd64.dll 21.20.16.x) null-derefs recreating
    // such textures during the add-pan resize (#4091). Pin the buffer to the
    // ceil'd, even-aligned device-pixel size instead. The render path derives
    // its scale from renderTarget()->pixelSize(), not width()×dpr, so the
    // ≤1 px overshoot is invisible.
    const qreal dpr = devicePixelRatioF();
    QSize devicePx(static_cast<int>(std::ceil(width() * dpr)),
                   static_cast<int>(std::ceil(height() * dpr)));
    if (devicePx.isEmpty()) {
        return;
    }
    devicePx.rwidth()  += devicePx.width()  & 1;
    devicePx.rheight() += devicePx.height() & 1;
    if (fixedColorBufferSize() != devicePx) {
        setFixedColorBufferSize(devicePx);
    }
}
#endif

void SpectrumWidget::applySettledResizeBuffers()
{
    QElapsedTimer commitTimer;
    commitTimer.start();
    bool buffersChanged = false;

    // This logical size owns the fixed render target, overlay images, FFT
    // columns, and waterfall until the next settled commit. During a live
    // window resize, render into this complete old-size frame and let
    // QRhiWidget scale it to the widget's intermediate geometry.
    if (width() > 0 && height() > 0) {
        m_resizePresentationSize = size();
    }

#ifdef AETHER_GPU_SPECTRUM
    const QSize previousRenderBufferSize = fixedColorBufferSize();
    updateFixedColorBufferSize();
    buffersChanged = fixedColorBufferSize() != previousRenderBufferSize;
#endif

    // Match the remainder-based split used by painting and GPU presentation,
    // so the stored row count cannot differ from its destination by one pixel.
    const int wfHeight = waterfallPixelHeight();
    const QSize waterfallSize(contentWidth(), wfHeight);
    if (waterfallSize.isEmpty()) {
        if (buffersChanged) {
            m_resizeBufferCommitLastNs = commitTimer.nsecsElapsed();
            m_resizeBufferCommitMaxNs = std::max(
                m_resizeBufferCommitMaxNs, m_resizeBufferCommitLastNs);
            ++m_resizeBufferCommitCount;
            update();
        }
        return;
    }

    const bool waterfallChanged = m_waterfall.size() != waterfallSize;
    if (!waterfallChanged && !buffersChanged) {
        return;
    }

    if (waterfallChanged) {
        QImage newWf(waterfallSize, QImage::Format_RGB32);
        newWf.fill(Qt::black);
        if (!m_waterfall.isNull()) {
            QImage scaled = m_waterfall.scaled(
                waterfallSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            if (!scaled.isNull()) {
                newWf = std::move(scaled);
            }
        }
        m_waterfall = std::move(newWf);
        m_wfWriteRow = 0;
        ensureWaterfallHistory();
        if (m_wfHistoryRowCount > 0) {
            rebuildWaterfallViewport();
        } else {
            resetVisibleWaterfallFrequencyFrames(m_centerMhz,
                                                 m_bandwidthMhz);
#ifdef AETHER_GPU_SPECTRUM
            m_wfTexFullUpload = true;
#endif
        }
    }

    m_resizeBufferCommitLastNs = commitTimer.nsecsElapsed();
    m_resizeBufferCommitMaxNs = std::max(
        m_resizeBufferCommitMaxNs, m_resizeBufferCommitLastNs);
    ++m_resizeBufferCommitCount;
    update();
}

void SpectrumWidget::resizeEvent(QResizeEvent* ev)
{
    SPECTRUM_BASE_CLASS::resizeEvent(ev);
    ++m_resizeEventCount;
    if (m_frequencyPreviewActive) {
        commitFrequencyPreview();
    }

    // Keep every size-dependent buffer at the last completed size while the
    // window is moving. QRhiWidget scales that fixed color buffer to the
    // widget's current geometry while render() continues presenting new FFT
    // and waterfall data into it. The render target, overlays, FFT columns,
    // waterfall, and its 24,000-row compact history still coalesce to one
    // commit.
    if (m_waterfall.isNull()) {
        applySettledResizeBuffers();
    } else if (m_resizeBufferSettleTimer) {
        m_resizeBufferSettleTimer->start();
    }

    // Re-assert mouse tracking — on macOS with WA_NativeWindow, reparenting
    // into a QSplitter can reset native window properties.
    setMouseTracking(true);

    // Position GPU renderer to cover FFT + waterfall area


    positionZoomButtons();
    positionFpsMeterLabels();
    positionPanadapterMessageOverlay();

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
    if (m_kiwiSdrDisplaySourceBtn) {
        const int sourceY = botY - (sz * 2)
            - m_kiwiSdrDisplaySourceBtn->height() - 4;
        m_kiwiSdrDisplaySourceBtn->move(pad, sourceY);
    }
}

void SpectrumWidget::positionPanadapterMessageOverlay()
{
    if (!m_panadapterMessageOverlay) {
        return;
    }

    const QRect contentRect(0, 0, qMax(0, width() - DBM_STRIP_W), height());
    m_panadapterMessageOverlay->setGeometry(contentRect);
    raisePanadapterMessageOverlay();
}

void SpectrumWidget::raisePanadapterMessageOverlay()
{
    if (m_panadapterMessageOverlay && m_panadapterMessageOverlay->hasMessages()) {
        m_panadapterMessageOverlay->raise();
    }
}

// ─── Colour map ───────────────────────────────────────────────────────────────

QRgb SpectrumWidget::dbmToRgb(float dbm) const
{
    return waterfallLevelToRgb(dbmToWaterfallLevel(dbm));
}

float SpectrumWidget::dbmToWaterfallLevel(float dbm) const
{
    // Black level shifts the floor: higher black_level = more of the noise is black.
    // Color gain controls the visible range: higher gain = narrower range = more contrast.
    // black_level 0-125: higher = floor moves closer to signals (more black)
    // color_gain 0-100: higher = narrower visible range = more contrast
    const float floorShift = (125 - m_wfBlackLevel) * 0.4f;  // inverted: 0=max shift, 125=no shift
    const float visRange = 80.0f - m_wfColorGain * 0.7f;  // 80 dB down to 10 dB
    const float effectiveMin = m_wfMinDbm + floorShift;
    const float effectiveMax = effectiveMin + visRange;

    return qBound(0.0f,
                  (dbm - effectiveMin) / (effectiveMax - effectiveMin),
                  1.0f);
}

QRgb SpectrumWidget::dssStrengthToRgb(float s) const
{
    // gamma in [0.25 .. 4]: gain=100 -> 0.25 (colour lifted to the noise floor),
    // gain=50 -> 1.0 (linear), gain=0 -> 4 (colour only on the strongest peaks).
    const float gamma = std::pow(4.0f, (50.0f - m_dssGain) / 50.0f);
    int n = 0;
    const auto* stops = wfSchemeStops(m_wfColorScheme, n);
    return interpolateGradient(std::pow(std::clamp(s, 0.0f, 1.0f), gamma),
                               stops, n);
}

quint64 SpectrumWidget::dssPaletteToken() const
{
    // Fold the inputs that define the 3DSS surface colour so the cached image
    // recolours when any change. The surface now maps strength through the
    // scheme + "3D Gain" (dssStrengthToRgb); the waterfall gain/black/min are
    // kept here too since they still affect the 2D/waterfall colour path.
    quint64 t = static_cast<quint64>(m_wfColorScheme);
    t = t * 131 + static_cast<quint64>(m_dssGain);
    t = t * 131 + static_cast<quint64>(m_wfColorGain);
    t = t * 131 + static_cast<quint64>(m_wfBlackLevel);
    t = t * 131 + static_cast<quint64>(qRound(m_wfMinDbm));
    return t;
}

float SpectrumWidget::dssFloorDbm()
{
    // 3D uses the FFT trace floor, not the waterfall colour aperture. Some
    // KiwiSDRs publish a manual display range whose floor is well above the
    // actual trace floor; using that colour floor collapses the 3D ridge to the
    // baseline. The Kiwi trace floor is stabilized by stabilizeKiwiSdrFftTrace().
    // Flex FFT floor estimates vary slightly frame-to-frame, so Flex follows a
    // smoothed anchor instead of re-anchoring the surface to every raw frame.
    float floor;
    if (m_kiwiSdrWaterfallActive) {
        if (m_kiwiSdrFftTraceFloorValid && m_kiwiSdrFftTraceFloorDbm > -500.0f) {
            floor = m_kiwiSdrFftTraceFloorDbm;
        } else if (!m_kiwiSdrFftTrace.isEmpty()) {
            const float frameFloor = estimateKiwiSdrTraceFloorDbm(m_kiwiSdrFftTrace);
            floor = frameFloor > -500.0f
                ? frameFloor
                : (m_kiwiSdrDisplayRangeValid
                    ? m_kiwiSdrDisplayFloorDbm
                    : m_refLevel - m_dynamicRange);
        } else {
            floor = m_kiwiSdrDisplayRangeValid
                ? m_kiwiSdrDisplayFloorDbm
                : m_refLevel - m_dynamicRange;
        }
    } else {
        auto updateAnchor = [this](float targetFloor) {
            if (!std::isfinite(targetFloor) || targetFloor <= -500.0f) {
                return;
            }
            if (!m_dssFloorAnchorValid || m_dssFloorAnchorDbm <= -500.0f
                || std::abs(targetFloor - m_dssFloorAnchorDbm) > 18.0f) {
                m_dssFloorAnchorDbm = targetFloor;
                m_dssFloorAnchorValid = true;
                return;
            }
            constexpr float kAnchorAlpha = 0.04f;
            m_dssFloorAnchorDbm =
                (1.0f - kAnchorAlpha) * m_dssFloorAnchorDbm
                + kAnchorAlpha * targetFloor;
        };

        if (m_noiseFloorEnable && m_noiseFloorBaselineValid) {
            updateAnchor(m_noiseFloorBaselineDbm);
        } else if (m_measuredNoiseFloorDbm > -500.0f) {
            updateAnchor(m_measuredNoiseFloorDbm);
        } else if (m_noiseFloorBaselineValid) {
            updateAnchor(m_noiseFloorBaselineDbm);
        }

        if (!m_dssFloorAnchorValid) {
            const QVector<float>& bins = noiseFloorAutoLevelBins();
            if (!bins.isEmpty()) {
                const float frameFloor = estimateNoiseFloorDbm(bins);
                if (frameFloor > -500.0f) {
                    m_dssFloorAnchorDbm = frameFloor;
                    m_dssFloorAnchorValid = true;
                }
            }
            if (!m_dssFloorAnchorValid && m_noiseFloorBaselineValid) {
                m_dssFloorAnchorDbm = m_noiseFloorBaselineDbm;
                m_dssFloorAnchorValid = true;
            }
            if (!m_dssFloorAnchorValid && m_measuredNoiseFloorDbm > -500.0f) {
                m_dssFloorAnchorDbm = m_measuredNoiseFloorDbm;
                m_dssFloorAnchorValid = true;
            }
        }
        floor = m_dssFloorAnchorValid
            ? m_dssFloorAnchorDbm
            : m_refLevel - m_dynamicRange;
    }
    return std::round(floor * 2.0f) / 2.0f + m_dssFloorOffsetDb;
}

float SpectrumWidget::peekDssFloorDbm() const
{
    float floor;
    if (m_kiwiSdrWaterfallActive) {
        if (m_kiwiSdrFftTraceFloorValid
            && m_kiwiSdrFftTraceFloorDbm > -500.0f) {
            floor = m_kiwiSdrFftTraceFloorDbm;
        } else if (!m_kiwiSdrFftTrace.isEmpty()) {
            const float frameFloor =
                estimateKiwiSdrTraceFloorDbm(m_kiwiSdrFftTrace);
            floor = frameFloor > -500.0f
                ? frameFloor
                : (m_kiwiSdrDisplayRangeValid
                    ? m_kiwiSdrDisplayFloorDbm
                    : m_refLevel - m_dynamicRange);
        } else {
            floor = m_kiwiSdrDisplayRangeValid
                ? m_kiwiSdrDisplayFloorDbm
                : m_refLevel - m_dynamicRange;
        }
    } else {
        if (m_dssFloorAnchorValid && m_dssFloorAnchorDbm > -500.0f) {
            floor = m_dssFloorAnchorDbm;
        } else if (m_noiseFloorEnable && m_noiseFloorBaselineValid
                   && m_noiseFloorBaselineDbm > -500.0f) {
            floor = m_noiseFloorBaselineDbm;
        } else if (m_measuredNoiseFloorDbm > -500.0f) {
            floor = m_measuredNoiseFloorDbm;
        } else if (m_noiseFloorBaselineValid
                   && m_noiseFloorBaselineDbm > -500.0f) {
            floor = m_noiseFloorBaselineDbm;
        } else {
            floor = m_refLevel - m_dynamicRange;
            const QVector<float>& bins = noiseFloorAutoLevelBins();
            if (!bins.isEmpty()) {
                const float frameFloor = estimateNoiseFloorDbm(bins);
                if (frameFloor > -500.0f) {
                    floor = frameFloor;
                }
            }
        }
    }
    return std::round(floor * 2.0f) / 2.0f + m_dssFloorOffsetDb;
}

float SpectrumWidget::dssSpanDb() const
{
    // The dB-per-height scale follows the normal panadapter dBm range, not the
    // 3D Floor depth. The floor control shifts the surface reference; Ctrl-drag
    // on the dBm strip remains the gesture that changes the scale/span. Keep the
    // normal 10 dB lower bound so arrows and drags remain responsive at narrow
    // ranges; the radio-range handoff keeps FFT bins correctly encoded there.
    return DbmRangeTransition::displaySpanDb(m_dynamicRange);
}

const QImage& SpectrumWidget::buildDssImage(const QSize& px,
                                            int scaleStripPx,
                                            float floorDbm)
{
    const float rangeDb  = std::round(dssSpanDb() * 2.0f) / 2.0f;

    // Same mapping as the GPU mesh: full colormap over the strength axis, gamma-
    // shaped by "3D Gain" (NOT dbmToRgb, which clipped the low range to black).
    auto palette = [this, floorDbm, rangeDb](float dbm) {
        const float r = (rangeDb > 0.0f) ? rangeDb : 1.0f;
        return dssStrengthToRgb((dbm - floorDbm) / r);
    };
    return m_dss.image(px, scaleStripPx, floorDbm, rangeDb, m_dssZCurve,
                       palette, dssPaletteToken(), m_bgFillColor);
}

void SpectrumWidget::pushDssRowForWaterfallStream(bool kiwiStream,
                                                  const QVector<float>& binsDbm,
                                                  double frameCenterMhz,
                                                  double frameBandwidthMhz,
                                                  bool updateLiveSurface)
{
    if (m_kiwiSdrWaterfallActive == kiwiStream) {
        appendDssWaterfallRow(binsDbm, frameCenterMhz, frameBandwidthMhz,
                              updateLiveSurface);
        return;
    }

    DssRenderer& dss = kiwiStream
        ? activeKiwiWaterfallState().dss
        : m_nativeWaterfallState.dss;
    const bool live = kiwiStream
        ? activeKiwiWaterfallState().live
        : m_nativeWaterfallState.live;
    if (live && updateLiveSurface) {
        pushDssLiveRow(dss, binsDbm, true);
    }

    const FrequencyFrame requestedFrame{frameCenterMhz, frameBandwidthMhz};
    const FrequencyFrame stampFrame = requestedFrame.isValid()
        ? requestedFrame
        : FrequencyFrame{m_centerMhz, m_bandwidthMhz};
    const float fallbackDbm = kiwiStream
        ? kKiwiSdrWaterfallMinDbm
        : m_refLevel - m_dynamicRange;
    retainDssHistoryRow(dss, binsDbm,
                        stampFrame.centerMhz, stampFrame.bandwidthMhz,
                        fallbackDbm);
}

void SpectrumWidget::resetDssUploadState()
{
    m_dss.invalidate();
#ifdef AETHER_GPU_SPECTRUM
    m_dssTexNeedsUpload = true;
    m_dssLastUploadedGen = ~0ull;
    m_dssMeshHeadUploaded = -1;
    m_dssMeshRowGenUploaded = ~0ull;
#endif
}

QRgb SpectrumWidget::kiwiSdrLevelToRgb(float level) const
{
    return waterfallLevelToRgb(kiwiSdrWaterfallLevel(level));
}

float SpectrumWidget::kiwiSdrWaterfallLevel(float level) const
{
    // Kiwi direct W/F bytes are decoded to the server's wrapped negative dB
    // scale; color aperture is still Kiwi-only and independent of Flex.
    const KiwiSdrProtocol::WaterfallDisplayRange defaultRange =
        KiwiSdrProtocol::defaultWaterfallDisplayRange(
            0,
            0,
            0);
    const float floorDbm = m_kiwiSdrDisplayRangeValid
        ? m_kiwiSdrDisplayFloorDbm
        : defaultRange.minDbm;
    const float ceilDbm = m_kiwiSdrDisplayRangeValid
        ? m_kiwiSdrDisplayCeilDbm
        : defaultRange.maxDbm;
    const float adjustedCeilDbm = qMax(floorDbm + 1.0f, ceilDbm);
    return KiwiSdrProtocol::waterfallColorIndex(
        level, floorDbm, adjustedCeilDbm);
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
    return waterfallLevelToRgb(intensityToWaterfallLevel(intensity));
}

float SpectrumWidget::intensityToWaterfallLevel(float intensity) const
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

    return qBound(0.0f, (intensity - blackThresh) / rangeWidth, 1.0f);
}

QRgb SpectrumWidget::waterfallLevelToRgb(float level) const
{
    int n = 0;
    const auto* stops = wfSchemeStops(m_wfColorScheme, n);
    return interpolateGradient(std::clamp(level, 0.0f, 1.0f), stops, n);
}

quint8 SpectrumWidget::encodeWaterfallLevel(float level)
{
    return static_cast<quint8>(std::lround(
        std::clamp(level, 0.0f, 1.0f) * 255.0f));
}

std::array<QRgb, 256> SpectrumWidget::waterfallHistoryColorLut() const
{
    std::array<QRgb, 256> lut{};
    for (int level = 0; level < static_cast<int>(lut.size()); ++level) {
        lut[static_cast<size_t>(level)] = waterfallLevelToRgb(
            static_cast<float>(level) / 255.0f);
    }
    return lut;
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

    const std::array<QRgb, 256> colorLut = waterfallHistoryColorLut();
    QVector<quint8> levels(destWidth, 0);
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
        const float level = dbmToWaterfallLevel(dbm);
        levels[x] = encodeWaterfallLevel(level);
        scanline[x] = colorLut[levels[x]];
    }

    appendHistoryRow(levels.constData(), QDateTime::currentMSecsSinceEpoch());
    appendDssWaterfallRow(bins);
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
    if (rowCenterMhz <= 0.0 || rowBandwidthMhz <= 0.0) {
        rowCenterMhz = m_centerMhz;
        rowBandwidthMhz = m_bandwidthMhz;
    }

    const std::array<QRgb, 256> colorLut = waterfallHistoryColorLut();
    QVector<quint8> levels(destWidth, 0);
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
        const float normalized = kiwiSdrWaterfallLevel(level);
        levels[x] = encodeWaterfallLevel(normalized);
        scanline[x] = colorLut[levels[x]];
    }

    appendHistoryRow(levels.constData(), QDateTime::currentMSecsSinceEpoch(),
                     rowCenterMhz, rowBandwidthMhz);
    if (m_wfLive) {
        QVector<QRgb> visibleLine(destWidth, qRgb(0, 0, 0));
        remapHistoryRowInto(visibleLine.data(), levels.constData(), destWidth,
                            colorLut,
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

void SpectrumWidget::releaseWaterfallFramePipelineResources()
{
    delete m_wfFramePipeline;
    m_wfFramePipeline = nullptr;
    delete m_wfFrameSrb;
    m_wfFrameSrb = nullptr;
    delete m_wfFrameTex;
    m_wfFrameTex = nullptr;
    delete m_wfFrameSampler;
    m_wfFrameSampler = nullptr;
    m_wfFrameTexReady = false;
    m_wfFrameTexDirty = true;
    m_wfPipelineMode = WaterfallPipelineMode::Legacy;
    m_wfFrameUpload.clear();
}

bool SpectrumWidget::initWaterfallPipeline()
{
    QRhi* r = rhi();
    if (!r) {
        return false;
    }

    const int textureWidth = qMax(contentWidth(), 64);
    const int textureHeight = qMax(m_waterfall.height(), 64);
    std::unique_ptr<QRhiBuffer> vbo(r->newBuffer(
        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
        sizeof(kQuadData)));
    std::unique_ptr<QRhiBuffer> ubo(r->newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
    std::unique_ptr<QRhiTexture> colorTexture(r->newTexture(
        QRhiTexture::RGBA8, QSize(textureWidth, textureHeight)));
    std::unique_ptr<QRhiSampler> colorSampler(r->newSampler(
        QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
        QRhiSampler::ClampToEdge, QRhiSampler::Repeat));
    if (!vbo->create() || !ubo->create() || !colorTexture->create()
        || !colorSampler->create()) {
        qCWarning(lcGui)
            << "SpectrumWidget: legacy waterfall resource creation failed";
        return false;
    }

    std::unique_ptr<QRhiShaderResourceBindings> legacySrb(
        r->newShaderResourceBindings());
    legacySrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::FragmentStage, ubo.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage,
            colorTexture.get(), colorSampler.get()),
    });
    if (!legacySrb->create()) {
        qCWarning(lcGui)
            << "SpectrumWidget: legacy waterfall bindings creation failed";
        return false;
    }

    const QShader vertexShader = loadShader(
        ":/shaders/resources/shaders/texturedquad.vert.qsb");
    const QShader legacyFragmentShader = loadShader(
        ":/shaders/resources/shaders/texturedquad.frag.qsb");
    if (!vertexShader.isValid() || !legacyFragmentShader.isValid()) {
        qCWarning(lcGui) << "SpectrumWidget: legacy waterfall shader load failed";
        return false;
    }

    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });

    std::unique_ptr<QRhiGraphicsPipeline> legacyPipeline(
        r->newGraphicsPipeline());
    legacyPipeline->setShaderStages({
        {QRhiShaderStage::Vertex, vertexShader},
        {QRhiShaderStage::Fragment, legacyFragmentShader},
    });
    legacyPipeline->setVertexInputLayout(layout);
    legacyPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    legacyPipeline->setShaderResourceBindings(legacySrb.get());
    legacyPipeline->setRenderPassDescriptor(
        renderTarget()->renderPassDescriptor());
    if (!legacyPipeline->create()) {
        qCWarning(lcGui) << "SpectrumWidget: legacy waterfall pipeline failed";
        return false;
    }

    m_wfVbo = vbo.release();
    m_wfUbo = ubo.release();
    m_wfGpuTex = colorTexture.release();
    m_wfSampler = colorSampler.release();
    m_wfSrb = legacySrb.release();
    m_wfPipeline = legacyPipeline.release();
    m_wfGpuTexW = textureWidth;
    m_wfGpuTexH = textureHeight;
    m_wfPipelineMode = WaterfallPipelineMode::Legacy;
    m_wfPipelineFallbackReason.clear();

    // The legacy texture/pipeline above is committed first and never depends
    // on binding 2. The row-frequency path below is an optional transaction:
    // every resource must succeed before the renderer switches to it.
    WaterfallRowFrameReadiness readiness;
    readiness.requested = waterfallGpuFrequencyPreviewRequested();
    const auto useLegacyPipeline = [this, &readiness](
        const QString& reason, bool warning) {
        m_wfPipelineMode = chooseWaterfallPipeline(readiness);
        m_wfPipelineFallbackReason = reason;
        if (warning) {
            qCWarning(lcGui)
                << "SpectrumWidget: using legacy waterfall pipeline:" << reason;
        } else {
            qDebug() << "SpectrumWidget: using legacy waterfall pipeline:"
                     << reason;
        }
        return true;
    };
    if (!readiness.requested) {
        return useLegacyPipeline(
            QStringLiteral("disabled by AETHER_WF_GPU_PREVIEW"), false);
    }

    readiness.formatSupported = r->isTextureFormatSupported(
        QRhiTexture::RGBA32F, {});
    if (!readiness.formatSupported) {
        return useLegacyPipeline(
            QStringLiteral("RGBA32F row texture unsupported"), true);
    }

    std::unique_ptr<QRhiTexture> frameTexture(r->newTexture(
        QRhiTexture::RGBA32F, QSize(1, textureHeight)));
    readiness.textureCreated = !waterfallFrameTextureFailureForced()
        && frameTexture->create();
    if (!readiness.textureCreated) {
        const QString reason = waterfallFrameTextureFailureForced()
            ? QStringLiteral("row texture failure forced by automation")
            : QStringLiteral("RGBA32F row texture creation failed");
        return useLegacyPipeline(reason, true);
    }

    std::unique_ptr<QRhiSampler> frameSampler(r->newSampler(
        QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
        QRhiSampler::ClampToEdge, QRhiSampler::Repeat));
    readiness.samplerCreated = frameSampler->create();
    if (!readiness.samplerCreated) {
        return useLegacyPipeline(
            QStringLiteral("row texture sampler creation failed"), true);
    }

    std::unique_ptr<QRhiShaderResourceBindings> frameSrb(
        r->newShaderResourceBindings());
    frameSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::FragmentStage, m_wfUbo),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage,
            m_wfGpuTex, m_wfSampler),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage,
            frameTexture.get(), frameSampler.get()),
    });
    readiness.bindingsCreated = frameSrb->create();
    if (!readiness.bindingsCreated) {
        return useLegacyPipeline(
            QStringLiteral("row texture bindings creation failed"), true);
    }

    const QShader frameFragmentShader = loadShader(
        ":/shaders/resources/shaders/texturedquad_rowframes.frag.qsb");
    if (!frameFragmentShader.isValid()) {
        return useLegacyPipeline(
            QStringLiteral("row-frequency shader load failed"), true);
    }

    std::unique_ptr<QRhiGraphicsPipeline> framePipeline(
        r->newGraphicsPipeline());
    framePipeline->setShaderStages({
        {QRhiShaderStage::Vertex, vertexShader},
        {QRhiShaderStage::Fragment, frameFragmentShader},
    });
    framePipeline->setVertexInputLayout(layout);
    framePipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    framePipeline->setShaderResourceBindings(frameSrb.get());
    framePipeline->setRenderPassDescriptor(
        renderTarget()->renderPassDescriptor());
    readiness.pipelineCreated = framePipeline->create();
    m_wfPipelineMode = chooseWaterfallPipeline(readiness);
    if (m_wfPipelineMode != WaterfallPipelineMode::RowFrequencyFrames) {
        return useLegacyPipeline(
            QStringLiteral("row-frequency pipeline creation failed"), true);
    }

    m_wfFrameTex = frameTexture.release();
    m_wfFrameSampler = frameSampler.release();
    m_wfFrameSrb = frameSrb.release();
    m_wfFramePipeline = framePipeline.release();
    m_wfFrameTexReady = true;
    m_wfFrameTexDirty = true;
    qDebug() << "SpectrumWidget: waterfall row-frequency pipeline created"
             << m_wfGpuTexW << "x" << m_wfGpuTexH;
    return true;
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

    m_ovFrequencyGpuTex = r->newTexture(QRhiTexture::RGBA8, QSize(pw, ph));
    m_ovFrequencyGpuTex->create();
    m_ovFrequencySrb = r->newShaderResourceBindings();
    m_ovFrequencySrb->setBindings({
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage,
            m_ovFrequencyGpuTex, m_ovSampler),
    });
    m_ovFrequencySrb->create();

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

    // Frequency-preview overlay path. Unlike m_ovPipeline (also reused by the
    // screen-space, background, and cached 3D layers), this pipeline has a small
    // uniform block that remaps the frequency-overlay texture. It stays at the
    // drag-start frame while band/spot/slice geometry follows the live GPU data.
    m_ovPreviewUbo = r->newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 8 * sizeof(float));
    const bool previewUboReady = m_ovPreviewUbo->create();
    m_ovPreviewSrb = r->newShaderResourceBindings();
    m_ovPreviewSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::FragmentStage, m_ovPreviewUbo),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage,
            m_ovFrequencyGpuTex, m_ovSampler),
    });
    const bool previewSrbReady = m_ovPreviewSrb->create();
    const QShader previewFs = loadShader(
        ":/shaders/resources/shaders/overlay_frequency_preview.frag.qsb");
    if (previewUboReady && previewSrbReady && previewFs.isValid()) {
        m_ovPreviewPipeline = r->newGraphicsPipeline();
        m_ovPreviewPipeline->setShaderStages({
            {QRhiShaderStage::Vertex, vs},
            {QRhiShaderStage::Fragment, previewFs},
        });
        m_ovPreviewPipeline->setVertexInputLayout(layout);
        m_ovPreviewPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_ovPreviewPipeline->setShaderResourceBindings(m_ovPreviewSrb);
        m_ovPreviewPipeline->setRenderPassDescriptor(
            renderTarget()->renderPassDescriptor());
        m_ovPreviewPipeline->setTargetBlends({blend});
        if (!m_ovPreviewPipeline->create()) {
            delete m_ovPreviewPipeline;
            m_ovPreviewPipeline = nullptr;
        }
    }
    if (!m_ovPreviewPipeline) {
        qWarning() << "SpectrumWidget: frequency-preview overlay pipeline unavailable;"
                      " falling back to CPU overlay refreshes";
    }

    m_overlayStatic = QImage(pw, ph, QImage::Format_RGBA8888_Premultiplied);
    m_overlayStatic.setDevicePixelRatio(dpr);
    m_overlayFrequency = QImage(
        pw, ph, QImage::Format_RGBA8888_Premultiplied);
    m_overlayFrequency.setDevicePixelRatio(dpr);

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

    // 3DSS surface layer — parallel texture + SRB so the overlay pipeline can
    // paint the cached 3D image as a full-screen quad in 3D mode. The image is
    // built/uploaded on demand in renderGpuFrame().
    m_dssGpuTex = r->newTexture(QRhiTexture::RGBA8, QSize(pw, ph));
    m_dssGpuTex->create();
    m_dssTexW = pw;
    m_dssTexH = ph;
    m_dssSrb = r->newShaderResourceBindings();
    m_dssSrb->setBindings({
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_dssGpuTex, m_ovSampler),
    });
    m_dssSrb->create();
    m_dssTexNeedsUpload = true;
    m_dssLastUploadedGen = ~0ull;

    qDebug() << "SpectrumWidget: overlay pipeline created" << pw << "x" << ph << "dpr:" << dpr;
}

void SpectrumWidget::initSpectrumPipeline()
{
    QRhi* r = rhi();

    // Column texture is (re)created at the spectrum viewport width in
    // renderGpuFrame; only the fixed resources are built here. The column is
    // sampled with LINEAR filtering, so the format must be linearly
    // *filterable*, not merely creatable — isTextureFormatSupported() cannot
    // distinguish the two, and float32 filtering is optional on GLES (and on
    // some older GL drivers). R16F is filterable on every QRhi backend and
    // half precision is ample for the normalized 0..1 amplitude stored here
    // (same conclusion as PR #3968). Use it unconditionally.
    m_fftColFormat = QRhiTexture::R16F;
    m_fftScopeUbo = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                 6 * 4 * sizeof(float));  // 6 vec4 — see panscope.frag U block (#4239)
    m_fftScopeUbo->create();

    // LINEAR column sampling interpolates the trace between device pixel
    // columns exactly like the old per-point polyline did between vertices.
    m_fftColSampler = r->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                    QRhiSampler::None,
                                    QRhiSampler::ClampToEdge,
                                    QRhiSampler::ClampToEdge);
    m_fftColSampler->create();

    QShader vs = loadShader(":/shaders/resources/shaders/overlay.vert.qsb");
    QShader fs = loadShader(":/shaders/resources/shaders/panscope.frag.qsb");
    if (!vs.isValid() || !fs.isValid()) {
        qWarning() << "SpectrumWidget: panscope shader load failed";
        return;
    }

    // Same full-viewport quad layout as the overlay pipeline (reuses m_ovVbo).
    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},                   // position
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},   // texcoord
    });

    // panscope.frag emits premultiplied color.
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

    m_fftScopePipeline = r->newGraphicsPipeline();
    m_fftScopePipeline->setShaderStages({
        {QRhiShaderStage::Vertex, vs},
        {QRhiShaderStage::Fragment, fs},
    });
    m_fftScopePipeline->setVertexInputLayout(layout);
    m_fftScopePipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    // Placeholder-width column texture so the SRB is valid from the first
    // frame; renderGpuFrame recreates it at the real viewport width.
    m_fftColTexW = 16;
    m_fftColTex = r->newTexture(m_fftColFormat, QSize(m_fftColTexW, 1));
    m_fftColTex->create();
    m_fftScopeSrb = r->newShaderResourceBindings();
    m_fftScopeSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::FragmentStage, m_fftScopeUbo),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, m_fftColTex, m_fftColSampler),
    });
    m_fftScopeSrb->create();
    m_fftScopePipeline->setShaderResourceBindings(m_fftScopeSrb);
    m_fftScopePipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_fftScopePipeline->setTargetBlends({blend});
    m_fftScopePipeline->create();

    qDebug() << "SpectrumWidget: spectrum pipeline created (per-pixel columns)";
}

void SpectrumWidget::uploadDssPaletteLut(QRhiResourceUpdateBatch* batch,
                                         float floorDbm, float rangeDb)
{
    if (!m_dssPaletteTex || !batch) {
        return;
    }
    // The 3D surface maps its strength axis (noise floor -> ref level) across the
    // FULL colormap gradient, bypassing dbmToRgb()'s waterfall black-level window
    // (which forced everything below ~(min + (125-black)*0.4) dBm to black, so
    // only the strongest signals showed any colour). The "3D Color" control
    // gamma-shapes the strength before the lookup: higher = colour reaches down
    // toward the noise floor; lower = colour only on the strongest signals.
    // Colour depends only on the scheme + that control (NOT the per-frame floor/
    // range, which jitter every frame), so the LUT re-bakes only on a real change.
    Q_UNUSED(floorDbm);
    Q_UNUSED(rangeDb);
    const quint64 token = static_cast<quint64>(m_wfColorScheme) * 131
                        + static_cast<quint64>(m_dssGain);
    if (token == m_dssLutToken) {
        return;  // unchanged
    }

    QImage lut(256, 1, QImage::Format_RGBA8888);  // owns its data
    for (int i = 0; i < 256; ++i) {
        const QRgb c = dssStrengthToRgb(i / 255.0f);
        lut.setPixelColor(i, 0, QColor(qRed(c), qGreen(c), qBlue(c)));
    }
    QRhiTextureSubresourceUploadDescription desc(lut);
    batch->uploadTexture(m_dssPaletteTex, QRhiTextureUploadEntry(0, 0, desc));
    m_dssLutToken = token;
}

void SpectrumWidget::initDssMeshPipeline()
{
    QRhi* r = rhi();
    m_dssMeshReady = false;

    // R16F height texture is the cleanest dBm store; if unsupported, fall back
    // to the cached-image quad path (no mesh).
    if (!r->isTextureFormatSupported(QRhiTexture::R16F, {})) {
        qCWarning(lcGui) << "SpectrumWidget: R16F unsupported — stacked-trace mesh disabled (CPU fallback)";
        return;
    }

    QShader vs = loadShader(":/shaders/resources/shaders/dss_mesh.vert.qsb");
    QShader fs = loadShader(":/shaders/resources/shaders/dss_mesh.frag.qsb");
    if (!vs.isValid() || !fs.isValid()) {
        qCWarning(lcGui) << "SpectrumWidget: dss_mesh shader load failed — stacked-trace mesh disabled";
        return;
    }

    const int cols = m_dss.cols();
    const int rows = m_dss.rows();
    const int fillVerts = rows * dssFillVerticesPerRow();
    const int lineVerts = rows * dssLineVerticesPerRow();

    m_dssMeshVbo = r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                fillVerts * 3 * sizeof(float));
    m_dssMeshLineVbo = r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                    lineVerts * 3 * sizeof(float));
    m_dssMeshUbo = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                kDssMeshUboFloats * sizeof(float));
    if (!m_dssMeshVbo->create() || !m_dssMeshLineVbo->create() || !m_dssMeshUbo->create()) {
        qCWarning(lcGui) << "SpectrumWidget: dss_mesh buffer create failed";
        return;
    }

    m_dssHeightTex  = r->newTexture(QRhiTexture::R16F, QSize(cols, rows));
    m_dssPaletteTex = r->newTexture(QRhiTexture::RGBA8, QSize(256, 1));
    if (!m_dssHeightTex->create() || !m_dssPaletteTex->create()) {
        qCWarning(lcGui) << "SpectrumWidget: dss_mesh texture create failed";
        return;
    }

    // Height sampled in the vertex stage; Nearest is enough (the grid is as dense
    // as the texture). Palette is Linear for a smooth floor->peak gradient.
    m_dssHeightSampler = r->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
        QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_dssPaletteSampler = r->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
        QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    if (!m_dssHeightSampler->create() || !m_dssPaletteSampler->create()) {
        qCWarning(lcGui) << "SpectrumWidget: dss_mesh sampler create failed";
        return;
    }

    m_dssMeshSrb = r->newShaderResourceBindings();
    m_dssMeshSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0,
            QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            m_dssMeshUbo),
        QRhiShaderResourceBinding::sampledTexture(1,
            QRhiShaderResourceBinding::VertexStage, m_dssHeightTex, m_dssHeightSampler),
        QRhiShaderResourceBinding::sampledTexture(2,
            QRhiShaderResourceBinding::FragmentStage, m_dssPaletteTex, m_dssPaletteSampler),
    });
    if (!m_dssMeshSrb->create()) {
        qCWarning(lcGui) << "SpectrumWidget: dss_mesh SRB create failed";
        return;
    }

    QRhiVertexInputLayout layout;
    layout.setBindings({{3 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0}});  // u, v, edge

    // Fill pipeline — opaque triangle lists (occlusion via back-to-front order).
    m_dssMeshFillPipeline = r->newGraphicsPipeline();
    m_dssMeshFillPipeline->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    m_dssMeshFillPipeline->setVertexInputLayout(layout);
    m_dssMeshFillPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    m_dssMeshFillPipeline->setShaderResourceBindings(m_dssMeshSrb);
    m_dssMeshFillPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());

    // Outline pipeline — alpha-blended line segments (the dim trace line).
    QRhiGraphicsPipeline::TargetBlend lblend;
    lblend.enable = true;
    lblend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    lblend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    lblend.srcAlpha = QRhiGraphicsPipeline::One;
    lblend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    m_dssMeshLinePipeline = r->newGraphicsPipeline();
    m_dssMeshLinePipeline->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    m_dssMeshLinePipeline->setVertexInputLayout(layout);
    m_dssMeshLinePipeline->setTopology(QRhiGraphicsPipeline::Lines);
    m_dssMeshLinePipeline->setShaderResourceBindings(m_dssMeshSrb);
    m_dssMeshLinePipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_dssMeshLinePipeline->setTargetBlends({lblend});

    if (!m_dssMeshFillPipeline->create() || !m_dssMeshLinePipeline->create()) {
        qCWarning(lcGui) << "SpectrumWidget: dss_mesh pipeline create failed";
        return;
    }

    m_dssMeshHeadUploaded = -1;
    m_dssMeshRowGenUploaded = ~0ull;
    m_dssLutToken = ~0ull;
    m_dssMeshReady = true;
    qDebug() << "SpectrumWidget: stacked-trace mesh pipeline created" << cols << "x" << rows;
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

    if (!initWaterfallPipeline()) {
        return;
    }
    initOverlayPipeline();
    initSpectrumPipeline();
    initDssMeshPipeline();

    // Upload quad vertex data for both pipelines
    QRhiResourceUpdateBatch* batch = r->nextResourceUpdateBatch();

    // Upload VBO data
    batch->uploadStaticBuffer(m_wfVbo, kQuadData);
    batch->uploadStaticBuffer(m_ovVbo, kQuadData);

    // 3DSS mesh: build the static perspective grid once (geometry never changes —
    // height comes from the ring-buffered texture sampled per-vertex).
    if (m_dssMeshReady) {
        const int cols = m_dss.cols();
        const int rows = m_dss.rows();
        QVector<float> fill;
        QVector<float> line;
        fill.reserve(rows * dssFillVerticesPerRow() * 3);
        line.reserve(rows * dssLineVerticesPerRow() * 3);
        for (int rr = rows - 1; rr >= 0; --rr) {
            const float v = static_cast<float>(rr) / rows;   // 0 front .. ~1 back
            for (int cc = 0; cc + 1 < cols; ++cc) {
                const float u0 = static_cast<float>(cc) / (cols - 1);
                const float u1 = static_cast<float>(cc + 1) / (cols - 1);
                appendDssVertex(fill, u0, v, 0.0f);   // ridge
                appendDssVertex(fill, u0, v, 1.0f);   // floor
                appendDssVertex(fill, u1, v, 0.0f);   // next ridge
                appendDssVertex(fill, u1, v, 0.0f);
                appendDssVertex(fill, u0, v, 1.0f);
                appendDssVertex(fill, u1, v, 1.0f);   // next floor
                appendDssVertex(line, u0, v, -1.0f);
                appendDssVertex(line, u1, v, -1.0f);
            }
        }
        batch->uploadStaticBuffer(m_dssMeshVbo, fill.constData());
        batch->uploadStaticBuffer(m_dssMeshLineVbo, line.constData());
        // The palette LUT is baked from the live floor/range on the first 3D
        // frame (renderGpuFrame) before the surface is drawn — no init upload.
    }

    // Initial full waterfall texture upload (convert RGB32→RGBA8)
    if (!m_waterfall.isNull()) {
        QImage rgba = m_waterfall.convertToFormat(QImage::Format_RGBA8888);
        QRhiTextureSubresourceUploadDescription desc(rgba);
        batch->uploadTexture(m_wfGpuTex, QRhiTextureUploadEntry(0, 0, desc));
        if (m_wfPipelineMode == WaterfallPipelineMode::RowFrequencyFrames
            && m_wfFrameTexReady) {
            prepareWaterfallFrameUpload();
            QRhiTextureSubresourceUploadDescription frameDesc(
                m_wfFrameUpload.constData(),
                m_wfFrameUpload.size() * static_cast<int>(sizeof(float)));
            frameDesc.setSourceSize(QSize(1, m_wfGpuTexH));
            batch->uploadTexture(m_wfFrameTex,
                                 QRhiTextureUploadEntry(0, 0, frameDesc));
            m_wfFrameTexDirty = false;
        }
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
    m_overlayFrequencyNeedsUpload = true;

    // Re-apply cursor and mouse tracking now that the native surface exists.
    setCursor(cursor());
    setMouseTracking(true);
}

void SpectrumWidget::renderGpuFrame(QRhiCommandBuffer* cb,
                                    const QSize& logicalSize,
                                    bool resizePreview)
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
    const int w = logicalSize.width();
    const int h = logicalSize.height();
    if (w <= 0 || h <= freqScaleH() + DIVIDER_H + 2) return;
    const bool perfEnabled = PerfTelemetry::instance().enabled();
    const qint64 perfStartNs = perfEnabled ? PerfTelemetry::nowNs() : 0;

    // panstats: whole CPU-side frame prep + encode (RAII catches every exit).
    m_panStats.gpuFrames++;
    struct FrameCost {
        quint64& acc;
        QElapsedTimer t;
        explicit FrameCost(quint64& a) : acc(a) { t.start(); }
        ~FrameCost() { acc += static_cast<quint64>(t.nsecsElapsed() / 1000); }
    } panStatsFrameCost(m_panStats.gpuFrameUs);

    const int chromeH = freqScaleH() + DIVIDER_H;
    const int contentH = h - chromeH;
    const int specH = static_cast<int>(contentH * m_spectrumFrac);
    const int wfY = specH + DIVIDER_H + freqScaleH();
    const int wfH = h - wfY;
    const QRect specRect(0, 0, w, specH);
    const QRect wfRect(0, wfY, w, wfH);
    // The FFT trace, 3DSS surface, and waterfall render into the frequency
    // canvas only (width minus the right dBm / time strip) so they end at the
    // tape, matching the contentWidth() mapping used by every marker (#3482).
    const int specContentW = std::max(1, specRect.width() - DBM_STRIP_W);
    const int wfContentW   = std::max(1, wfRect.width() - DBM_STRIP_W);
    int fftTracePointCount = 0;

    // 3DSS replaces only the spectrum trace: the surface fills specRect and the
    // waterfall, divider, freq scale, and all overlays keep their normal 2D
    // positions. Everything below is identical to 2D except the FFT trace is
    // swapped for the 3DSS surface quad inside specRect.
    const bool is3D = (m_spectrumRenderMode == SpectrumRenderMode::Mode3D);
    const float dssFrameFloorDbm = is3D
        ? dssFloorDbm()
        : m_lastDetectDssFloor;

    // Detect display state changes that may bypass markOverlayDirty()
    {
        // In 3D the dBm scale is anchored to the noise floor, which drifts every
        // FFT frame (dssFloorDbm() reads the measured floor, quantised to 0.5 dB).
        // The surface UBO and cached scale labels consume the same per-frame
        // floor, so a changed anchor invalidates both together (#3937). In 2D the
        // scale is Ref-anchored, so the floor is left out of the check there.
        const float dssFloor = dssFrameFloorDbm;
        if (m_centerMhz != m_lastDetectCenter || m_bandwidthMhz != m_lastDetectBw ||
            m_refLevel != m_lastDetectRef || m_dynamicRange != m_lastDetectDyn ||
            m_spectrumFrac != m_lastDetectFrac ||
            m_wnbActive != m_lastDetectWnb ||
            m_wnbUpdating != m_lastDetectWnbUpdating ||
            m_rfGainValue != m_lastDetectRfGain ||
            m_wideActive != m_lastDetectWide ||
            dssFloor != m_lastDetectDssFloor) {
            markOverlayDirty("detect");
            m_lastDetectCenter = m_centerMhz; m_lastDetectBw = m_bandwidthMhz;
            m_lastDetectRef = m_refLevel; m_lastDetectDyn = m_dynamicRange;
            m_lastDetectFrac = m_spectrumFrac;
            m_lastDetectWnb = m_wnbActive;
            m_lastDetectWnbUpdating = m_wnbUpdating;
            m_lastDetectRfGain = m_rfGainValue;
            m_lastDetectWide = m_wideActive;
            m_lastDetectDssFloor = dssFloor;
        }
    }

    auto* batch = r->nextResourceUpdateBatch();

    // Upload waterfall texture — full or incremental
    if (!m_waterfall.isNull()) {
        // Resize texture if needed — full re-upload
        if (m_waterfall.width() != m_wfGpuTexW || m_waterfall.height() != m_wfGpuTexH) {
            const int desiredWidth = m_waterfall.width();
            const int desiredHeight = m_waterfall.height();
            std::unique_ptr<QRhiTexture> colorTexture(r->newTexture(
                QRhiTexture::RGBA8, QSize(desiredWidth, desiredHeight)));
            std::unique_ptr<QRhiShaderResourceBindings> legacySrb(
                r->newShaderResourceBindings());
            bool legacyResizeReady = colorTexture->create();
            if (legacyResizeReady) {
                legacySrb->setBindings({
                    QRhiShaderResourceBinding::uniformBuffer(
                        0, QRhiShaderResourceBinding::FragmentStage, m_wfUbo),
                    QRhiShaderResourceBinding::sampledTexture(
                        1, QRhiShaderResourceBinding::FragmentStage,
                        colorTexture.get(), m_wfSampler),
                });
                legacyResizeReady = legacySrb->create();
            }

            const bool rowPipelineWasActive =
                m_wfPipelineMode == WaterfallPipelineMode::RowFrequencyFrames
                && m_wfFrameTexReady;
            std::unique_ptr<QRhiTexture> frameTexture;
            std::unique_ptr<QRhiShaderResourceBindings> frameSrb;
            bool frameResizeReady = !rowPipelineWasActive;
            if (legacyResizeReady && rowPipelineWasActive) {
                frameTexture.reset(r->newTexture(
                    QRhiTexture::RGBA32F, QSize(1, desiredHeight)));
                frameResizeReady = !waterfallFrameResizeFailureForced()
                    && frameTexture->create();
                if (frameResizeReady) {
                    frameSrb.reset(r->newShaderResourceBindings());
                    frameSrb->setBindings({
                        QRhiShaderResourceBinding::uniformBuffer(
                            0, QRhiShaderResourceBinding::FragmentStage,
                            m_wfUbo),
                        QRhiShaderResourceBinding::sampledTexture(
                            1, QRhiShaderResourceBinding::FragmentStage,
                            colorTexture.get(), m_wfSampler),
                        QRhiShaderResourceBinding::sampledTexture(
                            2, QRhiShaderResourceBinding::FragmentStage,
                            frameTexture.get(), m_wfFrameSampler),
                    });
                    frameResizeReady = frameSrb->create();
                }
            }

            if (!legacyResizeReady) {
                qCWarning(lcGui)
                    << "SpectrumWidget: waterfall resize transaction failed;"
                       " retaining the previous legacy texture";
            } else {
                QRhiTexture* oldColorTexture = m_wfGpuTex;
                QRhiShaderResourceBindings* oldLegacySrb = m_wfSrb;
                QRhiTexture* oldFrameTexture = m_wfFrameTex;
                QRhiShaderResourceBindings* oldFrameSrb = m_wfFrameSrb;
                m_wfGpuTex = colorTexture.release();
                m_wfSrb = legacySrb.release();
                m_wfGpuTexW = desiredWidth;
                m_wfGpuTexH = desiredHeight;
                m_wfTexFullUpload = true;

                if (rowPipelineWasActive && frameResizeReady) {
                    m_wfFrameTex = frameTexture.release();
                    m_wfFrameSrb = frameSrb.release();
                    delete oldFrameSrb;
                    delete oldFrameTexture;
                    m_wfFrameTexDirty = true;
                } else if (rowPipelineWasActive) {
                    releaseWaterfallFramePipelineResources();
                    m_wfPipelineFallbackReason =
                        QStringLiteral("row texture resize transaction failed");
                    qCWarning(lcGui)
                        << "SpectrumWidget: using legacy waterfall pipeline:"
                        << m_wfPipelineFallbackReason;
                }
                delete oldLegacySrb;
                delete oldColorTexture;
            }
        }

        const bool waterfallTextureMatchesSource =
            m_waterfall.width() == m_wfGpuTexW
            && m_waterfall.height() == m_wfGpuTexH;
        if (m_wfTexFullUpload && waterfallTextureMatchesSource) {
            // Full upload (init or resize)
            QImage rgba = m_waterfall.convertToFormat(QImage::Format_RGBA8888);
            QRhiTextureSubresourceUploadDescription desc(rgba);
            batch->uploadTexture(m_wfGpuTex, QRhiTextureUploadEntry(0, 0, desc));
            m_panStats.wfUploadBytes += static_cast<quint64>(rgba.sizeInBytes());
            if (perfEnabled)
                PerfTelemetry::instance().recordGpuUpload(PerfTelemetry::GpuUploadKind::WaterfallFull);
            m_wfLastUploadedRow = m_wfWriteRow;
            m_wfTexFullUpload = false;
        } else if (waterfallTextureMatchesSource
                   && m_wfWriteRow != m_wfLastUploadedRow) {
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
                m_panStats.wfUploadBytes +=
                    static_cast<quint64>(entries.size()) * m_wfGpuTexW * 4;
                if (perfEnabled) {
                    PerfTelemetry::instance().recordGpuUpload(
                        PerfTelemetry::GpuUploadKind::WaterfallIncremental);
                }
            }
            m_wfLastUploadedRow = m_wfWriteRow;
        }

        const double maxReferenceDriftMhz = std::max(
            1.0, std::abs(m_bandwidthMhz) * 8.0);
        if (!std::isfinite(m_wfFrameReferenceCenterMhz)
            || std::abs(m_centerMhz - m_wfFrameReferenceCenterMhz)
                > maxReferenceDriftMhz) {
            m_wfFrameTexDirty = true;
        }
        if (waterfallTextureMatchesSource
            && m_wfPipelineMode == WaterfallPipelineMode::RowFrequencyFrames
            && m_wfFrameTexReady && m_wfFrameTexDirty) {
            prepareWaterfallFrameUpload();
            QRhiTextureSubresourceUploadDescription frameDesc(
                m_wfFrameUpload.constData(),
                m_wfFrameUpload.size() * static_cast<int>(sizeof(float)));
            frameDesc.setSourceSize(QSize(1, m_wfGpuTexH));
            batch->uploadTexture(m_wfFrameTex,
                                 QRhiTextureUploadEntry(0, 0, frameDesc));
            m_panStats.wfUploadBytes += static_cast<quint64>(
                m_wfFrameUpload.size() * static_cast<int>(sizeof(float)));
            m_wfFrameTexDirty = false;
        }
    }

    // Update waterfall uniforms. Each row carries its own source frame, so new
    // radio rows can expose frequencies that were outside the drag-start view.
    float rowOffset = (m_wfGpuTexH > 0)
        ? static_cast<float>(m_wfWriteRow) / m_wfGpuTexH
        : 0.0f;
    const float waterfallTargetCenterOffsetMhz = static_cast<float>(
        m_centerMhz - m_wfFrameReferenceCenterMhz);
    const float waterfallTargetBandwidthMhz = static_cast<float>(m_bandwidthMhz);
    const float waterfallRowFrequencyFrames =
        m_wfPipelineMode == WaterfallPipelineMode::RowFrequencyFrames
            && m_wfFrameTexReady ? 1.0f : 0.0f;
    float uniforms[] = {
        rowOffset,
        waterfallTargetCenterOffsetMhz,
        waterfallTargetBandwidthMhz,
        waterfallRowFrequencyFrames,
    };
    batch->updateDynamicBuffer(m_wfUbo, 0, sizeof(uniforms), uniforms);

    // The compact 3DSS height surface still shares one preview frame across its
    // rows, so it retains the original affine preview uniforms.
    float frequencyScale = 1.0f;
    float frequencyOffset = 0.0f;
    float frequencyPreview = 0.0f;
    const FrequencyPreviewTransform previewTransform =
        frequencyPreviewTransform(
            FrequencyFrame{m_frequencyPreviewBaseCenterMhz,
                           m_frequencyPreviewBaseBandwidthMhz},
            FrequencyFrame{m_frequencyPreviewTargetCenterMhz,
                           m_frequencyPreviewTargetBandwidthMhz});
    if (m_frequencyPreviewActive && previewTransform.valid) {
        frequencyScale = static_cast<float>(previewTransform.scale);
        frequencyOffset = static_cast<float>(previewTransform.offset);
        frequencyPreview = 1.0f;
        ++m_frequencyPreviewPresentCount;
    }

    if (m_ovPreviewPipeline && m_ovPreviewSrb && m_ovPreviewUbo) {
        float overlayScale = 1.0f;
        float overlayOffset = 0.0f;
        float overlayPreview = 0.0f;
        const FrequencyPreviewTransform overlayTransform =
            frequencyPreviewTransform(
                FrequencyFrame{m_frequencyPreviewOverlayBaseCenterMhz,
                               m_frequencyPreviewOverlayBaseBandwidthMhz},
                FrequencyFrame{m_frequencyPreviewTargetCenterMhz,
                               m_frequencyPreviewTargetBandwidthMhz});
        if (m_frequencyPreviewActive && overlayTransform.valid) {
            overlayScale = static_cast<float>(overlayTransform.scale);
            overlayOffset = static_cast<float>(overlayTransform.offset);
            overlayPreview = 1.0f;
            ++m_frequencyPreviewOverlayTransformCount;
            ++m_panStats.previewOverlayTransforms;
        }

        const float logicalW = static_cast<float>(std::max(1, w));
        const float logicalH = static_cast<float>(std::max(1, h));
        const float previewUniforms[8] = {
            overlayScale,
            overlayOffset,
            overlayPreview,
            static_cast<float>(specContentW) / logicalW,
            static_cast<float>(specH) / logicalH,
            static_cast<float>(wfY) / logicalH,
            static_cast<float>(wfContentW) / logicalW,
            0.0f,
        };
        batch->updateDynamicBuffer(
            m_ovPreviewUbo, 0, sizeof(previewUniforms), previewUniforms);
    }

    // Render overlays — split into static (on state change) and dynamic (every frame)
    {
        // Resize overlay images if needed
        const qreal dpr = devicePixelRatioF();
        const int pw = static_cast<int>(w * dpr);
        const int ph = static_cast<int>(h * dpr);
        if (!resizePreview && m_overlayStatic.size() != QSize(pw, ph)) {
            m_overlayStatic = QImage(pw, ph, QImage::Format_RGBA8888_Premultiplied);
            m_overlayStatic.setDevicePixelRatio(dpr);
            m_overlayFrequency = QImage(
                pw, ph, QImage::Format_RGBA8888_Premultiplied);
            m_overlayFrequency.setDevicePixelRatio(dpr);
            m_ovGpuTex->setPixelSize(QSize(pw, ph));
            m_ovGpuTex->create();
            m_ovSrb->setBindings({
                QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_ovGpuTex, m_ovSampler),
            });
            m_ovSrb->create();
            m_ovFrequencyGpuTex->setPixelSize(QSize(pw, ph));
            m_ovFrequencyGpuTex->create();
            m_ovFrequencySrb->setBindings({
                QRhiShaderResourceBinding::sampledTexture(
                    1, QRhiShaderResourceBinding::FragmentStage,
                    m_ovFrequencyGpuTex, m_ovSampler),
            });
            m_ovFrequencySrb->create();
            if (m_ovPreviewSrb && m_ovPreviewUbo) {
                m_ovPreviewSrb->setBindings({
                    QRhiShaderResourceBinding::uniformBuffer(
                        0, QRhiShaderResourceBinding::FragmentStage,
                        m_ovPreviewUbo),
                    QRhiShaderResourceBinding::sampledTexture(
                        1, QRhiShaderResourceBinding::FragmentStage,
                        m_ovFrequencyGpuTex, m_ovSampler),
                });
                m_ovPreviewSrb->create();
            }
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
            // #3617 flag sprites are flag-sized (not panadapter-sized), so a
            // panadapter resize doesn't touch them — their quads just reposition.
            m_overlayStaticDirty = true;
        }

        // panstats: time the bg + static QPainter repaints as one rebuild.
        const bool rebuildStaticOverlay = m_overlayStaticDirty && !resizePreview;
        const bool panStatsOvRebuild = rebuildStaticOverlay;
        QElapsedTimer panStatsOvTimer;
        if (panStatsOvRebuild)
            panStatsOvTimer.start();

        // Keep the center frequency scale exact during the GPU preview without
        // repainting either full-pan overlay. This strip is typically only a
        // few dozen pixels tall; upload it directly into the screen-space
        // texture at the divider's destination row.
        const bool refreshFrequencyScalePreview =
            m_frequencyPreviewActive
            && m_frequencyScalePreviewNeedsUpload
            && !rebuildStaticOverlay
            && !resizePreview;
        if (refreshFrequencyScalePreview) {
            QElapsedTimer previewScaleTimer;
            previewScaleTimer.start();
            const int scaleBandH = DIVIDER_H + freqScaleH();
            const QSize scalePixelSize(
                pw, static_cast<int>(scaleBandH * dpr));
            if (m_frequencyScalePreviewImage.size() != scalePixelSize) {
                m_frequencyScalePreviewImage = QImage(
                    scalePixelSize,
                    QImage::Format_RGBA8888_Premultiplied);
                m_frequencyScalePreviewImage.setDevicePixelRatio(dpr);
            }
            m_frequencyScalePreviewImage.fill(Qt::transparent);
            {
                QPainter scalePainter(&m_frequencyScalePreviewImage);
                scalePainter.setRenderHint(QPainter::Antialiasing, false);
                scalePainter.fillRect(
                    0, 0, w, DIVIDER_H,
                    AetherSDR::ThemeManager::instance().color(
                        "color.background.2"));
                drawFreqScale(
                    scalePainter,
                    QRect(0, DIVIDER_H, w, freqScaleH()));
            }

            m_panStats.previewScalePaintUs += static_cast<quint64>(
                previewScaleTimer.nsecsElapsed() / 1000);
            ++m_panStats.previewScaleRefreshes;
            const quint64 previewBytes = static_cast<quint64>(
                m_frequencyScalePreviewImage.sizeInBytes());
            m_panStats.previewScaleUploadBytes += previewBytes;
            m_panStats.overlayUploadBytes += previewBytes;

            QRhiTextureSubresourceUploadDescription scaleDesc(
                m_frequencyScalePreviewImage);
            scaleDesc.setDestinationTopLeft(
                QPoint(0, static_cast<int>(specH * dpr)));
            batch->uploadTexture(
                m_ovGpuTex,
                QRhiTextureUploadEntry(0, 0, scaleDesc));
            if (perfEnabled) {
                PerfTelemetry::instance().recordGpuUpload(
                    PerfTelemetry::GpuUploadKind::Overlay);
            }
            m_frequencyScalePreviewNeedsUpload = false;
        }

        // Background-image layer — kept separate from the static overlay so
        // it can render BELOW the FFT trace (parity with software paint).
        // Rebuilt whenever the static overlay is rebuilt, since markOverlayDirty
        // also fires when m_bgImage, m_bgOpacity, or m_bgFillColor change.
        //
        // Composition (bottom → top):
        //     m_bgFillColor (full opacity)
        //     m_bgImage     (opacity = 1 - m_bgOpacity/100, lets fill bleed through)
        if (rebuildStaticOverlay) {
            m_overlayBg.fill(Qt::transparent);
            QPainter bp(&m_overlayBg);
            bp.setRenderHint(QPainter::Antialiasing, false);
            bp.fillRect(specRect, m_bgFillColor);
            if (!m_bgImage.isNull()) {
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

        // Static overlays: only repaint when state changes (markOverlayDirty).
        // Frequency-anchored elements live in their own texture so the preview
        // shader can move them without also distorting screen-space controls.
        if (rebuildStaticOverlay) {
            m_overlayFrequency.fill(Qt::transparent);
            QPainter frequencyPainter(&m_overlayFrequency);
            frequencyPainter.setRenderHint(QPainter::Antialiasing, false);

            if (m_bandPlanFontSize > 0) {
                drawBandPlan(frequencyPainter, specRect);
            }
            drawTnfMarkers(frequencyPainter, specRect);
            if (m_showSpots || m_showSHistory) {
                drawSpotMarkers(frequencyPainter, specRect);
            }
            drawSwrSweep(frequencyPainter, specRect);
            drawSliceMarkers(frequencyPainter, specRect, wfRect);
            drawOffScreenSlices(frequencyPainter, specRect);

            m_overlayStatic.fill(Qt::transparent);
            QPainter p(&m_overlayStatic);
            p.setRenderHint(QPainter::Antialiasing, false);

            // Divider bar
            p.fillRect(0, specH, w, DIVIDER_H, AetherSDR::ThemeManager::instance().color("color.background.2"));
            drawFreqScale(p, QRect(0, specH + DIVIDER_H, w, freqScaleH()));
            drawSmartMtrValueLabels(p);

            // The auto-SQL floor and squelch line are anchored to the 2D
            // dynamic-range y-axis, which doesn't map onto the 3D surface — only
            // the (frequency) x-axis carries over there. Suppress them in 3D.
            if (!is3D) {
                drawAutoSqlFloor(p, specRect);
                drawSquelchLine(p, specRect);
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
                // Flip at the content edge — the strip is opaque now (#3482).
                if (lx + tw > w - DBM_STRIP_W) lx = m_cursorPos.x() - tw - 4;
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
                if (lx + tw > w - DBM_STRIP_W) { lx = cx - tw - 4; }  // content edge (#3482)
                int ly = m_cursorPos.y() - th - 4;
                if (ly < 0) { ly = m_cursorPos.y() + 16; }
                p.fillRect(lx, ly, tw, th, AetherSDR::theme::withAlpha("color.background.0", 200));
                p.setPen(AetherSDR::ThemeManager::instance().color("color.text.primary"));
                p.drawText(lx + 4, ly + fm.ascent() + 2, label);
            }

            drawConnectionAnimation(p, specRect);
            // dBm strip: both render modes use a readable full-height amplitude
            // reference; the 3D surface itself is perspective-foreshortened.
            if (is3D) {
                drawDbmScale3D(p, specRect, dssFrameFloorDbm);
            } else {
                drawDbmScale(p, specRect);
            }
            drawTimeScale(p, wfRect);

            m_overlayStaticDirty = false;
            m_overlayNeedsUpload = true;
            m_overlayFrequencyNeedsUpload = true;
            m_frequencyScalePreviewNeedsUpload = false;
        }

        if (panStatsOvRebuild) {
            m_panStats.overlayRebuilds++;
            m_panStats.overlayRebuildUs +=
                static_cast<quint64>(panStatsOvTimer.nsecsElapsed() / 1000);
        }

        // Upload overlay texture only when content changed
        if (m_overlayNeedsUpload && !resizePreview) {
            QRhiTextureSubresourceUploadDescription ovDesc(m_overlayStatic);
            batch->uploadTexture(m_ovGpuTex, QRhiTextureUploadEntry(0, 0, ovDesc));
            m_panStats.overlayUploadBytes += static_cast<quint64>(m_overlayStatic.sizeInBytes());
            if (perfEnabled)
                PerfTelemetry::instance().recordGpuUpload(PerfTelemetry::GpuUploadKind::Overlay);
            m_overlayNeedsUpload = false;
        }
        if (m_overlayFrequencyNeedsUpload && !resizePreview) {
            QRhiTextureSubresourceUploadDescription frequencyDesc(
                m_overlayFrequency);
            batch->uploadTexture(
                m_ovFrequencyGpuTex,
                QRhiTextureUploadEntry(0, 0, frequencyDesc));
            m_panStats.overlayUploadBytes += static_cast<quint64>(
                m_overlayFrequency.sizeInBytes());
            if (perfEnabled) {
                PerfTelemetry::instance().recordGpuUpload(
                    PerfTelemetry::GpuUploadKind::Overlay);
            }
            m_overlayFrequencyNeedsUpload = false;
        }
        if (m_overlayBgNeedsUpload && !resizePreview) {
            QRhiTextureSubresourceUploadDescription bgDesc(m_overlayBg);
            batch->uploadTexture(m_bgGpuTex, QRhiTextureUploadEntry(0, 0, bgDesc));
            m_panStats.overlayUploadBytes += static_cast<quint64>(m_overlayBg.sizeInBytes());
            if (perfEnabled)
                PerfTelemetry::instance().recordGpuUpload(PerfTelemetry::GpuUploadKind::Overlay);
            m_overlayBgNeedsUpload = false;
        }

        // 3DSS surface texture — 3D mode only, rebuilt/uploaded only when the
        // cached surface actually changed (generation bump). Rendered at a
        // CAPPED resolution (the surface is intrinsically 56x256, so a smaller
        // texture is visually free via the Linear overlay sampler) and stretched
        // to the specRect viewport. This keeps the per-frame QPainter rebuild and
        // texture upload an order of magnitude cheaper than a full-widget image.
        if (is3D && m_dssMeshReady) {
            // GPU mesh path: keep the palette LUT current, upload every height
            // row pushed since the last frame into the ring texture, and refresh
            // the uniforms. Geometry is static — pan/zoom rebuild nothing.
            const float floorDbm = dssFrameFloorDbm;
            const float rangeDb  = std::round(dssSpanDb() * 2.0f) / 2.0f;
            uploadDssPaletteLut(batch, floorDbm, rangeDb);

            const int cols = m_dss.cols();
            const int rows = m_dss.rows();
            const int head = m_dss.headRing();
            const int valid = m_dss.rowCount();
            const quint64 rowGeneration = m_dss.rowGeneration();
            // Catch-up upload: pushRow runs per FFT block (often faster than
            // vsync), so the ring head can advance by >1 between frames. Small
            // catch-ups upload only changed ring slots; large catch-ups refresh the
            // full height texture in one entry to avoid many per-row uploads right
            // when the GUI is already behind.
            if (valid > 0 && rowGeneration != m_dssMeshRowGenUploaded) {
                static_assert(DssRenderer::kCols <= 65536,
                              "qfloat16 row fits a texture width");
                const quint64 rowsSinceUpload =
                    (m_dssMeshRowGenUploaded == ~0ull
                     || rowGeneration < m_dssMeshRowGenUploaded)
                    ? static_cast<quint64>(valid)
                    : rowGeneration - m_dssMeshRowGenUploaded;
                int newRows = (m_dssMeshHeadUploaded < 0
                               || rowsSinceUpload >= static_cast<quint64>(rows))
                    ? valid
                    : static_cast<int>(rowsSinceUpload);
                newRows = std::clamp(newRows, 0, valid);
                if (newRows > kDssMaxIncrementalUploadRows) {
                    const int rowBytes = cols * int(sizeof(qfloat16));
                    m_dssTextureScratch.resize(rows * rowBytes);
                    char* textureData = m_dssTextureScratch.data();
                    for (int ring = 0; ring < rows; ++ring) {
                        qfloat16* dst = reinterpret_cast<qfloat16*>(textureData + ring * rowBytes);
                        const float* srcRow = m_dss.rowDataRing(ring);
                        for (int c = 0; c < cols; ++c) {
                            dst[c] = qfloat16(srcRow[c]);
                        }
                    }
                    QRhiTextureSubresourceUploadDescription fullDesc;
                    fullDesc.setData(m_dssTextureScratch);
                    fullDesc.setSourceSize(QSize(cols, rows));
                    batch->uploadTexture(m_dssHeightTex, QRhiTextureUploadEntry(0, 0, fullDesc));
                    if (perfEnabled) {
                        PerfTelemetry::instance().recordGpuUpload(
                            PerfTelemetry::GpuUploadKind::Overlay);
                    }
                } else {
                    QVarLengthArray<QRhiTextureUploadEntry, kDssMaxIncrementalUploadRows> entries;
                    m_dssRowScratch.resize(cols * int(sizeof(qfloat16)));
                    for (int n = 0; n < newRows; ++n) {
                        const int ring = (head + n) % rows;  // head..head+newRows-1
                        const float* srcRow = m_dss.rowDataRing(ring);
                        qfloat16* dst = reinterpret_cast<qfloat16*>(m_dssRowScratch.data());
                        for (int c = 0; c < cols; ++c) {
                            dst[c] = qfloat16(srcRow[c]);
                        }
                        QRhiTextureSubresourceUploadDescription rowDesc;
                        rowDesc.setData(m_dssRowScratch);  // descriptor keeps the bytes alive
                        rowDesc.setSourceSize(QSize(cols, 1));
                        rowDesc.setDestinationTopLeft(QPoint(0, ring));
                        entries.append(QRhiTextureUploadEntry(0, 0, rowDesc));
                    }
                    if (!entries.isEmpty()) {
                        QRhiTextureUploadDescription desc;
                        desc.setEntries(entries.cbegin(), entries.cend());
                        batch->uploadTexture(m_dssHeightTex, desc);
                        if (perfEnabled) {
                            PerfTelemetry::instance().recordGpuUpload(
                                PerfTelemetry::GpuUploadKind::Overlay);
                        }
                    }
                }
                m_dssMeshHeadUploaded = head;
                m_dssMeshRowGenUploaded = rowGeneration;
            }

            // rowOffset carries a half-texel so the shader (texY = fract(rowOffset
            // + v), v = rr/rows) samples row (head+rr) CENTRES, not boundaries —
            // avoids Nearest off-by-one. Column centring uses texCols in the shader.
            const float rowOffset = rows > 0
                ? (static_cast<float>(head) + 0.5f) / static_cast<float>(rows)
                : 0.0f;
            float ubo[kDssMeshUboFloats] = {
                rowOffset,
                floorDbm, rangeDb, m_dssZCurve,
                DssRenderer::kBackWidthFrac, DssRenderer::kDepthSpanFrac,
                DssRenderer::kFrontMaxRidgeFrac, DssRenderer::kHaze,
                static_cast<float>(cols),
                frequencyScale, frequencyOffset, frequencyPreview,
                static_cast<float>(m_bgFillColor.redF()),
                static_cast<float>(m_bgFillColor.greenF()),
                static_cast<float>(m_bgFillColor.blueF()),
                1.0f,                                         // bgFill vec4
            };
            batch->updateDynamicBuffer(m_dssMeshUbo, 0, sizeof(ubo), ubo);
        } else if (is3D) {
            // CPU cached-image fallback (no mesh pipeline). Rendered at a capped
            // resolution and stretched to the specRect viewport.
            const float fbDpr = static_cast<float>(renderTarget()->pixelSize().width())
                              / static_cast<float>(qMax(1, w));
            const int specPwDev = qMax(1, qRound(specContentW * fbDpr));
            const int specPhDev = qMax(1, qRound(specH * fbDpr));
            const double sc = qMin(1.0, qMin(double(kDssMaxW) / specPwDev,
                                             double(kDssMaxH) / specPhDev));
            const int dssW = qMax(2, static_cast<int>(specPwDev * sc));
            const int dssH = qMax(2, static_cast<int>(specPhDev * sc));
            const QImage& surf = buildDssImage(QSize(dssW, dssH), 0,
                                               dssFrameFloorDbm);
            // m_dssGpuTex/m_dssSrb/m_ovSampler come from initOverlayPipeline();
            // guard against a partial GPU init (OOM / device loss) so the
            // fallback never dereferences a null resource.
            if (!surf.isNull() && m_dssGpuTex && m_dssSrb && m_ovSampler) {
                if (m_dssTexW != dssW || m_dssTexH != dssH) {
                    m_dssTexW = dssW;
                    m_dssTexH = dssH;
                    m_dssGpuTex->setPixelSize(QSize(dssW, dssH));
                    m_dssGpuTex->create();
                    m_dssSrb->setBindings({
                        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_dssGpuTex, m_ovSampler),
                    });
                    m_dssSrb->create();
                    m_dssTexNeedsUpload = true;
                }
                if (m_dssTexNeedsUpload
                    || m_dss.generation() != m_dssLastUploadedGen) {
                    QRhiTextureSubresourceUploadDescription dssDesc(surf);
                    batch->uploadTexture(m_dssGpuTex, QRhiTextureUploadEntry(0, 0, dssDesc));
                    if (perfEnabled)
                        PerfTelemetry::instance().recordGpuUpload(PerfTelemetry::GpuUploadKind::Overlay);
                    m_dssLastUploadedGen = m_dss.generation();
                    m_dssTexNeedsUpload = false;
                }
            }
        }

        // Position the flags for THIS frame so a dragged flag's sprite is drawn at
        // the current marker position (no one-frame lag).  (Live-flag selection
        // runs OUTSIDE the render callback — from the refresh timer / mouse move —
        // because it shows/raises widgets, which must not happen mid-render.)
        repositionVfoFlags(specRect);

        // FFT trace columns. 2D only — in 3D the surface replaces the trace.
        // The display trace (spatially smoothed + Catmull-Rom resampled by
        // buildFftDisplayTrace) is sampled at one point per DEVICE pixel
        // column, normalized to the dynamic range, and uploaded as a width×1
        // R32F texture; panscope.frag evaluates fill + feather + core stroke
        // per pixel. This replaces the per-frame feather/core/fill vertex
        // bake and its ~1.4 MB/frame VBO uploads.
        QElapsedTimer panStatsFftTimer;
        panStatsFftTimer.start();
        if (!is3D && m_fftScopePipeline && m_fftColTex) {
            const float fbDpr = static_cast<float>(renderTarget()->pixelSize().width())
                              / static_cast<float>(qMax(1, w));
            const int cols = std::clamp(
                static_cast<int>(std::lround(specContentW * fbDpr)),
                2, kMaxFftDisplayTracePoints);
            const QVector<float>& trace =
                buildFftDisplayTrace(displaySpectrumBins(), cols);
            const int n = trace.size();
            if (n >= 2) {
                fftTracePointCount = n;
                if (m_fftColTexW != n) {
                    m_fftColTexW = n;
                    m_fftColTex->setPixelSize(QSize(n, 1));
                    m_fftColTex->create();
                    m_fftScopeSrb->setBindings({
                        QRhiShaderResourceBinding::uniformBuffer(
                            0, QRhiShaderResourceBinding::FragmentStage, m_fftScopeUbo),
                        QRhiShaderResourceBinding::sampledTexture(
                            1, QRhiShaderResourceBinding::FragmentStage,
                            m_fftColTex, m_fftColSampler),
                    });
                    m_fftScopeSrb->create();
                }

                const float minDbm = m_refLevel - m_dynamicRange;
                const float range = qMax(1e-3f, m_dynamicRange);
                if (m_fftColFormat == QRhiTexture::R32F) {
                    m_fftColScratch.resize(n * int(sizeof(float)));
                    float* colData = reinterpret_cast<float*>(m_fftColScratch.data());
                    for (int i = 0; i < n; ++i) {
                        colData[i] = qBound(0.0f, (trace[i] - minDbm) / range, 1.0f);
                    }
                } else {  // R16F fallback (GL without float32 filtering)
                    m_fftColScratch.resize(n * int(sizeof(qfloat16)));
                    qfloat16* colData = reinterpret_cast<qfloat16*>(m_fftColScratch.data());
                    for (int i = 0; i < n; ++i) {
                        colData[i] = qfloat16(
                            qBound(0.0f, (trace[i] - minDbm) / range, 1.0f));
                    }
                }
                QRhiTextureSubresourceUploadDescription colDesc;
                colDesc.setData(m_fftColScratch);
                colDesc.setSourceSize(QSize(n, 1));
                batch->uploadTexture(m_fftColTex, QRhiTextureUploadEntry(0, 0, colDesc));
                m_panStats.fftVboBytes += static_cast<quint64>(m_fftColScratch.size());

                // UBO — see panscope.frag's U block.
                const QColor dk = m_fftFillColor.darker(300);
                const float fa = m_fftFillAlpha;
                const float ubo[24] = {
                    static_cast<float>(specContentW) * fbDpr,       // plot: wPx
                    static_cast<float>(specH) * fbDpr,              // hPx
                    static_cast<float>(n),                          // columnCount
                    1.0f,                                           // hasData
                    // Match the old vertex bake: stroke half-widths were
                    // DEVICE-pixel offsets with no dpr scaling.
                    m_fftLineWidth,                                 // coreHalfWidthPx
                    kFftLineFeatherPx,                              // featherPx
                    kFftLineCoreAlpha,
                    kFftLineFeatherAlpha,
                    fa,                                             // fillAlpha
                    m_fftHeatMap ? 1.0f : 0.0f,
                    0.0f,                                           // fillP.z — unused (pad)
                    0.0f,
                    static_cast<float>(m_fftFillColor.redF()),
                    static_cast<float>(m_fftFillColor.greenF()),
                    static_cast<float>(m_fftFillColor.blueF()),
                    1.0f,
                    static_cast<float>(dk.redF()),
                    static_cast<float>(dk.greenF()),
                    static_cast<float>(dk.blueF()),
                    1.0f,
                    // lineColor — trace stroke, independent of fill (#4239)
                    static_cast<float>(m_fftLineColor.redF()),
                    static_cast<float>(m_fftLineColor.greenF()),
                    static_cast<float>(m_fftLineColor.blueF()),
                    1.0f,
                };
                batch->updateDynamicBuffer(m_fftScopeUbo, 0, sizeof(ubo), ubo);
            }
        }
        if (!is3D)
            m_panStats.fftBuildUs +=
                static_cast<quint64>(panStatsFftTimer.nsecsElapsed() / 1000);
    }

    cb->resourceUpdate(batch);

    // Begin render pass
    const QColor clearColor(0x0a, 0x0a, 0x14);
    cb->beginPass(renderTarget(), clearColor, {1.0f, 0});

    const QSize outputSize = renderTarget()->pixelSize();
    const float dpr = outputSize.width() / static_cast<float>(qMax(1, w));

    // Draw waterfall quad — viewport restricted to waterfall rect. Always drawn:
    // 3DSS replaces only the spectrum trace, the waterfall stays below it.
    const bool useRowFrequencyPipeline =
        m_wfPipelineMode == WaterfallPipelineMode::RowFrequencyFrames
        && m_wfFramePipeline && m_wfFrameSrb && m_wfFrameTexReady;
    QRhiGraphicsPipeline* waterfallPipeline = useRowFrequencyPipeline
        ? m_wfFramePipeline : m_wfPipeline;
    QRhiShaderResourceBindings* waterfallSrb = useRowFrequencyPipeline
        ? m_wfFrameSrb : m_wfSrb;
    if (waterfallPipeline && waterfallSrb) {
        cb->setGraphicsPipeline(waterfallPipeline);
        cb->setShaderResources(waterfallSrb);
        // QRhiViewport: (x, y, width, height) — y is bottom-up in GL convention
        float vpX = static_cast<float>(wfRect.x()) * dpr;
        float vpY = static_cast<float>(h - wfRect.bottom() - 1) * dpr;
        float vpW = static_cast<float>(wfContentW) * dpr;
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

    // Draw the 3DSS surface in the SPECTRUM viewport (FFT z-slot; waterfall + bg
    // below, static overlay on top).
    if (is3D && m_dssMeshReady && m_dssMeshFillPipeline) {
        // GPU height-map mesh: static grid, height from the ring texture.
        const QRhiViewport vp(static_cast<float>(specRect.x()) * dpr,
                              static_cast<float>(h - specRect.bottom() - 1) * dpr,
                              static_cast<float>(specContentW) * dpr,
                              static_cast<float>(specRect.height()) * dpr);
        const int drawnRows = m_dss.rowCount();

        if (drawnRows > 0) {
            // Opaque fill curtains, back (oldest) -> front (newest) for occlusion.
            // The static VBO is already ordered back-to-front for all rows; drawing
            // only the valid suffix keeps startup frames short while the history fills.
            const int skippedRows = m_dss.rows() - drawnRows;
            cb->setGraphicsPipeline(m_dssMeshFillPipeline);
            cb->setShaderResources(m_dssMeshSrb);
            cb->setViewport(vp);
            {
                const QRhiCommandBuffer::VertexInput vbuf(m_dssMeshVbo, 0);
                cb->setVertexInput(0, 1, &vbuf);
                cb->draw(drawnRows * dssFillVerticesPerRow(), 1,
                         skippedRows * dssFillVerticesPerRow(), 0);
            }
            // Dim per-row trace outline on top.
            cb->setGraphicsPipeline(m_dssMeshLinePipeline);
            cb->setShaderResources(m_dssMeshSrb);
            cb->setViewport(vp);
            {
                const QRhiCommandBuffer::VertexInput vbuf(m_dssMeshLineVbo, 0);
                cb->setVertexInput(0, 1, &vbuf);
                cb->draw(drawnRows * dssLineVerticesPerRow(), 1,
                         skippedRows * dssLineVerticesPerRow(), 0);
            }
        }
    } else if (is3D && m_ovPipeline && m_dssSrb && m_dssTexW > 0) {
        // Cached-image fallback quad, stretched to the specRect viewport.
        cb->setGraphicsPipeline(m_ovPipeline);
        cb->setShaderResources(m_dssSrb);
        cb->setViewport({static_cast<float>(specRect.x()) * dpr,
                         static_cast<float>(h - specRect.bottom() - 1) * dpr,
                         static_cast<float>(specContentW) * dpr,
                         static_cast<float>(specRect.height()) * dpr});
        const QRhiCommandBuffer::VertexInput vbuf(m_ovVbo, 0);
        cb->setVertexInput(0, 1, &vbuf);
        cb->draw(4);
    }

    // Draw FFT spectrum — viewport restricted to spectrum rect (2D only).
    // One full-viewport quad; panscope.frag evaluates fill + feather + core
    // stroke per pixel from the column texture.
    if (!is3D && m_fftScopePipeline && m_ovVbo && fftTracePointCount > 0) {
        float specVpX = static_cast<float>(specRect.x()) * dpr;
        float specVpY = static_cast<float>(h - specRect.bottom() - 1) * dpr;
        float specVpW = static_cast<float>(specContentW) * dpr;
        float specVpH = static_cast<float>(specRect.height()) * dpr;
        cb->setGraphicsPipeline(m_fftScopePipeline);
        cb->setShaderResources(m_fftScopeSrb);
        cb->setViewport({specVpX, specVpY, specVpW, specVpH});
        const QRhiCommandBuffer::VertexInput vbuf(m_ovVbo, 0);
        cb->setVertexInput(0, 1, &vbuf);
        cb->draw(4);
    }

    // Frequency-anchored overlay geometry follows the preview transform. When
    // the preview shader is unavailable, the CPU fallback refreshes this
    // texture at the target frame and the ordinary overlay pipeline draws it.
    QRhiGraphicsPipeline* frequencyOverlayPipeline =
        m_frequencyPreviewActive && m_ovPreviewPipeline
            ? m_ovPreviewPipeline : m_ovPipeline;
    QRhiShaderResourceBindings* frequencyOverlaySrb =
        m_frequencyPreviewActive && m_ovPreviewPipeline
            ? m_ovPreviewSrb : m_ovFrequencySrb;
    if (frequencyOverlayPipeline && frequencyOverlaySrb) {
        cb->setGraphicsPipeline(frequencyOverlayPipeline);
        cb->setShaderResources(frequencyOverlaySrb);
        cb->setViewport({0, 0,
            static_cast<float>(outputSize.width()),
            static_cast<float>(outputSize.height())});
        const QRhiCommandBuffer::VertexInput vbuf(m_ovVbo, 0);
        cb->setVertexInput(0, 1, &vbuf);
        cb->draw(4);
    }

    // Screen-space chrome stays fixed above the transformed markers: frequency
    // and amplitude scales, SQL lines, cursor/status text, and time controls.
    if (m_ovPipeline && m_ovSrb) {
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

    // VFO flag/widget repositioning now runs earlier (repositionVfoFlags(),
    // before the flag-layer render) so GPU-composited flags follow the marker
    // without a one-frame lag (#3617).

    raisePanadapterMessageOverlay();

    if (perfEnabled) {
        PerfTelemetry::instance().recordRender(
            static_cast<double>(PerfTelemetry::nowNs() - perfStartNs) / 1000000.0);
    }
}

// Reposition VFO widgets. paintEvent() is compiled only in software mode
// (#else !AETHER_GPU_SPECTRUM), so in GPU mode this is the sole place VFOs are
// repositioned. Logic mirrors the paintEvent() block below exactly.  Called
// from renderGpuFrame BEFORE the flag-layer render so a dragged flag's snapshot
// is rasterized at this frame's position (no one-frame lag).  (#3617)
void SpectrumWidget::repositionVfoFlags(const QRect& specRect)
{
    QVector<VfoPos> vfos;
    for (const auto& so : m_sliceOverlays) {
        if (auto* vw = m_vfoWidgets.value(so.sliceId, nullptr)) {
            int x = mhzToX(so.freqMhz);
            if (so.mode == "RTTY" || so.mode == "DIGL") {
                double hiMhz = so.freqMhz + so.filterHighHz / 1.0e6;
                x = mhzToX(hiMhz) + 4;
            }
            vfos.append({so.sliceId, x, vw, so.splitPartnerId, &so});
        }
    }
    std::sort(vfos.begin(), vfos.end(), [](const VfoPos& a, const VfoPos& b) {
        return a.x < b.x;
    });

    const int panelW = vfos.isEmpty() ? 0 : vfos[0].w->width();
    // Flip flags against the content edge (width minus the right strip), not the
    // raw widget edge — a right-facing flag at the true right edge slides under
    // the dBm tape (#3482).
    const int specW  = contentWidth();

    QMap<int, VfoWidget::FlagDir> dirMap;
    assignDiversityPairDirections(vfos, dirMap);
    assignSplitPairDirections(vfos, dirMap);
    assignModeForcedDirections(m_sliceOverlays, dirMap);

    if (vfos.size() == 1) {
        VfoWidget::FlagDir dir = dirMap.value(vfos[0].sliceId, VfoWidget::Auto);
        if (!dirMap.contains(vfos[0].sliceId) && vfos[0].overlay) {
            dir = singleVfoFlagDirectionForOverlay(
                *vfos[0].overlay, vfos[0].w, vfos[0].x, specW);
        }
        vfos[0].w->updatePosition(vfos[0].x, specRect.top(), dir);
    } else {
        for (int i = 0; i < vfos.size(); ++i) {
            VfoWidget::FlagDir dir = VfoWidget::Auto;
            if (dirMap.contains(vfos[i].sliceId)) {
                dir = dirMap[vfos[i].sliceId];
            } else {
                dir = deconflictedVfoFlagDirection(vfos, i, panelW, specW);
            }
            vfos[i].w->updatePosition(vfos[i].x, specRect.top(), dir);
        }
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
    if (m_resizeBufferSettleTimer && m_resizeBufferSettleTimer->isActive()) {
        // Keep presenting fresh FFT/waterfall data into the last settled render
        // target while QRhiWidget stretches it to the live geometry. This
        // preserves motion without recreating full-size resources at every
        // intermediate mouse position (once per visible panadapter).
        static const bool liveResizePreviewEnabled = [] {
            if (!qEnvironmentVariableIsSet("AETHER_RESIZE_LIVE_PREVIEW")) {
                return true;
            }
            return qEnvironmentVariableIntValue("AETHER_RESIZE_LIVE_PREVIEW") != 0;
        }();
        if (liveResizePreviewEnabled && m_resizePresentationSize.isValid()) {
            ++m_resizePreviewFrameCount;
            renderGpuFrame(cb, m_resizePresentationSize, true);
            return;
        }
        ++m_resizeFrameHoldCount;
        return;
    }
    renderGpuFrame(cb, size(), false);
}

void SpectrumWidget::releaseResources()
{
    releaseWaterfallFramePipelineResources();
    delete m_wfPipeline;     m_wfPipeline = nullptr;
    delete m_wfSrb;          m_wfSrb = nullptr;
    delete m_wfVbo;          m_wfVbo = nullptr;
    delete m_wfUbo;          m_wfUbo = nullptr;
    delete m_wfGpuTex;       m_wfGpuTex = nullptr;
    delete m_wfSampler;      m_wfSampler = nullptr;
    m_wfPipelineFallbackReason.clear();

    delete m_ovPipeline;     m_ovPipeline = nullptr;
    delete m_ovSrb;          m_ovSrb = nullptr;
    delete m_ovFrequencySrb; m_ovFrequencySrb = nullptr;
    delete m_ovFrequencyGpuTex; m_ovFrequencyGpuTex = nullptr;
    delete m_ovPreviewPipeline; m_ovPreviewPipeline = nullptr;
    delete m_ovPreviewSrb;      m_ovPreviewSrb = nullptr;
    delete m_ovPreviewUbo;      m_ovPreviewUbo = nullptr;
    delete m_ovVbo;          m_ovVbo = nullptr;
    delete m_ovGpuTex;       m_ovGpuTex = nullptr;
    delete m_ovSampler;      m_ovSampler = nullptr;

    // Bg-image layer (added with the FFT-above-bg fix) — same lifecycle
    // as the overlay scaffolding it lives alongside; release in the same
    // teardown sweep.
    delete m_bgSrb;          m_bgSrb = nullptr;
    delete m_bgGpuTex;       m_bgGpuTex = nullptr;

    // 3DSS surface layer — same lifecycle as the overlay scaffolding.
    delete m_dssSrb;         m_dssSrb = nullptr;
    delete m_dssGpuTex;      m_dssGpuTex = nullptr;
    m_dssTexW = m_dssTexH = 0;
    m_dssTexNeedsUpload = true;
    m_dssLastUploadedGen = ~0ull;

    // 3DSS GPU mesh.
    delete m_dssMeshFillPipeline; m_dssMeshFillPipeline = nullptr;
    delete m_dssMeshLinePipeline; m_dssMeshLinePipeline = nullptr;
    delete m_dssMeshSrb;          m_dssMeshSrb = nullptr;
    delete m_dssMeshVbo;          m_dssMeshVbo = nullptr;
    delete m_dssMeshLineVbo;      m_dssMeshLineVbo = nullptr;
    delete m_dssMeshUbo;          m_dssMeshUbo = nullptr;
    delete m_dssHeightTex;        m_dssHeightTex = nullptr;
    delete m_dssPaletteTex;       m_dssPaletteTex = nullptr;
    delete m_dssHeightSampler;    m_dssHeightSampler = nullptr;
    delete m_dssPaletteSampler;   m_dssPaletteSampler = nullptr;
    m_dssMeshReady = false;
    m_dssMeshHeadUploaded = -1;
    m_dssMeshRowGenUploaded = ~0ull;
    m_dssLutToken = ~0ull;

    delete m_fftScopePipeline; m_fftScopePipeline = nullptr;
    delete m_fftScopeSrb;      m_fftScopeSrb = nullptr;
    delete m_fftScopeUbo;      m_fftScopeUbo = nullptr;
    delete m_fftColTex;        m_fftColTex = nullptr;
    delete m_fftColSampler;    m_fftColSampler = nullptr;
    m_fftColTexW = 0;

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

    // panstats: software-path paint cost (GPU builds only reach here before
    // QRhi init or after GPU teardown).
    m_panStats.paintEvents++;
    struct PaintCost {
        quint64& acc;
        QElapsedTimer t;
        explicit PaintCost(quint64& a) : acc(a) { t.start(); }
        ~PaintCost() { acc += static_cast<quint64>(t.nsecsElapsed() / 1000); }
    } panStatsPaintCost(m_panStats.paintUs);
    Q_UNUSED(ev);

    QElapsedTimer frameTimer;
    frameTimer.start();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const int chromeH  = freqScaleH() + DIVIDER_H;
    const int contentH = height() - chromeH;
    // Both via the shared helpers so this rect and the waterfall QImage
    // allocated by applySettledResizeBuffers() are always the same height — a
    // 1:1 vertical blit. (wfH == contentH - specH by construction; see
    // waterfallPixelHeight().)
    const int specH    = spectrumPixelHeight();
    const int wfH      = waterfallPixelHeight();

    const int divY     = specH;
    const int scaleY   = specH + DIVIDER_H;
    const int wfY      = scaleY + freqScaleH();

    const QRect specRect (0, 0,       width(), specH);
    const QRect divRect  (0, divY,    width(), DIVIDER_H);
    const QRect scaleRect(0, scaleY,  width(), freqScaleH());
    const QRect wfRect   (0, wfY,     width(), wfH);

    // The FFT trace and waterfall span only the frequency canvas (width minus
    // the right dBm / time strip) so they end at the tape rather than painting
    // under it, matching the contentWidth() mapping used by mhzToX (#3482).
    const QRect specContentRect = specRect.adjusted(0, 0, -DBM_STRIP_W, 0);
    const QRect wfContentRect   = wfRect.adjusted(0, 0, -DBM_STRIP_W, 0);

    const bool is3D = (m_spectrumRenderMode == SpectrumRenderMode::Mode3D);
    const float dssFrameFloorDbm = is3D
        ? dssFloorDbm()
        : m_lastDetectDssFloor;

    // Spectrum region: the 3DSS surface, or the classic bg + grid + FFT trace.
    // 3DSS replaces ONLY the spectrum trace — the divider, freq scale, waterfall,
    // overlays, and scales below run identically in both modes.
    if (is3D) {
        // Cap the software surface (like the GPU path) so a HiDPI/maximized
        // window doesn't rebuild a multi-megapixel QImage every frame; the
        // surface is intrinsically low-res, so stretch it on draw.
        const QImage& surf =
            buildDssImage(specRect.size().boundedTo(QSize(kDssMaxW, kDssMaxH)),
                          0, dssFrameFloorDbm);
        p.fillRect(specRect, m_bgFillColor);
        if (!surf.isNull()) {
            p.drawImage(specContentRect, surf);
        }
    } else {
        // Composition z-order: bg fill → bg image → grid → FFT trace.
        p.fillRect(specRect, m_bgFillColor);
        if (!m_bgImage.isNull()) {
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
        drawSpectrum(p, specContentRect);
    }

    if (m_bandPlanFontSize > 0) drawBandPlan(p, specRect);

    p.fillRect(divRect, AetherSDR::ThemeManager::instance().color("color.background.1"));
    p.setPen(QColor(m_draggingDivider ? 0x00b4d8 : 0x304050));
    p.drawLine(divRect.left(), divRect.center().y(), divRect.right(), divRect.center().y());

    drawFreqScale(p, scaleRect);
    p.fillRect(wfRect, Qt::black);  // paint the strip gap before the time tape
    drawWaterfall(p, wfContentRect);
    drawTnfMarkers(p, specRect);
    if (m_showSpots || m_showSHistory) drawSpotMarkers(p, specRect);
    drawSwrSweep(p, specRect);
    drawSliceMarkers(p, specRect, wfRect);
    drawSmartMtrValueLabels(p);
    drawOffScreenSlices(p, specRect);

    // dB-axis overlays don't map onto the 3D surface (see GPU path) — suppress.
    if (!is3D) {
        drawAutoSqlFloor(p, specRect);
        drawSquelchLine(p, specRect);
    }

    drawConnectionAnimation(p, specRect);

    // Reposition all VFO widgets — deconflict flags so they fly away from each other
    // Split pairs always face each other: RX←  →TX
    {
        // Collect visible VFOs sorted by screen X position
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
                vfos.append({so.sliceId, x, w, so.splitPartnerId, &so});
            }
        }
        std::sort(vfos.begin(), vfos.end(), [](const VfoPos& a, const VfoPos& b) {
            return a.x < b.x;
        });

        const int panelW = vfos.isEmpty() ? 0 : vfos[0].w->width();
        const int specW = contentWidth();  // flip against the tape edge (#3482)

        // First pass: assign directions for role-locked pairs
        QMap<int, VfoWidget::FlagDir> dirMap;  // sliceId → direction
        assignDiversityPairDirections(vfos, dirMap);
        assignSplitPairDirections(vfos, dirMap);

        // Second pass: assign remaining mode-forced VFOs
        // In RTTY/DIGL, force flag to fly right so it doesn't cover M/S passband
        assignModeForcedDirections(m_sliceOverlays, dirMap);

        if (vfos.size() == 1) {
            VfoWidget::FlagDir dir = dirMap.value(vfos[0].sliceId, VfoWidget::Auto);
            if (!dirMap.contains(vfos[0].sliceId) && vfos[0].overlay) {
                dir = singleVfoFlagDirectionForOverlay(
                    *vfos[0].overlay, vfos[0].w, vfos[0].x, specW);
            }
            vfos[0].w->updatePosition(vfos[0].x, specRect.top(), dir);
        } else {
            for (int i = 0; i < vfos.size(); ++i) {
                VfoWidget::FlagDir dir = VfoWidget::Auto;

                if (dirMap.contains(vfos[i].sliceId)) {
                    // Split pair or RTTY: use pre-assigned direction
                    dir = dirMap[vfos[i].sliceId];
                } else {
                    dir = deconflictedVfoFlagDirection(vfos, i, panelW, specW);
                }

                vfos[i].w->updatePosition(vfos[i].x, specRect.top(), dir);
            }
        }
    }
    // Active widget on top, but overlay stays above all
    applyActiveVfoZOrder();

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
        // Flip at the content edge — the strip is opaque now (#3482).
        if (lx + tw > contentWidth()) lx = m_cursorPos.x() - tw - 4;
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
        if (lx + tw > contentWidth()) { lx = cx - tw - 4; }  // content edge (#3482)
        int ly = m_cursorPos.y() - th - 4;
        if (ly < 0) { ly = m_cursorPos.y() + 16; }
        p.fillRect(lx, ly, tw, th, AetherSDR::theme::withAlpha("color.background.0", 200));
        p.setPen(AetherSDR::ThemeManager::instance().color("color.text.primary"));
        p.drawText(lx + 4, ly + fm.ascent() + 2, label);
    }

    // dBm strip: 2D draws a linear dBm axis; 3D maps the ticks onto the front
    // (live) trace's ridge band. Same strip chrome and click targets either way.
    if (is3D) {
        drawDbmScale3D(p, specRect, dssFrameFloorDbm);
    } else {
        drawDbmScale(p, specRect);
    }
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
double SpectrumWidget::effectiveGridStepMhz(int /* widgetWidth */) const
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
    const QVector<float>& fftBins =
        buildFftDisplayTrace(displaySpectrumBins(),
                             qMax(2, r.width() * kFftDisplayOversample));
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
    struct Pt { float x, y, t; };
    QVector<Pt> pts(n);
    for (int i = 0; i < n; ++i) {
        const float dbm  = fftBins[i];
        const float norm = qBound(0.0f, (m_refLevel - dbm) / m_dynamicRange, 1.0f);
        const float xFrac = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
        pts[i].x = static_cast<float>(r.left()) + xFrac * static_cast<float>(w);
        pts[i].y = static_cast<float>(r.top())
            + qBound(0.0f, norm * static_cast<float>(h), static_cast<float>(h - 1));
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
    linePen.setCapStyle(Qt::RoundCap);
    linePen.setJoinStyle(Qt::RoundJoin);

    if (m_fftHeatMap) {
        // Heat map fill: per-column vertical gradient from heat color at top to dark blue at base
        const int bottom = r.bottom();
        for (int i = 0; i < n - 1; ++i) {
            QPolygonF trapezoid;
            trapezoid << QPointF(pts[i].x, pts[i].y)
                      << QPointF(pts[i + 1].x, pts[i + 1].y)
                      << QPointF(pts[i + 1].x, bottom)
                      << QPointF(pts[i].x, bottom);

            float avgT = (pts[i].t + pts[i + 1].t) * 0.5f;
            QColor top = heatColor(avgT);
            const float swFillAlpha = m_fftFillAlpha;
            top.setAlphaF(swFillAlpha * 0.3f);
            QColor bot(0, 0, 77, static_cast<int>(255 * swFillAlpha));
            QLinearGradient grad(0, std::min(pts[i].y, pts[i + 1].y), 0, bottom);
            grad.setColorAt(0.0, top);
            grad.setColorAt(1.0, bot);
            p.setPen(Qt::NoPen);
            p.setBrush(grad);
            p.drawPolygon(trapezoid);
        }

        // Heat map line: per-segment coloring
        if (drawLine) {
            for (int i = 0; i < n - 1; ++i) {
                float avgT = (pts[i].t + pts[i + 1].t) * 0.5f;
                linePen.setColor(heatColor(avgT));
                p.setPen(linePen);
                p.drawLine(QPointF(pts[i].x, pts[i].y),
                           QPointF(pts[i + 1].x, pts[i + 1].y));
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
        // Cull at the content edge: a spot past the tape would be laid out
        // invisibly under the opaque strip while still consuming a cluster
        // bin — demoting a real visible spot into the "+N" overflow (#3482).
        if (x < 0 || x > contentWidth()) { ++mIdx; continue; }

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

void SpectrumWidget::drawSmartMtrValueLabels(QPainter& p)
{
    // Each flag draws its own SmartMTR value labels (if active) into our overlay
    // painter, in our (widget) coordinates — so they land on top of the slice
    // markers drawn just above. Drawn last = guaranteed on top of the slice.
    for (VfoWidget* w : m_vfoWidgets) {
        if (w)
            w->drawSmartMtrLabels(p);
    }
}

// TODO(a11y): QAccessibleInterface needed — the adaptive-filter markers drawn
// here (edge triangles, audio low/high cut labels, AUTO status ball) are
// data-bearing content a screen reader can't introspect. Expose via a
// SpectrumWidget QAccessibleInterface. Tracked in aethersdr/AetherSDR#3957.
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
        const double fLoMhz = so.freqMhz + so.filterLowHz / 1.0e6;
        const double fHiMhz = so.freqMhz + so.filterHighHz / 1.0e6;

        const int vfoX = mhzToX(so.freqMhz);
        const int fX1  = mhzToX(fLoMhz);
        const int fX2  = mhzToX(fHiMhz);
        const int fW   = fX2 - fX1;

        auto drawFrequencyLine = [&](int x) {
            p.drawLine(x, specRect.top(), x, specRect.bottom());
            if (m_extendedFrequencyLine && !wfRect.isEmpty()) {
                p.drawLine(x, wfRect.top(), x, wfRect.bottom());
            }
        };

        // Passive target marker: Center Lock is a controller state, so its
        // visible affordance stays attached to the locked slice rather than
        // adding another mouse control to the pan chrome.
        if (so.sliceId == m_centerLockSliceId) {
            const QPoint targetCenter(vfoX, specRect.top() + 18);
            p.setBrush(AetherSDR::theme::withAlpha("color.background.0", 190));
            p.setPen(QPen(QColor(col.red(), col.green(), col.blue(), 240), 2));
            p.drawEllipse(targetCenter, 6, 6);
            p.setPen(QPen(QColor(col.red(), col.green(), col.blue(), 240), 1));
            p.drawLine(targetCenter.x() - 9, targetCenter.y(),
                       targetCenter.x() + 9, targetCenter.y());
            p.drawLine(targetCenter.x(), targetCenter.y() - 9,
                       targetCenter.x(), targetCenter.y() + 9);
        }

        // ── Filter passband shading ──────────────────────────────────────
        // Default stays spectrum-only because the waterfall is a historical
        // record of received signals (#1270). The waterfall fill is opt-in.
        const QColor passbandFill(bandCol.red(), bandCol.green(), bandCol.blue(), 35);
        p.fillRect(QRect(fX1, specRect.top(), fW, specRect.height()), passbandFill);
        if (m_extendedPassband && !wfRect.isEmpty()) {
            p.fillRect(QRect(fX1, wfRect.top(), fW, wfRect.height()), passbandFill);
        }

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
            drawFrequencyLine(markX);

            // Space line — red, dashed
            p.setPen(QPen(AetherSDR::theme::withAlpha("color.accent.danger", 200), 1, Qt::DashLine));
            drawFrequencyLine(spaceX);

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
            drawFrequencyLine(markerX);

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

        // ── Adaptive RX filter edge markers (RFC #3878) ──────────────────
        // Floor-level triangles whose apex sticks to each filter edge and
        // points inward, with the audio cut value labelled just outside the
        // body. Shown whenever the feature is enabled (even at the baseline
        // width); hidden only when the passband is too narrow to read two
        // markers. White (#ffffff) to match the SmartMTR extreme triangles.
        const bool adaptiveSsb = so.adaptiveEnabled &&
            (so.mode == QStringLiteral("USB") || so.mode == QStringLiteral("LSB"));
        if (adaptiveSsb && fW >= 14) {
            float floorDbm = m_measuredNoiseFloorDbm;
            float norm;
            if (floorDbm > -500.0f && m_dynamicRange > 0.0f) {
                norm = (m_refLevel - floorDbm) / m_dynamicRange;
            } else {
                norm = 0.85f;  // floor not measured yet — sit low in the pane
            }
            const int floorY = specRect.top()
                + static_cast<int>(std::clamp(norm, 0.0f, 1.0f) * specRect.height());

            const int triLen  = 7;   // apex (on edge) -> base (outside)
            const int triHalf = 5;   // half base height
            const QColor mk(255, 255, 255);

            p.setPen(Qt::NoPen);
            p.setBrush(mk);
            QPolygon triL;  // left (low-freq) edge: apex on fX1, body to the left
            triL << QPoint(fX1, floorY)
                 << QPoint(fX1 - triLen, floorY - triHalf)
                 << QPoint(fX1 - triLen, floorY + triHalf);
            p.drawPolygon(triL);
            QPolygon triR;  // right (high-freq) edge: apex on fX2, body to the right
            triR << QPoint(fX2, floorY)
                 << QPoint(fX2 + triLen, floorY - triHalf)
                 << QPoint(fX2 + triLen, floorY + triHalf);
            p.drawPolygon(triR);

            // Labels: audio cut magnitudes, just beyond each triangle base.
            QFont lf = p.font();
            lf.setPixelSize(11);              // match SmartMTR extreme labels
            lf.setWeight(QFont::Light);       // (~11 px, Light weight)
            p.setFont(lf);
            const QFontMetrics fm(lf);
            const QString lblL = QString::number(qAbs(so.filterLowHz));
            const QString lblR = QString::number(qAbs(so.filterHighHz));
            const int ty  = floorY + fm.ascent() / 2 - 1;
            const int gap = 3;
            const int lxR = fX2 + triLen + gap;                                  // left-aligned
            const int lxL = fX1 - triLen - gap - fm.horizontalAdvance(lblL);     // right-aligned
            const auto haloText = [&](int x, const QString& s) {
                p.setPen(QColor(0, 0, 0, 200));
                p.drawText(x - 1, ty, s); p.drawText(x + 1, ty, s);
                p.drawText(x, ty - 1, s); p.drawText(x, ty + 1, s);
                p.setPen(mk);
                p.drawText(x, ty, s);
            };
            haloText(lxL, lblL);
            haloText(lxR, lblR);

            // Status ball just outside the HIGH-CUT label (RFC #3878):
            // green = AUTO fit applied, gray = enabled but idle (no signal to
            // fit right now — e.g. weak signal or holding). Idle is a normal
            // waiting state, not an error, so it reads neutral rather than red.
            // The high-cut is the edge farthest from the carrier — the larger
            // |offset| — which is the right edge for USB and the left edge for LSB.
            const bool hiOnRight = qAbs(so.filterHighHz) >= qAbs(so.filterLowHz);
            const int  ballR = 4;
            const int  bgap  = 5;
            const int  ballCx = hiOnRight
                ? lxR + fm.horizontalAdvance(lblR) + bgap + ballR   // outward (right)
                : lxL - bgap - ballR;                               // outward (left)
            const QColor ballCol = so.adaptiveActive
                ? AetherSDR::theme::withAlpha("color.accent.success", 255)
                : AetherSDR::theme::withAlpha("color.text.disabled", 255);
            p.setPen(QPen(QColor(0, 0, 0, 160), 1));   // dark outline for contrast
            p.setBrush(ballCol);
            p.drawEllipse(QPoint(ballCx, floorY), ballR, ballR);
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
        double pxPerStep = (stepMhz / m_bandwidthMhz) * contentWidth();  // label spacing in mapped px (#3482)
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

void SpectrumWidget::drawDbmScaleChrome(QPainter& p, const QRect& specRect)
{
    const int stripX = specRect.right() - DBM_STRIP_W + 1;
    const QRect strip(stripX, specRect.top(), DBM_STRIP_W, specRect.height());

    // Opaque background: since #3482 the FFT trace/waterfall end at the strip's
    // left edge, so nothing meaningful renders beneath it — a solid fill gives a
    // crisp right edge instead of letting the bg/grid bleed through and read as
    // right-side asymmetry against the hard left window border.
    p.fillRect(strip, AetherSDR::ThemeManager::instance().color("color.background.0"));

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
}

void SpectrumWidget::drawDbmScaleLabels(QPainter& p, const QRect& specRect,
                                        float topDbm, float rangeDb)
{
    if (rangeDb <= 0.0f) {
        return;
    }
    const int stripX = specRect.right() - DBM_STRIP_W + 1;

    // ── dBm labels — full-height LINEAR axis: topDbm at the top, topDbm-rangeDb
    //    at the baseline, evenly spaced across specRect.height(). ──────────
    QFont f = p.font();
    f.setPointSize(7);
    p.setFont(f);
    const QFontMetrics fm(f);

    const int labelTop = specRect.top() + DBM_ARROW_H + 4;

    // Use adaptive step: aim for ~4-6 labels
    float rawStep = rangeDb / 5.0f;
    float stepDb;
    if      (rawStep >= 20.0f) stepDb = 20.0f;
    else if (rawStep >= 10.0f) stepDb = 10.0f;
    else if (rawStep >= 5.0f)  stepDb = 5.0f;
    else                        stepDb = 2.0f;

    const float bottomDbm = topDbm - rangeDb;
    const float firstLabel = std::ceil(bottomDbm / stepDb) * stepDb;

    auto drawTickLabel = [&](float dbm, int y, int textBaseline) {
        p.setPen(AetherSDR::ThemeManager::instance().color("color.background.3"));
        p.drawLine(stripX, y, stripX + 4, y);

        const QString label = QString::number(static_cast<int>(std::lround(dbm)));
        p.setPen(AetherSDR::ThemeManager::instance().color("color.text.secondary"));
        p.drawText(stripX + 6, textBaseline, label);
    };

    for (float dbm = firstLabel; dbm <= topDbm; dbm += stepDb) {
        const float frac = (topDbm - dbm) / rangeDb;
        const int y = specRect.top() + static_cast<int>(frac * specRect.height());
        if (y < labelTop || y > specRect.bottom() - 5) continue;

        drawTickLabel(dbm, y, y + fm.ascent() / 2);
    }

    const int bottomY = specRect.bottom();
    if (bottomY >= labelTop) {
        drawTickLabel(bottomDbm, bottomY, bottomY - 2);
    }
}

void SpectrumWidget::drawDbmScale(QPainter& p, const QRect& specRect)
{
    drawDbmScaleChrome(p, specRect);
    drawDbmScaleLabels(p, specRect, m_refLevel, m_dynamicRange);
}

// ─── dBm scale strip for 3D stacked-trace mode ───────────────────────────────
//
// In 3D mode, a single right-side axis cannot be pixel-exact for every
// perspective row. Keep it as a full-height amplitude reference anchored to the
// 3D floor, so plain drag visibly shifts the dBm numbers and Ctrl/Meta-drag
// changes the span.
void SpectrumWidget::drawDbmScale3D(QPainter& p, const QRect& specRect,
                                    float floorDbm)
{
    drawDbmScaleChrome(p, specRect);
    // Round the span the same way the mesh/CPU surface do (buildDssImage /
    // renderGpuFrame use std::round(span*2)/2) so labels and surface agree to
    // the pixel instead of a sub-dB top/bottom skew (#3937).
    const float span = std::round(dssSpanDb() * 2.0f) / 2.0f;
    drawDbmScaleLabels(p, specRect, floorDbm + span, span);
}

// ─── Time scale (right edge of waterfall) ─────────────────────────────────────

void SpectrumWidget::drawTimeScale(QPainter& p, const QRect& wfRect)
{
    const QRect strip = waterfallTimeScaleRect(wfRect);
    const int stripX = strip.x();

    // Background. The frequency mapping reserves exactly DBM_STRIP_W (#3482),
    // so only the rightmost DBM_STRIP_W of the strip sits over dead canvas —
    // that part is opaque, matching the dBm strip, for a crisp right edge. In
    // history mode (LIVE off) the strip widens to 72 px and its LEFT half lies
    // on top of *mapped* waterfall content: keep that overlap translucent so
    // scrubbed history stays readable beneath the extra timestamps rather than
    // being blacked out by an opaque fill.
    const QColor bg0 = AetherSDR::ThemeManager::instance().color("color.background.0");
    if (strip.width() > DBM_STRIP_W) {
        const QRect overlap(stripX, strip.top(),
                            strip.width() - DBM_STRIP_W, strip.height());
        const QRect reserved(stripX + overlap.width(), strip.top(),
                             DBM_STRIP_W, strip.height());
        p.fillRect(overlap, AetherSDR::theme::withAlpha("color.background.0", 220));
        p.fillRect(reserved, bg0);
    } else {
        p.fillRect(strip, bg0);
    }

    // Left border line
    p.setPen(AetherSDR::ThemeManager::instance().color("color.background.2"));
    p.drawLine(stripX, wfRect.top(), stripX, wfRect.bottom());

    const QRect liveRect = waterfallLiveButtonRect(wfRect);
    p.setPen(AetherSDR::ThemeManager::instance().color("color.meter.bar.fill"));
    // Dedicated, independently-themable tokens for the waterfall LIVE chip
    // (decoupled from the shared accent.danger / text.label semantics, #3744).
    // Red while live, grey while viewing history; recolor either via the theme
    // editor's color.waterfall.live / color.waterfall.history tokens.
    p.setBrush(m_wfLive
        ? AetherSDR::ThemeManager::instance().color("color.waterfall.live")
        : AetherSDR::ThemeManager::instance().color("color.waterfall.history"));
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
