#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>

namespace AetherSDR {

enum class DssOutlinePipelineMode {
    DedicatedRibbonPipeline,
    SharedFillPipeline,
};

// QRhi's OpenGLES2 backend, which covers desktop GL as well as GLES, reuses the
// fill program for ribbon outlines: live probes showed flat/stale outlines
// with a separate, identically configured program.
// Whether the "3D Span" control can actually affect the display.
//
// rowSpanFactor is a dss_mesh.vert uniform, so ONLY the GPU height-map mesh
// honours it: DssRenderer::rebuild(), the CPU image fallback, ignores it and
// always draws the narrowing trapezoid. Two independent ways to end up there --
// a build configured with AETHER_GPU_SPECTRUM=OFF, and a runtime without
// RGBA16F support leaving m_dssMeshReady false -- and BOTH must disable the
// control, or it moves, labels and persists while nothing on screen changes,
// which an operator cannot tell apart from "this source ships no overhang".
constexpr bool dssRowSpanSupported(bool gpuSpectrumBuild, bool meshReady)
{
    return gpuSpectrumBuild && meshReady;
}

constexpr DssOutlinePipelineMode dssOutlinePipelineModeForBackend(
    bool openGlEs2Backend)
{
    return openGlEs2Backend
        ? DssOutlinePipelineMode::SharedFillPipeline
        : DssOutlinePipelineMode::DedicatedRibbonPipeline;
}

// The pipeline the outline draw binds, given the mode above. Templated on the
// pipeline type purely so the selection stays testable without a QRhi device:
// SpectrumWidget instantiates it with QRhiGraphicsPipeline*. Keeping the
// selection here rather than as a ternary at the draw site is what lets
// spectrum_preview_logic_test pin the mapping the renderer actually uses.
// dedicatedPipeline is null on OpenGL — never created — so the shared-fill
// answer must not depend on it.
template <typename PipelineT>
constexpr PipelineT* dssOutlinePipelineFor(DssOutlinePipelineMode mode,
                                           PipelineT* fillPipeline,
                                           PipelineT* dedicatedPipeline)
{
    return mode == DssOutlinePipelineMode::SharedFillPipeline
        ? fillPipeline
        : dedicatedPipeline;
}

struct FrequencyFrame {
    double centerMhz{0.0};
    double bandwidthMhz{0.0};

    bool isValid() const
    {
        return std::isfinite(centerMhz)
            && std::isfinite(bandwidthMhz)
            && centerMhz > 0.0
            && bandwidthMhz > 0.0;
    }
};

struct FrequencyPreviewTransform {
    double scale{1.0};
    double offset{0.0};
    bool valid{false};
};

inline double frequencyCanvasFraction(double localX, int contentWidth)
{
    const int safeWidth = std::max(1, contentWidth);
    return std::clamp(localX / static_cast<double>(safeWidth) - 0.5,
                      -0.5, 0.5);
}

inline double frequencyAtFraction(const FrequencyFrame& frame, double fraction)
{
    return frame.centerMhz + fraction * frame.bandwidthMhz;
}

inline double centerForAnchoredBandwidth(double anchorMhz,
                                         double anchorFraction,
                                         double bandwidthMhz)
{
    return anchorMhz - anchorFraction * bandwidthMhz;
}

inline FrequencyPreviewTransform frequencyPreviewTransform(
    const FrequencyFrame& base,
    const FrequencyFrame& target)
{
    if (!base.isValid() || !target.isValid()) {
        return {};
    }
    return FrequencyPreviewTransform{
        target.bandwidthMhz / base.bandwidthMhz,
        (target.centerMhz - base.centerMhz) / base.bandwidthMhz,
        true,
    };
}

// FFT packets do not carry their own center/bandwidth. During a zoom preview,
// keep the compact 3D surface in one uniform base frame until commit; treating
// each untagged arrival as the moving visual target creates artificial
// uncovered floor bands that then scroll through retained history.
//
// A pure pan is different: the radio is already returning bins for the moving
// center. Stamp those rows at the current frame so remapping them into the base
// plus the preview shader applies the pan exactly once, matching the waterfall.
inline bool dssUntaggedRowUsesPreviewBase(
    const FrequencyFrame& previewBase,
    const FrequencyFrame& previewTarget,
    bool nativePreviewActive)
{
    if (!nativePreviewActive
        || !previewBase.isValid() || !previewTarget.isValid()) {
        return false;
    }
    const double bandwidthScale = std::max(
        {1.0, std::abs(previewBase.bandwidthMhz),
         std::abs(previewTarget.bandwidthMhz)});
    return std::abs(
        previewTarget.bandwidthMhz - previewBase.bandwidthMhz)
        > bandwidthScale * 1.0e-9;
}

inline FrequencyFrame resolvedUntaggedDssFrame(
    const FrequencyFrame& requested,
    const FrequencyFrame& current,
    const FrequencyFrame& previewBase,
    bool usePreviewBase)
{
    if (requested.isValid()) {
        return requested;
    }
    if (usePreviewBase && previewBase.isValid()) {
        return previewBase;
    }
    return current;
}

inline double sourceUnitPosition(double targetUnitPosition,
                                 const FrequencyFrame& source,
                                 const FrequencyFrame& target)
{
    if (!source.isValid() || !target.isValid()
        || !std::isfinite(targetUnitPosition)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double targetFrequencyMhz = target.centerMhz
        + (targetUnitPosition - 0.5) * target.bandwidthMhz;
    return 0.5 + (targetFrequencyMhz - source.centerMhz)
        / source.bandwidthMhz;
}

struct FrequencyRangeCommand {
    double centerMhz{0.0};
    double bandwidthMhz{0.0};

    bool isValid() const
    {
        return FrequencyFrame{centerMhz, bandwidthMhz}.isValid();
    }
};

class FrequencyRangeCommandThrottle {
public:
    std::optional<FrequencyRangeCommand> request(
        const FrequencyRangeCommand& command,
        bool due,
        bool force)
    {
        if (!command.isValid()) {
            return std::nullopt;
        }
        m_pending = command;
        if (!due && !force) {
            return std::nullopt;
        }
        return takePending();
    }

    std::optional<FrequencyRangeCommand> takePending()
    {
        if (!m_pending.has_value()) {
            return std::nullopt;
        }
        const FrequencyRangeCommand command = *m_pending;
        m_pending.reset();
        return command;
    }

    void clear() { m_pending.reset(); }
    bool hasPending() const { return m_pending.has_value(); }

private:
    std::optional<FrequencyRangeCommand> m_pending;
};

// Coalesces bandwidth changes into one post-settle 3D floor reacquisition.
// A new zoom cancels any previously-armed frame so rapid gestures cannot
// resynchronize against an intermediate bandwidth.
class DssZoomFloorSyncGate {
public:
    void noteBandwidthChange()
    {
        m_bandwidthChangeQueued = true;
        m_waitingForFreshFrame = false;
        m_adjustmentAttempts = 0;
    }

    void settle(bool flex3dActive)
    {
        m_waitingForFreshFrame =
            m_bandwidthChangeQueued && flex3dActive;
        m_bandwidthChangeQueued = false;
    }

    bool consumeFreshFrame(bool frameReady)
    {
        if (!m_waitingForFreshFrame || !frameReady) {
            return false;
        }
        m_waitingForFreshFrame = false;
        m_adjustmentAttempts = 0;
        return true;
    }

    bool beginAdjustment(int maxAttempts)
    {
        if (!m_waitingForFreshFrame
            || m_adjustmentAttempts >= std::max(0, maxAttempts)) {
            return false;
        }
        ++m_adjustmentAttempts;
        return true;
    }

    void clear()
    {
        m_bandwidthChangeQueued = false;
        m_waitingForFreshFrame = false;
        m_adjustmentAttempts = 0;
    }

    bool bandwidthChangeQueued() const { return m_bandwidthChangeQueued; }
    bool waitingForFreshFrame() const { return m_waitingForFreshFrame; }
    int adjustmentAttempts() const { return m_adjustmentAttempts; }

private:
    bool m_bandwidthChangeQueued{false};
    bool m_waitingForFreshFrame{false};
    int m_adjustmentAttempts{0};
};

inline bool dssFrameFloorLooksClipped(int finiteBins,
                                      int minValueBins,
                                      int longestMinRunBins)
{
    return finiteBins >= 64
        && minValueBins >= std::max(32, finiteBins / 5)
        && longestMinRunBins >= std::max(16, finiteBins / 16);
}

// Clipped FLEX FFT input needs radio-side encoder headroom in both 2D and 3D.
// Deliberately takes no render-mode argument: limiting recovery to the 3D
// renderer regresses the shared 2D FFT path when y_pixels/range state is stale.
inline bool flexFftFrameNeedsHeadroomRecovery(bool kiwiActive,
                                              int finiteBins,
                                              int minValueBins,
                                              int longestMinRunBins)
{
    return !kiwiActive
        && dssFrameFloorLooksClipped(
            finiteBins, minValueBins, longestMinRunBins);
}

// Windows in which an FFT frame's dBm encoding does not correspond to the
// settled zoom, so it must not anchor the 3D floor. Kept out of the widget so
// the timing policy is testable without a live radio or a QWidget.
struct DssZoomFloorFrameGuards {
    // std::int64_t, not qint64: this header stays Qt-free so the logic can be
    // unit-tested without linking Qt. Callers pass qint64 epoch values.
    std::int64_t nowMs{0};
    std::int64_t notBeforeMs{0};    // radio still switching bandwidth
    std::int64_t txEndMs{0};        // 0 when no recent TX→RX transition
    std::int64_t postTxSettleMs{0}; // receiver AGC recovery window (#2117)
    bool scaleSettling{false};      // y_pixels change still settling
    bool rebaseActive{false};       // bins may be reprojected preview data
    bool draggingDbmScale{false};
};

inline bool dssZoomFloorFrameTrusted(const DssZoomFloorFrameGuards& guards)
{
    if (guards.nowMs < guards.notBeforeMs) {
        return false;
    }
    if (guards.txEndMs > 0
        && guards.nowMs - guards.txEndMs < guards.postTxSettleMs) {
        return false;
    }
    return !guards.scaleSettling && !guards.rebaseActive
        && !guards.draggingDbmScale;
}

enum class WaterfallPipelineMode {
    Legacy,
    RowFrequencyFrames,
};

struct WaterfallPaletteRecolorPlan {
    bool retainNativeFrames{false};
    FrequencyFrame primaryFrame;
    FrequencyFrame supplementalFrame;
};

inline WaterfallPaletteRecolorPlan waterfallPaletteRecolorPlan(
    WaterfallPipelineMode pipelineMode,
    const FrequencyFrame& historyFrame,
    const FrequencyFrame& supplementalHistoryFrame,
    const FrequencyFrame& viewportFrame)
{
    if (pipelineMode == WaterfallPipelineMode::RowFrequencyFrames) {
        return WaterfallPaletteRecolorPlan{
            true,
            historyFrame.isValid() ? historyFrame : viewportFrame,
            supplementalHistoryFrame.isValid()
                ? supplementalHistoryFrame : FrequencyFrame{},
        };
    }
    return WaterfallPaletteRecolorPlan{false, viewportFrame, {}};
}

// Scanline that a retained row of age `ageRows` occupies in a visible ring
// whose newest row sits at `writeRowOrigin`. appendVisibleRow() decrements the
// write row *before* writing, so age 0 is the origin itself and age grows
// downward, wrapping — which is exactly the order drawWaterfall() blits from.
//
// The origin is the whole reason a palette recolour is not just a viewport
// rebuild: a rebuild re-lays the ring out from scanline 0 (origin 0), which is
// invisible only because it repaints everything at once. Recolouring in place
// must keep the live origin, or the waterfall jumps by m_wfWriteRow rows the
// moment the operator touches the Scheme dropdown.
inline int waterfallVisibleRowForAge(int writeRowOrigin, int ageRows,
                                     int height)
{
    if (height <= 0) {
        return -1;
    }
    const int origin = ((writeRowOrigin % height) + height) % height;
    const int age = ((ageRows % height) + height) % height;
    return (origin + age) % height;
}

// Mirror of texturedquad.frag and texturedquad_rowframes.frag: both cubic
// shaders clamp source ages in logical history before mapping them into the
// physical ring. Wrapping an out-of-range tap directly would blend the newest
// and oldest rows across the visible history boundary.
inline int waterfallCubicPhysicalRowForSourceAge(int writeRowOrigin,
                                                 int sourceAge, int height)
{
    if (height <= 0) {
        return -1;
    }
    return waterfallVisibleRowForAge(
        writeRowOrigin, std::clamp(sourceAge, 0, height - 1), height);
}

struct WaterfallRowFrameReadiness {
    bool requested{false};
    bool formatSupported{false};
    bool textureCreated{false};
    bool samplerCreated{false};
    bool bindingsCreated{false};
    bool pipelineCreated{false};
};

inline WaterfallPipelineMode chooseWaterfallPipeline(
    const WaterfallRowFrameReadiness& readiness)
{
    return readiness.requested
            && readiness.formatSupported
            && readiness.textureCreated
            && readiness.samplerCreated
            && readiness.bindingsCreated
            && readiness.pipelineCreated
        ? WaterfallPipelineMode::RowFrequencyFrames
        : WaterfallPipelineMode::Legacy;
}

inline float waterfallScrollProgressRows(std::int64_t elapsedMs, float msPerRow,
                                         float distanceRows = 1.0f)
{
    if (elapsedMs <= 0 || !std::isfinite(msPerRow) || msPerRow <= 0.0f
        || !std::isfinite(distanceRows) || distanceRows <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(elapsedMs) / msPerRow,
                      0.0f, distanceRows);
}

inline float waterfallScrollSampleOffsetUnit(float progressRows,
                                              float distanceRows,
                                              int textureRows)
{
    if (!std::isfinite(progressRows) || !std::isfinite(distanceRows)
        || distanceRows <= 0.0f || textureRows <= 0) {
        return 0.0f;
    }
    return (distanceRows
            - std::clamp(progressRows, 0.0f, distanceRows))
        / static_cast<float>(textureRows);
}

inline bool dssFftScaleSettleActive(std::int64_t nowMs,
                                    std::int64_t settleUntilMs)
{
    return settleUntilMs > 0 && nowMs < settleUntilMs;
}

// Mirror of dss_mesh.vert's retained-row visibility edge. Scroll phase moves
// geometry only; it must never change the opacity of a stored row.
inline float dssHistoryAvailability(float sourceAge, float validRows,
                                    float rows, float remainingRows)
{
    if (!std::isfinite(sourceAge) || !std::isfinite(validRows)
        || !std::isfinite(rows) || !std::isfinite(remainingRows)
        || validRows <= 0.0f || rows < 1.0f || remainingRows < 0.0f) {
        return 0.0f;
    }
    return std::clamp(validRows - sourceAge, 0.0f, 1.0f);
}

// Mirror of dss_mesh.vert's exact retained sample selection. remainingRows is
// validated but deliberately does not affect the result: scroll animation must
// not morph a row's amplitude into either neighbouring FFT.
inline std::optional<float> dssRetainedSampleAge(float sourceAge,
                                                 float remainingRows,
                                                 float validRows,
                                                 float rows)
{
    if (!std::isfinite(sourceAge) || !std::isfinite(remainingRows)
        || !std::isfinite(validRows) || !std::isfinite(rows)
        || sourceAge < 0.0f || remainingRows < 0.0f || rows < 1.0f) {
        return std::nullopt;
    }
    const float cappedValidRows = std::clamp(validRows, 0.0f, rows);
    if (sourceAge >= cappedValidRows) {
        return std::nullopt;
    }
    return sourceAge;
}

struct DssFixedGridCrossfade {
    float baseAge{0.0f};
    float overlayAge{0.0f};
    float baseAlpha{1.0f};
    float overlayAlpha{0.0f};
};

// Mirror of dss_mesh.vert's fixed ridge-outline scroll. Both layers sample
// exact rows and trade opacity at a fixed depth. At the next head advance, the
// fully opaque overlay becomes the following interval's base at that same
// depth.
inline std::optional<DssFixedGridCrossfade> dssFixedGridCrossfade(
    float sourceAge, float progressRows, float distanceRows)
{
    if (!std::isfinite(sourceAge) || !std::isfinite(progressRows)
        || !std::isfinite(distanceRows) || sourceAge < 0.0f
        || progressRows < 0.0f || distanceRows <= 0.0f) {
        return std::nullopt;
    }
    const float overlayAlpha =
        std::clamp(progressRows / distanceRows, 0.0f, 1.0f);
    return DssFixedGridCrossfade{
        sourceAge + distanceRows,
        sourceAge,
        1.0f - overlayAlpha,
        overlayAlpha,
    };
}

// Actual per-layer alpha used by the ridge shader. The base is rendered first,
// then the overlay with source-over blending. Compensation keeps the combined
// opacity constant when both exact outlines cover the same pixel.
inline float dssSourceOverCrossfadeLayerAlpha(float opacity,
                                              float overlayPhase,
                                              bool overlayLayer)
{
    if (!std::isfinite(opacity) || !std::isfinite(overlayPhase)) {
        return 0.0f;
    }
    const float clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);
    const float phase = std::clamp(overlayPhase, 0.0f, 1.0f);
    if (!overlayLayer) {
        return clampedOpacity * (1.0f - phase);
    }
    const float baseAlpha = clampedOpacity * (1.0f - phase);
    return clampedOpacity * phase
        / std::max(1.0f - baseAlpha, 0.0001f);
}

inline std::optional<float> dssMovingGeometryDepth(float sourceAge,
                                                   float remainingRows,
                                                   float visibleRows)
{
    if (!std::isfinite(sourceAge) || !std::isfinite(remainingRows)
        || !std::isfinite(visibleRows) || sourceAge < 0.0f
        || remainingRows < 0.0f || visibleRows < 1.0f) {
        return std::nullopt;
    }
    return (sourceAge - remainingRows) / visibleRows;
}

inline float dssFlatOutlineAlphaScale(float colorStrength)
{
    if (!std::isfinite(colorStrength)) {
        return 1.0f;
    }
    constexpr float kFloorScale = 0.42f;
    constexpr float kTransitionStrength = 0.035f;
    const float t = std::clamp(
        colorStrength / kTransitionStrength, 0.0f, 1.0f);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return kFloorScale + (1.0f - kFloorScale) * smooth;
}

inline float dssRibbonCoverage(float coordinate)
{
    if (!std::isfinite(coordinate)) {
        return 0.0f;
    }
    constexpr float kSolidHalfWidth = 0.15f;
    constexpr float kRibbonHalfWidth = 1.0f;
    const float t = std::clamp(
        (std::abs(coordinate) - kSolidHalfWidth)
            / (kRibbonHalfWidth - kSolidHalfWidth),
        0.0f, 1.0f);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return 1.0f - smooth;
}

struct DssRibbonPixelOffset {
    float x{0.0f};
    float y{0.0f};
};

// Mirror of dss_mesh.vert's side-endpoint treatment. A perpendicular
// screen-space ribbon is correct in the trace interior, but its horizontal
// component makes the exposed outer endpoint crawl as the adjacent FFT slope
// changes. Endpoints use a fixed vertical butt boundary instead.
inline DssRibbonPixelOffset dssRibbonPixelOffset(float frequencyUnit,
                                                 float normalX,
                                                 float normalY,
                                                 float ribbonSide)
{
    if (!std::isfinite(frequencyUnit)
        || !std::isfinite(normalX) || !std::isfinite(normalY)
        || !std::isfinite(ribbonSide)) {
        return {};
    }
    if (frequencyUnit <= 0.0f || frequencyUnit >= 1.0f) {
        return DssRibbonPixelOffset{0.0f, ribbonSide};
    }
    return DssRibbonPixelOffset{
        normalX * ribbonSide,
        normalY * ribbonSide,
    };
}

inline float dssSideOutlineAlphaScale(float frequencyUnit,
                                      float plotWidthPx,
                                      float perspectiveWidth)
{
    if (!std::isfinite(frequencyUnit) || !std::isfinite(plotWidthPx)
        || !std::isfinite(perspectiveWidth)
        || plotWidthPx <= 0.0f || perspectiveWidth <= 0.0f) {
        return 0.0f;
    }
    constexpr float kFadeWidthPx = 8.0f;
    const float edgeDistancePx =
        std::max(0.0f, std::min(frequencyUnit, 1.0f - frequencyUnit))
        * plotWidthPx * perspectiveWidth;
    const float t = std::clamp(
        edgeDistancePx / kFadeWidthPx, 0.0f, 1.0f);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return smooth;
}

struct StablePresentationAnchor {
    float value{0.0f};
    float acquisitionMean{0.0f};
    int sampleCount{0};
    bool hasValue{false};
    bool locked{false};
};

inline bool observeStablePresentationAnchor(
    StablePresentationAnchor& anchor,
    float sample,
    int samplesToLock = 3,
    float quantum = 1.0f)
{
    if (!std::isfinite(sample) || samplesToLock <= 0
        || !std::isfinite(quantum) || quantum <= 0.0f) {
        return false;
    }
    if (anchor.locked) {
        return false;
    }

    if (!anchor.hasValue) {
        anchor.value = sample;
        anchor.acquisitionMean = sample;
        anchor.sampleCount = 1;
        anchor.hasValue = true;
    } else {
        anchor.sampleCount = std::min(anchor.sampleCount + 1, samplesToLock);
        anchor.acquisitionMean +=
            (sample - anchor.acquisitionMean)
            / static_cast<float>(anchor.sampleCount);
    }

    if (anchor.sampleCount >= samplesToLock) {
        anchor.value =
            std::round(anchor.acquisitionMean / quantum) * quantum;
        anchor.locked = true;
    }
    return true;
}

inline float stablePresentationValue(const StablePresentationAnchor& anchor,
                                     float fallback)
{
    if (anchor.hasValue && std::isfinite(anchor.value)) {
        return anchor.value;
    }
    return std::isfinite(fallback) ? fallback : 0.0f;
}

struct ObservedWaterfallCadence {
    std::int64_t lastRowTimestampMs{0};
    float msPerRow{100.0f};
    int sampleCount{0};
    bool valid{false};
};

inline bool observeWaterfallCadence(ObservedWaterfallCadence& cadence,
                                    std::int64_t rowTimestampMs)
{
    constexpr std::int64_t kMinIntervalMs = 10;
    constexpr std::int64_t kMaxIntervalMs = 15000;
    if (rowTimestampMs <= 0) {
        return false;
    }
    if (cadence.lastRowTimestampMs <= 0) {
        cadence.lastRowTimestampMs = rowTimestampMs;
        return false;
    }

    const std::int64_t intervalMs =
        rowTimestampMs - cadence.lastRowTimestampMs;
    if (intervalMs <= 0) {
        return false;
    }
    cadence.lastRowTimestampMs = rowTimestampMs;
    if (intervalMs < kMinIntervalMs || intervalMs > kMaxIntervalMs) {
        return false;
    }

    const float measuredMs = static_cast<float>(intervalMs);
    if (!cadence.valid) {
        cadence.msPerRow = measuredMs;
        cadence.sampleCount = 1;
        cadence.valid = true;
        return true;
    }

    // Track real arrival cadence while damping network jitter. Rate controls
    // explicitly reset the tracker, so the first interval at a new rate locks
    // immediately instead of being averaged against the previous setting.
    const float boundedMeasurement = std::clamp(
        measuredMs, cadence.msPerRow * 0.25f, cadence.msPerRow * 4.0f);
    const float alpha = cadence.sampleCount < 3 ? 0.5f : 0.2f;
    cadence.msPerRow += alpha * (boundedMeasurement - cadence.msPerRow);
    cadence.sampleCount = std::min(cadence.sampleCount + 1, 1000);
    return true;
}

inline bool observeWaterfallCadenceAndTimeScale(
    ObservedWaterfallCadence& cadence,
    StablePresentationAnchor& timeScaleAnchor,
    std::int64_t rowTimestampMs)
{
    if (!observeWaterfallCadence(cadence, rowTimestampMs)) {
        return false;
    }
    observeStablePresentationAnchor(timeScaleAnchor, cadence.msPerRow);
    return true;
}

inline float observedWaterfallMsPerRow(
    const ObservedWaterfallCadence& cadence,
    float fallbackMsPerRow)
{
    if (cadence.valid && std::isfinite(cadence.msPerRow)
        && cadence.msPerRow > 0.0f) {
        return cadence.msPerRow;
    }
    return std::isfinite(fallbackMsPerRow) && fallbackMsPerRow > 0.0f
        ? fallbackMsPerRow
        : 100.0f;
}

inline float selectedWaterfallMsPerRow(
    bool kiwiVisible,
    float flexMsPerRow,
    const ObservedWaterfallCadence* kiwiCadence,
    float kiwiFallbackMsPerRow = 100.0f)
{
    if (!kiwiVisible) {
        return std::isfinite(flexMsPerRow) && flexMsPerRow > 0.0f
            ? flexMsPerRow
            : 100.0f;
    }
    return kiwiCadence
        ? observedWaterfallMsPerRow(*kiwiCadence, kiwiFallbackMsPerRow)
        : observedWaterfallMsPerRow(
              ObservedWaterfallCadence{}, kiwiFallbackMsPerRow);
}

inline float selectedWaterfallTimeScaleMsPerRow(
    bool kiwiVisible,
    float flexMsPerRow,
    const StablePresentationAnchor* kiwiTimeScaleAnchor,
    float kiwiFallbackMsPerRow = 100.0f)
{
    if (!kiwiVisible) {
        return std::isfinite(flexMsPerRow) && flexMsPerRow > 0.0f
            ? flexMsPerRow
            : 100.0f;
    }
    if (!kiwiTimeScaleAnchor) {
        return std::isfinite(kiwiFallbackMsPerRow)
                && kiwiFallbackMsPerRow > 0.0f
            ? kiwiFallbackMsPerRow
            : 100.0f;
    }
    return stablePresentationValue(
        *kiwiTimeScaleAnchor, kiwiFallbackMsPerRow);
}

} // namespace AetherSDR
