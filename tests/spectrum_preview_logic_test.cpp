#include "gui/SpectrumPreviewLogic.h"
#include "gui/DssSupplementalCoverage.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "spectrum_preview_logic_test: %s\n", message);
    return 1;
}

bool nearlyEqual(double a, double b, double epsilon = 1.0e-12)
{
    return std::abs(a - b) <= epsilon;
}

int testDssRowSpanSupported()
{
    using namespace AetherSDR;

    // A build without AETHER_GPU_SPECTRUM has no mesh pipeline at all, so the
    // control can never affect the display whatever the runtime reports. This
    // is the case that shipped enabled: the only runtime correction lived
    // inside the GPU-only block, so the software build never corrected it and
    // the slider moved, labelled and persisted over a fallback that ignores it.
    if (dssRowSpanSupported(false, false) || dssRowSpanSupported(false, true)) {
        return fail("a CPU-only build must never offer the 3D span control");
    }

    // A GPU build still has to prove the mesh came up — RGBA16F support is a
    // runtime property, and without it the CPU image fallback draws instead.
    if (dssRowSpanSupported(true, false)) {
        return fail("a GPU build without the mesh must not offer the control");
    }
    if (!dssRowSpanSupported(true, true)) {
        return fail("the control must be offered once the mesh is ready");
    }
    return 0;
}

int testCursorAnchoredZoom()
{
    using namespace AetherSDR;
    const FrequencyFrame base{14.1, 0.2};
    constexpr std::array<double, 5> kFractions{
        -0.5, -0.25, 0.0, 0.31, 0.5,
    };
    constexpr std::array<double, 2> kBandwidths{0.08, 0.45};
    for (double fraction : kFractions) {
        const double anchorMhz = frequencyAtFraction(base, fraction);
        for (double bandwidthMhz : kBandwidths) {
            const double centerMhz = centerForAnchoredBandwidth(
                anchorMhz, fraction, bandwidthMhz);
            const FrequencyFrame zoomed{centerMhz, bandwidthMhz};
            if (!nearlyEqual(frequencyAtFraction(zoomed, fraction),
                             anchorMhz)) {
                return fail("cursor anchor moved during zoom");
            }
        }
    }
    if (!nearlyEqual(frequencyCanvasFraction(250.0, 1000), -0.25)
        || !nearlyEqual(frequencyCanvasFraction(-100.0, 1000), -0.5)
        || !nearlyEqual(frequencyCanvasFraction(1200.0, 1000), 0.5)) {
        return fail("frequency canvas fraction should clamp to the canvas");
    }
    return 0;
}

int testFrequencyFrameMapping()
{
    using namespace AetherSDR;
    const FrequencyFrame source{14.0, 0.2};
    const FrequencyFrame pannedTarget{14.05, 0.2};
    const FrequencyFrame zoomedTarget{14.0, 0.1};
    if (!nearlyEqual(sourceUnitPosition(0.5, source, pannedTarget), 0.75)
        || !nearlyEqual(sourceUnitPosition(0.5, source, zoomedTarget), 0.5)
        || !nearlyEqual(sourceUnitPosition(0.0, source, zoomedTarget), 0.25)
        || !nearlyEqual(sourceUnitPosition(1.0, source, zoomedTarget), 0.75)) {
        return fail("frequency frame mapping is incorrect");
    }
    if (sourceUnitPosition(1.0, source, FrequencyFrame{14.2, 0.2}) <= 1.0) {
        return fail("newly exposed frequency should map outside the retained frame");
    }
    const FrequencyFrame panTarget{14.05, 0.2};
    if (!nearlyEqual(sourceUnitPosition(0.5, source, panTarget), 0.75)
        || !nearlyEqual(
            sourceUnitPosition(0.5, panTarget, panTarget), 0.5)) {
        return fail("old and newly captured DSS rows must map independently");
    }

    const FrequencyFrame vhfBase{146.520000, 0.005};
    const FrequencyFrame vhfTarget{146.520375, 0.0025};
    const FrequencyPreviewTransform transform = frequencyPreviewTransform(
        vhfBase, vhfTarget);
    if (!transform.valid || !nearlyEqual(transform.scale, 0.5)
        || !nearlyEqual(transform.offset, 0.075, 1.0e-10)) {
        return fail("preview transform lost narrow-band VHF precision");
    }
    const double sourceU = sourceUnitPosition(0.37, vhfBase, vhfTarget);
    const double shaderEquivalent = 0.5 + transform.offset
        + (0.37 - 0.5) * transform.scale;
    if (!nearlyEqual(sourceU, shaderEquivalent, 1.0e-10)) {
        return fail("CPU and shader preview transforms diverged");
    }
    if (!std::isnan(sourceUnitPosition(
            0.5, FrequencyFrame{14.0, 0.0}, vhfTarget))) {
        return fail("invalid source frame should fail closed");
    }
    if (FrequencyFrame{std::numeric_limits<double>::infinity(), 0.2}.isValid()
        || FrequencyFrame{14.0, std::numeric_limits<double>::infinity()}
               .isValid()) {
        return fail("non-finite frequency frames should be rejected");
    }

    return 0;
}

int testUntaggedDssFrameResolution()
{
    using namespace AetherSDR;
    const FrequencyFrame requested{7.15, 0.1};
    const FrequencyFrame current{14.1, 0.2};
    const FrequencyFrame previewBase{14.1, 3.2};
    const FrequencyFrame zoomTarget{14.1, 0.2};
    const FrequencyFrame panTarget{14.2, 3.2};

    const FrequencyFrame explicitFrame = resolvedUntaggedDssFrame(
        requested, current, previewBase,
        dssUntaggedRowUsesPreviewBase(
            previewBase, zoomTarget, true));
    if (!nearlyEqual(explicitFrame.centerMhz, requested.centerMhz)
        || !nearlyEqual(explicitFrame.bandwidthMhz,
                        requested.bandwidthMhz)) {
        return fail("explicit DSS row frame must remain authoritative");
    }

    const FrequencyFrame previewFrame = resolvedUntaggedDssFrame(
        FrequencyFrame{}, current, previewBase,
        dssUntaggedRowUsesPreviewBase(
            previewBase, zoomTarget, true));
    if (!nearlyEqual(previewFrame.centerMhz, previewBase.centerMhz)
        || !nearlyEqual(previewFrame.bandwidthMhz,
                        previewBase.bandwidthMhz)) {
        return fail("untagged native preview row must retain the base frame");
    }

    const FrequencyFrame panningFrame = resolvedUntaggedDssFrame(
        FrequencyFrame{}, current, previewBase,
        dssUntaggedRowUsesPreviewBase(
            previewBase, panTarget, true));
    if (!nearlyEqual(panningFrame.centerMhz, current.centerMhz)
        || !nearlyEqual(panningFrame.bandwidthMhz,
                        current.bandwidthMhz)) {
        return fail("untagged DSS pan row must use the current frame");
    }

    const FrequencyFrame settledFrame = resolvedUntaggedDssFrame(
        FrequencyFrame{}, current, previewBase, false);
    if (!nearlyEqual(settledFrame.centerMhz, current.centerMhz)
        || !nearlyEqual(settledFrame.bandwidthMhz,
                        current.bandwidthMhz)) {
        return fail("settled untagged DSS row must use the current frame");
    }
    return 0;
}

int testCommandThrottle()
{
    using namespace AetherSDR;
    FrequencyRangeCommandThrottle throttle;
    const FrequencyRangeCommand first{14.0, 0.2};
    const FrequencyRangeCommand intermediate{14.01, 0.18};
    const FrequencyRangeCommand latest{14.02, 0.16};
    const FrequencyRangeCommand final{14.03, 0.14};

    const std::optional<FrequencyRangeCommand> emittedFirst =
        throttle.request(first, true, false);
    if (!emittedFirst.has_value() || throttle.hasPending()) {
        return fail("first due command should emit immediately");
    }
    if (throttle.request(intermediate, false, false).has_value()
        || throttle.request(latest, false, false).has_value()
        || !throttle.hasPending()) {
        return fail("intermediate commands should coalesce");
    }
    const std::optional<FrequencyRangeCommand> emittedFinal =
        throttle.request(final, false, true);
    if (!emittedFinal.has_value()
        || !nearlyEqual(emittedFinal->centerMhz, final.centerMhz)
        || throttle.hasPending()
        || throttle.takePending().has_value()) {
        return fail("forced release should emit once and cancel delayed output");
    }
    throttle.request(intermediate, false, false);
    throttle.request(latest, false, false);
    const std::optional<FrequencyRangeCommand> timeoutCommand =
        throttle.takePending();
    if (!timeoutCommand.has_value()
        || !nearlyEqual(timeoutCommand->centerMhz, latest.centerMhz)
        || throttle.hasPending()) {
        return fail("timeout should emit only the latest coalesced command");
    }
    if (throttle.request(FrequencyRangeCommand{
            std::numeric_limits<double>::quiet_NaN(), 0.2}, true, false)
            .has_value()) {
        return fail("invalid command should be rejected");
    }
    return 0;
}

int testDssZoomFloorSyncGate()
{
    using namespace AetherSDR;
    DssZoomFloorSyncGate gate;
    if (gate.bandwidthChangeQueued() || gate.waitingForFreshFrame()
        || gate.consumeFreshFrame(true)) {
        return fail("3D zoom floor synchronization should start idle");
    }

    gate.noteBandwidthChange();
    if (!gate.bandwidthChangeQueued() || gate.waitingForFreshFrame()
        || gate.consumeFreshFrame(true)) {
        return fail("3D zoom floor synchronization must wait for settle");
    }

    gate.settle(false);
    if (gate.bandwidthChangeQueued() || gate.waitingForFreshFrame()
        || gate.consumeFreshFrame(true)) {
        return fail("2D or Kiwi zoom must not arm a Flex 3D floor sync");
    }

    gate.noteBandwidthChange();
    gate.noteBandwidthChange();
    gate.settle(true);
    if (gate.bandwidthChangeQueued() || !gate.waitingForFreshFrame()
        || gate.consumeFreshFrame(false)) {
        return fail("coalesced 3D zoom must wait for a usable FFT frame");
    }
    if (!gate.beginAdjustment(2)
        || gate.adjustmentAttempts() != 1
        || !gate.waitingForFreshFrame()
        || !gate.beginAdjustment(2)
        || gate.beginAdjustment(2)
        || gate.adjustmentAttempts() != 2) {
        return fail("3D zoom range adjustments should be bounded and keep the gate armed");
    }
    if (!gate.consumeFreshFrame(true) || gate.waitingForFreshFrame()
        || gate.consumeFreshFrame(true)
        || gate.adjustmentAttempts() != 0) {
        return fail("only the first usable post-zoom FFT frame may resynchronize");
    }

    gate.noteBandwidthChange();
    gate.settle(true);
    gate.noteBandwidthChange();
    if (!gate.bandwidthChangeQueued() || gate.waitingForFreshFrame()) {
        return fail("a newer zoom must cancel an intermediate armed frame");
    }
    gate.clear();
    if (gate.bandwidthChangeQueued() || gate.waitingForFreshFrame()) {
        return fail("3D zoom floor synchronization should clear completely");
    }
    return 0;
}

int testDssFrameFloorClipDetection()
{
    using namespace AetherSDR;
    if (!dssFrameFloorLooksClipped(1600, 1558, 298)
        || dssFrameFloorLooksClipped(1600, 500, 19)
        || dssFrameFloorLooksClipped(768, 31, 31)
        || dssFrameFloorLooksClipped(63, 63, 63)) {
        return fail("3D floor clipping threshold is incorrect");
    }
    if (!flexFftFrameNeedsHeadroomRecovery(
            false, 1600, 1558, 298)
        || flexFftFrameNeedsHeadroomRecovery(
            true, 1600, 1558, 298)
        || flexFftFrameNeedsHeadroomRecovery(
            false, 1600, 500, 19)) {
        return fail(
            "FLEX floor recovery must cover both render modes but not Kiwi");
    }
    return 0;
}

int testDssZoomFloorFrameGuards()
{
    using namespace AetherSDR;

    // A frame with every window clear is the one the gate is waiting for.
    DssZoomFloorFrameGuards guards;
    guards.nowMs = 10'000;
    guards.notBeforeMs = 10'000;
    guards.postTxSettleMs = 400;
    if (!dssZoomFloorFrameTrusted(guards)) {
        return fail("a settled post-zoom frame must anchor the 3D floor");
    }

    // The radio needs ~100-300 ms to switch bandwidth. Frames before the hold
    // still carry the pre-zoom encoding; the drag-release path arms with no
    // delay of its own, so this is the only thing keeping them out.
    guards.nowMs = 9'999;
    if (dssZoomFloorFrameTrusted(guards)) {
        return fail("a frame before the bandwidth-switch hold must be rejected");
    }
    guards.nowMs = 10'000;

    // Post-TX AGC recovery (#2117): the first RX frame after unkey reads hot.
    guards.txEndMs = guards.nowMs - 399;
    if (dssZoomFloorFrameTrusted(guards)) {
        return fail("a post-TX AGC transient frame must not anchor the floor");
    }
    guards.txEndMs = guards.nowMs - 400;
    if (!dssZoomFloorFrameTrusted(guards)) {
        return fail("frames past the post-TX window are usable again");
    }
    guards.txEndMs = 0;

    // A y_pixels change still settling decodes bins against the old height.
    guards.scaleSettling = true;
    if (dssZoomFloorFrameTrusted(guards)) {
        return fail("a mis-decoded scale-settling frame must be rejected");
    }
    guards.scaleSettling = false;

    // An open dBm-range rebase can hand the sync reprojected preview bins.
    guards.rebaseActive = true;
    if (dssZoomFloorFrameTrusted(guards)) {
        return fail("reprojected preview bins must never anchor the floor");
    }
    guards.rebaseActive = false;

    // The operator owns the scale while dragging it.
    guards.draggingDbmScale = true;
    if (dssZoomFloorFrameTrusted(guards)) {
        return fail("a zoom sync must not fight an active dBm scale drag");
    }
    guards.draggingDbmScale = false;

    if (!dssZoomFloorFrameTrusted(guards)) {
        return fail("clearing every guard must restore a usable frame");
    }
    return 0;
}

int testWaterfallPipelineSelection()
{
    using namespace AetherSDR;
    WaterfallRowFrameReadiness readiness{
        true, true, true, true, true, true,
    };
    if (chooseWaterfallPipeline(readiness)
        != WaterfallPipelineMode::RowFrequencyFrames) {
        return fail("complete row-frame resources should select the GPU preview");
    }

    bool* stages[] = {
        &readiness.requested,
        &readiness.formatSupported,
        &readiness.textureCreated,
        &readiness.samplerCreated,
        &readiness.bindingsCreated,
        &readiness.pipelineCreated,
    };
    for (bool* stage : stages) {
        *stage = false;
        if (chooseWaterfallPipeline(readiness)
            != WaterfallPipelineMode::Legacy) {
            return fail("any row-frame failure must select the legacy pipeline");
        }
        *stage = true;
    }
    return 0;
}

int testWaterfallPaletteRecolorPlan()
{
    using namespace AetherSDR;
    const FrequencyFrame historyFrame{14.0, 0.2};
    const FrequencyFrame supplementalFrame{14.0, 0.8};
    const FrequencyFrame viewportFrame{14.05, 0.1};

    const WaterfallPaletteRecolorPlan nativePlan =
        waterfallPaletteRecolorPlan(
            WaterfallPipelineMode::RowFrequencyFrames,
            historyFrame, supplementalFrame, viewportFrame);
    if (!nativePlan.retainNativeFrames
        || !nearlyEqual(nativePlan.primaryFrame.centerMhz,
                        historyFrame.centerMhz)
        || !nearlyEqual(nativePlan.primaryFrame.bandwidthMhz,
                        historyFrame.bandwidthMhz)
        || !nearlyEqual(nativePlan.supplementalFrame.centerMhz,
                        supplementalFrame.centerMhz)
        || !nearlyEqual(nativePlan.supplementalFrame.bandwidthMhz,
                        supplementalFrame.bandwidthMhz)
        || !nearlyEqual(
            sourceUnitPosition(
                0.5, nativePlan.primaryFrame, viewportFrame),
            0.75)) {
        return fail(
            "row-frame recolor must retain matching native frame metadata");
    }

    const WaterfallPaletteRecolorPlan flattenedPlan =
        waterfallPaletteRecolorPlan(
            WaterfallPipelineMode::Legacy,
            historyFrame, supplementalFrame, viewportFrame);
    if (flattenedPlan.retainNativeFrames
        || !nearlyEqual(flattenedPlan.primaryFrame.centerMhz,
                        viewportFrame.centerMhz)
        || !nearlyEqual(flattenedPlan.primaryFrame.bandwidthMhz,
                        viewportFrame.bandwidthMhz)
        || flattenedPlan.supplementalFrame.isValid()
        || !nearlyEqual(
            sourceUnitPosition(
                0.5, flattenedPlan.primaryFrame, viewportFrame),
            0.5)) {
        return fail(
            "legacy recolor must label flattened pixels as the viewport frame");
    }

    const WaterfallPaletteRecolorPlan missingSupplementalPlan =
        waterfallPaletteRecolorPlan(
            WaterfallPipelineMode::RowFrequencyFrames,
            FrequencyFrame{}, FrequencyFrame{}, viewportFrame);
    if (!missingSupplementalPlan.retainNativeFrames
        || !nearlyEqual(missingSupplementalPlan.primaryFrame.centerMhz,
                        viewportFrame.centerMhz)
        || missingSupplementalPlan.supplementalFrame.isValid()) {
        return fail(
            "missing native frames must fall back without inventing coverage");
    }
    return 0;
}

// The property a palette recolour has to hold and a viewport rebuild does not:
// recolouring in place may not move, drop, or duplicate a single visible row.
// A rebuild gets away with re-laying the ring out from scanline 0 only because
// it repaints everything at once; a recolour keeps the live write row, so the
// age → scanline map is the whole correctness argument.
int testWaterfallVisibleRowForAge()
{
    using namespace AetherSDR;
    constexpr int kHeight = 7;

    // Age 0 is the newest row and sits on the origin itself — that is where
    // appendVisibleRow() last wrote, and where drawWaterfall() starts blitting.
    if (waterfallVisibleRowForAge(3, 0, kHeight) != 3
        || waterfallVisibleRowForAge(0, 0, kHeight) != 0) {
        return fail("age 0 must land on the write row");
    }

    // Older rows walk downward and wrap.
    if (waterfallVisibleRowForAge(5, 1, kHeight) != 6
        || waterfallVisibleRowForAge(5, 2, kHeight) != 0
        || waterfallVisibleRowForAge(5, 3, kHeight) != 1) {
        return fail("increasing age must walk down the ring and wrap");
    }

    // A full sweep covers every scanline exactly once, from any origin. This
    // is what "the waterfall cannot move under a palette change" reduces to.
    for (int origin = 0; origin < kHeight; ++origin) {
        bool seen[kHeight] = {};
        for (int age = 0; age < kHeight; ++age) {
            const int row = waterfallVisibleRowForAge(origin, age, kHeight);
            if (row < 0 || row >= kHeight || seen[row]) {
                return fail("a full age sweep must be a permutation of rows");
            }
            seen[row] = true;
        }
    }

    // Origin 0 is the rebuild path: age is the scanline, unchanged.
    for (int age = 0; age < kHeight; ++age) {
        if (waterfallVisibleRowForAge(0, age, kHeight) != age) {
            return fail("origin 0 must reproduce the plain rebuild layout");
        }
    }

    // m_wfWriteRow is decremented modulo the image height by appendVisibleRow,
    // but a restored stream state can carry one from a differently sized image.
    if (waterfallVisibleRowForAge(-2, 0, kHeight) != 5
        || waterfallVisibleRowForAge(kHeight + 2, 0, kHeight) != 2
        || waterfallVisibleRowForAge(1, -1, kHeight) != 0) {
        return fail("out-of-range origin and age must normalise, not index OOB");
    }

    if (waterfallVisibleRowForAge(0, 0, 0) != -1
        || waterfallVisibleRowForAge(0, 0, -4) != -1) {
        return fail("an empty image must report no destination row");
    }
    return 0;
}

int testWaterfallCubicSourceAgeBoundary()
{
    using namespace AetherSDR;
    constexpr int kHeight = 7;
    constexpr int kOrigin = 5;

    // The newest edge's leading cubic tap must duplicate age 0, not wrap to
    // age kHeight - 1 at the opposite end of the retained history.
    if (waterfallCubicPhysicalRowForSourceAge(kOrigin, -1, kHeight) != 5
        || waterfallCubicPhysicalRowForSourceAge(kOrigin, 0, kHeight) != 5
        || waterfallCubicPhysicalRowForSourceAge(kOrigin, 1, kHeight) != 6
        || waterfallCubicPhysicalRowForSourceAge(kOrigin, 2, kHeight) != 0) {
        return fail("waterfall cubic newest-edge taps must clamp logically");
    }

    // Likewise, the oldest edge's trailing taps duplicate the oldest row
    // instead of wrapping into the newest history.
    if (waterfallCubicPhysicalRowForSourceAge(kOrigin, kHeight - 2, kHeight)
            != 3
        || waterfallCubicPhysicalRowForSourceAge(kOrigin, kHeight - 1, kHeight)
               != 4
        || waterfallCubicPhysicalRowForSourceAge(kOrigin, kHeight, kHeight)
               != 4
        || waterfallCubicPhysicalRowForSourceAge(
               kOrigin, kHeight + 1, kHeight)
               != 4
        || waterfallCubicPhysicalRowForSourceAge(kOrigin, 0, 0) != -1) {
        return fail("waterfall cubic oldest-edge taps must clamp logically");
    }
    return 0;
}

int testWaterfallScrollProgress()
{
    using namespace AetherSDR;
    if (!nearlyEqual(waterfallScrollProgressRows(0, 40.0f), 0.0)
        || !nearlyEqual(waterfallScrollProgressRows(10, 40.0f), 0.25)
        || !nearlyEqual(waterfallScrollProgressRows(40, 40.0f), 1.0)
        || !nearlyEqual(waterfallScrollProgressRows(80, 40.0f), 1.0)
        || !nearlyEqual(waterfallScrollProgressRows(80, 40.0f, 3.0f), 2.0)
        || !nearlyEqual(waterfallScrollProgressRows(160, 40.0f, 3.0f), 3.0)) {
        return fail("waterfall scrolling should advance one row over one interval");
    }
    if (!nearlyEqual(waterfallScrollProgressRows(-1, 40.0f), 0.0)
        || !nearlyEqual(waterfallScrollProgressRows(10, 0.0f), 0.0)
        || !nearlyEqual(
            waterfallScrollProgressRows(
                10, std::numeric_limits<float>::quiet_NaN()),
            0.0)) {
        return fail("invalid waterfall timing should disable interpolation");
    }
    if (!nearlyEqual(
            waterfallScrollSampleOffsetUnit(0.0f, 1.0f, 100), 0.01, 1.0e-6)
        || !nearlyEqual(
            waterfallScrollSampleOffsetUnit(0.5f, 1.0f, 100), 0.005, 1.0e-6)
        || !nearlyEqual(
            waterfallScrollSampleOffsetUnit(1.0f, 1.0f, 100), 0.0, 1.0e-6)
        || !nearlyEqual(
            waterfallScrollSampleOffsetUnit(1.0f, 3.0f, 100), 0.02, 1.0e-6)
        || !nearlyEqual(
            waterfallScrollSampleOffsetUnit(0.5f, 1.0f, 0), 0.0, 1.0e-6)) {
        return fail("waterfall sample offset should preserve the row handoff");
    }
    return 0;
}

int testDssFftScaleSettleWindow()
{
    using namespace AetherSDR;
    if (!dssFftScaleSettleActive(1000, 1750)
        || dssFftScaleSettleActive(1750, 1750)
        || dssFftScaleSettleActive(1800, 1750)
        || dssFftScaleSettleActive(1000, 0)) {
        return fail("3D FFT scale settling must end exactly at its deadline");
    }
    return 0;
}

int testDssStartupHistoryAvailability()
{
    using namespace AetherSDR;
    constexpr float kRows = 96.0f;
    if (!nearlyEqual(dssHistoryAvailability(96.0f, 96.0f, kRows, 0.0f), 0.0)
        || !nearlyEqual(dssHistoryAvailability(95.0f, 96.0f, kRows, 0.0f), 1.0)
        || !nearlyEqual(dssHistoryAvailability(94.0f, 96.0f, kRows, 0.0f), 1.0)
        || !nearlyEqual(dssHistoryAvailability(0.0f, 96.0f, kRows, 0.0f), 1.0)) {
        return fail("3D FFT rear visibility must be binary on a full history");
    }
    // Non-finite input and an empty history both fail closed to hidden.
    if (!nearlyEqual(dssHistoryAvailability(
                         std::numeric_limits<float>::quiet_NaN(), 96.0f,
                         kRows, 0.0f),
                     0.0)
        || !nearlyEqual(dssHistoryAvailability(0.0f, 0.0f, kRows, 0.0f), 0.0)
        || !nearlyEqual(dssHistoryAvailability(0.0f, 96.0f, kRows,
                                              std::numeric_limits<float>::
                                                  quiet_NaN()),
                        0.0)) {
        return fail("3D FFT rear visibility must fail closed on bad input");
    }
    // Visibility belongs to the retained slot, not the scroll clock. This holds
    // both for a full ring and while the ring is still filling.
    for (const float remainingRows : {0.0f, 0.25f, 0.5f, 1.0f, 3.0f}) {
        if (!nearlyEqual(
                dssHistoryAvailability(95.0f, 96.0f, kRows, remainingRows),
                1.0)) {
            return fail("3D FFT rear visibility must not pulse with scroll "
                        "latency on a full history");
        }
    }
    constexpr float kFilling = 10.0f;   // 10 of 96 rows populated
    for (const float remainingRows : {0.0f, 0.5f, 1.0f, 3.0f}) {
        if (!nearlyEqual(
                dssHistoryAvailability(
                    9.0f, kFilling, kRows, remainingRows),
                1.0)
            || !nearlyEqual(
                dssHistoryAvailability(
                    10.0f, kFilling, kRows, remainingRows),
                0.0)) {
            return fail("3D FFT filling visibility must not pulse with scroll");
        }
    }

    for (const float sourceAge : {0.0f, 1.0f, 94.0f, 95.0f, 96.0f, 97.0f}) {
        const bool visible =
            dssHistoryAvailability(sourceAge, 96.0f, kRows, 0.0f) > 0.0f;
        const bool hasSample =
            dssRetainedSampleAge(sourceAge, 0.0f, 96.0f, kRows).has_value();
        if (visible != hasSample) {
            return fail("3D FFT rear visibility must agree with the height "
                        "path's valid-range early-out");
        }
    }

    // Every stored FFT is sampled at its exact age for every animation phase.
    // The old interpolation returned e.g. 17.5 here and visibly morphed peaks.
    for (const float sourceAge : {0.0f, 17.0f, 94.0f, 95.0f}) {
        for (const float remainingRows : {0.0f, 0.25f, 1.0f, 3.0f}) {
            const std::optional<float> sampleAge =
                dssRetainedSampleAge(
                    sourceAge, remainingRows, 96.0f, kRows);
            if (!sampleAge || !nearlyEqual(*sampleAge, sourceAge)) {
                return fail("3D FFT scroll must not interpolate peak height");
            }
        }
    }
    if (dssRetainedSampleAge(96.0f, 0.0f, 96.0f, kRows).has_value()
        || dssRetainedSampleAge(90.0f, 1.0f, 200.0f, kRows)
               != std::optional<float>{90.0f}) {
        return fail("3D FFT exact sample selection must honor valid-row bounds");
    }

    // The leading/trailing geometry is fixed. A head advance changes only which
    // two exact ages occupy those depths: the preceding interval's fully opaque
    // overlay is the following interval's base, representing the same FFT.
    for (const float sourceAge : {0.0f, 17.0f, 95.0f}) {
        for (const float distanceRows : {1.0f, 3.0f, 8.0f}) {
            const std::optional<DssFixedGridCrossfade> before =
                dssFixedGridCrossfade(
                    sourceAge, distanceRows, distanceRows);
            const std::optional<DssFixedGridCrossfade> after =
                dssFixedGridCrossfade(sourceAge, 0.0f, distanceRows);
            const std::optional<DssFixedGridCrossfade> halfway =
                dssFixedGridCrossfade(
                    sourceAge, distanceRows * 0.5f, distanceRows);
            if (!before || !after || !halfway
                || !nearlyEqual(before->overlayAge, sourceAge)
                || !nearlyEqual(before->baseAlpha, 0.0)
                || !nearlyEqual(before->overlayAlpha, 1.0)
                || !nearlyEqual(after->baseAge,
                                sourceAge + distanceRows)
                || !nearlyEqual(after->baseAlpha, 1.0)
                || !nearlyEqual(after->overlayAlpha, 0.0)
                || !nearlyEqual(halfway->baseAge,
                                sourceAge + distanceRows)
                || !nearlyEqual(halfway->overlayAge, sourceAge)
                || !nearlyEqual(halfway->baseAlpha, 0.5)
                || !nearlyEqual(halfway->overlayAlpha, 0.5)) {
                return fail("3D FFT fixed-grid crossfade ages are incorrect");
            }

            // Treat the ring head as a monotonically increasing generation.
            // The physical row identity is head - age, and must be identical
            // on both sides of the discrete head advance.
            const float physicalBefore = 0.0f - before->overlayAge;
            const float physicalAfter =
                distanceRows - after->baseAge;
            if (!nearlyEqual(physicalBefore, physicalAfter)) {
                return fail("3D FFT boundary rows must survive head advance");
            }
        }
    }
    if (const std::optional<DssFixedGridCrossfade> rear =
            dssFixedGridCrossfade(95.0f, 0.0f, 8.0f);
        !rear || rear->baseAge > 103.0f) {
        return fail("3D FFT transition reserve must cover the trailing row");
    }

    for (const float opacity : {0.12f, 0.37f, 0.62f}) {
        for (const float phase : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            const float baseAlpha =
                dssSourceOverCrossfadeLayerAlpha(
                    opacity, phase, false);
            const float overlayAlpha =
                dssSourceOverCrossfadeLayerAlpha(
                    opacity, phase, true);
            const float combinedAlpha =
                baseAlpha + overlayAlpha * (1.0f - baseAlpha);
            if (!nearlyEqual(combinedAlpha, opacity, 1.0e-6)) {
                return fail("3D FFT outline crossfade must preserve opacity");
            }
        }
    }

    // Coloured curtain geometry still moves, so the same physical row is
    // continuous across a multi-row head advance even though its source age
    // changes by that distance.
    for (const float sourceAge : {1.0f, 17.0f, 80.0f}) {
        for (const float distanceRows : {1.0f, 3.0f, 8.0f}) {
            const std::optional<float> before =
                dssMovingGeometryDepth(sourceAge, 0.0f, kRows);
            const std::optional<float> after =
                dssMovingGeometryDepth(
                    sourceAge + distanceRows, distanceRows, kRows);
            if (!before || !after || !nearlyEqual(*before, *after, 1.0e-7)) {
                return fail("3D FFT interior geometry must remain continuous");
            }
        }
    }
    if (!nearlyEqual(dssFlatOutlineAlphaScale(0.0f), 0.42, 1.0e-6)
        || !(dssFlatOutlineAlphaScale(0.0175f) > 0.42f)
        || !(dssFlatOutlineAlphaScale(0.0175f) < 1.0f)
        || !nearlyEqual(dssFlatOutlineAlphaScale(0.035f), 1.0, 1.0e-6)
        || !nearlyEqual(dssFlatOutlineAlphaScale(0.25f), 1.0, 1.0e-6)) {
        return fail("3D FFT floor-outline anti-shimmer curve is incorrect");
    }
    if (!nearlyEqual(dssRibbonCoverage(0.0f), 1.0, 1.0e-6)
        || !nearlyEqual(dssRibbonCoverage(0.15f), 1.0, 1.0e-6)
        || !nearlyEqual(dssRibbonCoverage(0.575f), 0.5, 1.0e-6)
        || !nearlyEqual(dssRibbonCoverage(-0.575f), 0.5, 1.0e-6)
        || !nearlyEqual(dssRibbonCoverage(1.0f), 0.0, 1.0e-6)
        || !nearlyEqual(dssRibbonCoverage(2.0f), 0.0, 1.0e-6)
        || !nearlyEqual(dssRibbonCoverage(
                            std::numeric_limits<float>::quiet_NaN()),
                        0.0, 1.0e-6)) {
        return fail("3D FFT ribbon coverage curve is incorrect");
    }
    const DssRibbonPixelOffset leftOffset =
        dssRibbonPixelOffset(0.0f, -0.6f, 0.8f, 1.0f);
    const DssRibbonPixelOffset rightOffset =
        dssRibbonPixelOffset(1.0f, 0.6f, 0.8f, -1.0f);
    const DssRibbonPixelOffset interiorOffset =
        dssRibbonPixelOffset(0.5f, -0.6f, 0.8f, -1.0f);
    const DssRibbonPixelOffset invalidOffset = dssRibbonPixelOffset(
        std::numeric_limits<float>::quiet_NaN(),
        -0.6f, 0.8f, 1.0f);
    if (!nearlyEqual(leftOffset.x, 0.0, 1.0e-6)
        || !nearlyEqual(leftOffset.y, 1.0, 1.0e-6)
        || !nearlyEqual(rightOffset.x, 0.0, 1.0e-6)
        || !nearlyEqual(rightOffset.y, -1.0, 1.0e-6)
        || !nearlyEqual(interiorOffset.x, 0.6, 1.0e-6)
        || !nearlyEqual(interiorOffset.y, -0.8, 1.0e-6)
        || !nearlyEqual(invalidOffset.x, 0.0, 1.0e-6)
        || !nearlyEqual(invalidOffset.y, 0.0, 1.0e-6)) {
        return fail("3D FFT side endpoints must use a stable butt boundary");
    }
    if (!nearlyEqual(
            dssSideOutlineAlphaScale(
                0.0f, 1000.0f, 1.0f), 0.0, 1.0e-6)
        || !nearlyEqual(
            dssSideOutlineAlphaScale(
                1.0f, 1000.0f, 0.5f), 0.0, 1.0e-6)
        || !nearlyEqual(
            dssSideOutlineAlphaScale(
                0.004f, 1000.0f, 1.0f), 0.5, 1.0e-5)
        || !nearlyEqual(
            dssSideOutlineAlphaScale(
                0.016f, 1000.0f, 0.5f), 1.0, 1.0e-6)
        || !nearlyEqual(
            dssSideOutlineAlphaScale(
                0.5f, 1000.0f, 1.0f), 1.0, 1.0e-6)
        || !nearlyEqual(
            dssSideOutlineAlphaScale(
                std::numeric_limits<float>::quiet_NaN(),
                1000.0f, 1.0f),
            0.0, 1.0e-6)) {
        return fail(
            "3D FFT side fade must affect only 8 pixels");
    }

    return 0;
}

int testDssOutlinePipelinePolicy()
{
    using namespace AetherSDR;
    if (dssOutlinePipelineModeForBackend(true)
            != DssOutlinePipelineMode::SharedFillPipeline
        || dssOutlinePipelineModeForBackend(false)
            != DssOutlinePipelineMode::DedicatedRibbonPipeline) {
        return fail("3D FFT outline pipeline policy is incorrect");
    }
    return 0;
}

// The policy above only names a mode; this pins the pipeline the outline draw
// actually binds, which is the half that can regress on its own. Renderer proof
// still comes from the three-backend manual verification — no headless test can
// reach a QRhi draw — but the selection itself no longer rests on a ternary
// duplicated at the draw site.
int testDssOutlinePipelineSelection()
{
    using namespace AetherSDR;
    struct FakePipeline {
        int id{0};
    };
    FakePipeline fill{1};
    FakePipeline dedicated{2};

    // OpenGL must land on the fill pipeline. That is what makes the rebind at
    // the outline draw a no-op, so no GL program switch separates it from the
    // fill draw — the whole point of the Linux fix.
    if (dssOutlinePipelineFor(dssOutlinePipelineModeForBackend(true), &fill,
                              &dedicated)
        != &fill) {
        return fail("OpenGL outline draw must reuse the fill pipeline");
    }
    // Metal/D3D11 keep the already-validated dedicated ribbon pipeline.
    if (dssOutlinePipelineFor(dssOutlinePipelineModeForBackend(false), &fill,
                              &dedicated)
        != &dedicated) {
        return fail("non-OpenGL outline draw must use the ribbon pipeline");
    }
    // initDssMeshPipeline never creates the dedicated pipeline on OpenGL, so
    // the shared-fill answer must survive a null one rather than disabling
    // outlines the way a `m_dssMeshLinePipeline`-based guard would.
    if (dssOutlinePipelineFor(DssOutlinePipelineMode::SharedFillPipeline, &fill,
                              static_cast<FakePipeline*>(nullptr))
        != &fill) {
        return fail("shared-fill outline selection must not need a ribbon pipeline");
    }
    return 0;
}

int testObservedWaterfallCadence()
{
    using namespace AetherSDR;
    ObservedWaterfallCadence cadence;
    StablePresentationAnchor timeScaleAnchor;
    if (observeWaterfallCadenceAndTimeScale(
            cadence, timeScaleAnchor, 1000)
        || cadence.valid
        || !nearlyEqual(observedWaterfallMsPerRow(cadence, 75.0f), 75.0)
        || !nearlyEqual(
            selectedWaterfallTimeScaleMsPerRow(
                true, 82.0f, &timeScaleAnchor, 75.0f),
            75.0)) {
        return fail("first waterfall row should only seed cadence timing");
    }
    if (!observeWaterfallCadenceAndTimeScale(
            cadence, timeScaleAnchor, 1100)
        || !cadence.valid
        || !nearlyEqual(cadence.msPerRow, 100.0)
        || timeScaleAnchor.locked
        || !nearlyEqual(
            selectedWaterfallTimeScaleMsPerRow(
                true, 82.0f, &timeScaleAnchor),
            100.0)) {
        return fail("second waterfall row should establish observed cadence");
    }
    if (!observeWaterfallCadenceAndTimeScale(
            cadence, timeScaleAnchor, 1220)
        || !nearlyEqual(cadence.msPerRow, 110.0)
        || timeScaleAnchor.locked
        || !nearlyEqual(
            selectedWaterfallTimeScaleMsPerRow(
                true, 82.0f, &timeScaleAnchor),
            100.0)) {
        return fail("waterfall cadence should smooth ordinary arrival jitter");
    }
    if (observeWaterfallCadenceAndTimeScale(
            cadence, timeScaleAnchor, 1220)
        || !nearlyEqual(cadence.msPerRow, 110.0)) {
        return fail("duplicate waterfall timestamps must not change cadence");
    }
    if (!observeWaterfallCadenceAndTimeScale(
            cadence, timeScaleAnchor, 1360)
        || !nearlyEqual(cadence.msPerRow, 125.0)
        || !timeScaleAnchor.locked
        || !nearlyEqual(
            selectedWaterfallTimeScaleMsPerRow(
                true, 82.0f, &timeScaleAnchor),
            112.0)) {
        return fail("Kiwi time scale should lock after startup acquisition");
    }
    if (!observeWaterfallCadenceAndTimeScale(
            cadence, timeScaleAnchor, 1560)
        || !nearlyEqual(cadence.msPerRow, 140.0)
        || !nearlyEqual(
            selectedWaterfallTimeScaleMsPerRow(
                true, 82.0f, &timeScaleAnchor),
            112.0)) {
        return fail("arrival jitter must not move the locked Kiwi time scale");
    }
    if (!nearlyEqual(
            selectedWaterfallMsPerRow(false, 82.0f, &cadence), 82.0)
        || !nearlyEqual(
            selectedWaterfallMsPerRow(true, 82.0f, &cadence), 140.0)
        || !nearlyEqual(
            selectedWaterfallTimeScaleMsPerRow(
                false, 82.0f, &timeScaleAnchor),
            82.0)
        || !nearlyEqual(
            selectedWaterfallMsPerRow(
                true, 82.0f, nullptr, 95.0f),
            95.0)
        || !nearlyEqual(
            selectedWaterfallTimeScaleMsPerRow(
                true, 82.0f, nullptr, 95.0f),
            95.0)) {
        return fail("Kiwi cadence selection must not alter Flex timing");
    }

    cadence = ObservedWaterfallCadence{};
    if (!nearlyEqual(
            selectedWaterfallTimeScaleMsPerRow(
                true, 82.0f, &timeScaleAnchor),
            112.0)) {
        return fail("Kiwi stream resets must preserve the locked time scale");
    }
    observeWaterfallCadence(cadence, 2000);
    if (!observeWaterfallCadence(cadence, 7000)
        || !nearlyEqual(cadence.msPerRow, 5000.0)) {
        return fail("slow Kiwi waterfall rates must retain their full interval");
    }
    return 0;
}

int testStablePresentationAnchor()
{
    using namespace AetherSDR;
    StablePresentationAnchor anchor;
    if (observeStablePresentationAnchor(
            anchor, std::numeric_limits<float>::quiet_NaN())
        || anchor.hasValue) {
        return fail("presentation anchors must reject invalid samples");
    }
    if (!observeStablePresentationAnchor(anchor, -101.1f, 3, 0.5f)
        || !anchor.hasValue
        || anchor.locked
        || !nearlyEqual(
            stablePresentationValue(anchor, -90.0f), -101.1, 1.0e-4)) {
        return fail("first presentation sample should establish a fixed provisional value");
    }
    if (!observeStablePresentationAnchor(anchor, -99.9f, 3, 0.5f)
        || anchor.locked
        || !nearlyEqual(
            stablePresentationValue(anchor, -90.0f), -101.1, 1.0e-4)) {
        return fail("presentation acquisition should not move the provisional axis");
    }
    if (!observeStablePresentationAnchor(anchor, -100.2f, 3, 0.5f)
        || !anchor.locked
        || !nearlyEqual(stablePresentationValue(anchor, -90.0f), -100.5)) {
        return fail("presentation anchor should lock to a quantized acquisition mean");
    }
    if (observeStablePresentationAnchor(anchor, -80.0f, 3, 0.5f)
        || !nearlyEqual(stablePresentationValue(anchor, -90.0f), -100.5)) {
        return fail("locked presentation anchors must ignore live measurement drift");
    }
    return 0;
}

int testDssSupplementalCoverageCalibration()
{
    std::vector<float> tile(128, 102.0f);
    std::vector<float> fft(64, -108.0f);
    const std::vector<float> supplemental =
        AetherSDR::buildDssSupplementalCoverage(
            tile, 14.0, 14.4,
            fft, 14.2, 0.2,
            -130.0f, -20.0f);
    if (supplemental.size() != tile.size()) {
        return fail("overlapping waterfall tile should produce DSS supplemental coverage");
    }
    for (const float dbm : supplemental) {
        if (!nearlyEqual(dbm, -108.0, 1.0e-5)) {
            return fail("supplemental tile should use the robust FFT overlap offset");
        }
    }

    // Waterfall intensities are arbitrary display units, not dBm.  In
    // particular, their signal span need not match the FFT span one-for-one.
    // Calibrate both the floor and span so a retained off-screen carrier does
    // not become taller than the same carrier once live FFT coverage arrives.
    std::fill(tile.begin() + 48, tile.begin() + 80, 142.0f);
    tile[8] = 142.0f; // same carrier level, outside the current FFT frame
    std::fill(fft.begin() + 16, fft.begin() + 48, -88.0f);
    const std::vector<float> scaled =
        AetherSDR::buildDssSupplementalCoverage(
            tile, 14.0, 14.4,
            fft, 14.2, 0.2,
            -130.0f, -20.0f);
    if (scaled.size() != tile.size()) {
        return fail("scaled waterfall tile should produce DSS supplemental coverage");
    }
    if (!nearlyEqual(scaled[16], -108.0, 0.25)
        || !nearlyEqual(scaled[64], -88.0, 0.25)
        || !nearlyEqual(scaled[8], -88.0, 0.25)) {
        return fail(
            "supplemental calibration must match the FFT floor and signal span");
    }

    const std::vector<float> disjoint =
        AetherSDR::buildDssSupplementalCoverage(
            tile, 13.0, 13.1,
            fft, 14.2, 0.2,
            -130.0f, -20.0f);
    if (!disjoint.empty()) {
        return fail("disjoint waterfall tiles must not invent DSS coverage");
    }
    return 0;
}

} // namespace

int main()
{
    if (const int result = testDssRowSpanSupported(); result != 0) {
        return result;
    }
    if (const int result = testCursorAnchoredZoom(); result != 0) {
        return result;
    }
    if (const int result = testFrequencyFrameMapping(); result != 0) {
        return result;
    }
    if (const int result = testUntaggedDssFrameResolution(); result != 0) {
        return result;
    }
    if (const int result = testCommandThrottle(); result != 0) {
        return result;
    }
    if (const int result = testDssZoomFloorSyncGate(); result != 0) {
        return result;
    }
    if (const int result = testDssFrameFloorClipDetection(); result != 0) {
        return result;
    }
    if (const int result = testDssZoomFloorFrameGuards(); result != 0) {
        return result;
    }
    if (const int result = testWaterfallPipelineSelection(); result != 0) {
        return result;
    }
    if (const int result = testWaterfallPaletteRecolorPlan(); result != 0) {
        return result;
    }
    if (const int result = testWaterfallVisibleRowForAge(); result != 0) {
        return result;
    }
    if (const int result = testWaterfallCubicSourceAgeBoundary(); result != 0) {
        return result;
    }
    if (const int result = testWaterfallScrollProgress(); result != 0) {
        return result;
    }
    if (const int result = testDssFftScaleSettleWindow(); result != 0) {
        return result;
    }
    if (const int result = testDssStartupHistoryAvailability(); result != 0) {
        return result;
    }
    if (const int result = testDssOutlinePipelinePolicy(); result != 0) {
        return result;
    }
    if (const int result = testDssOutlinePipelineSelection(); result != 0) {
        return result;
    }
    if (const int result = testObservedWaterfallCadence(); result != 0) {
        return result;
    }
    if (const int result = testDssSupplementalCoverageCalibration();
        result != 0) {
        return result;
    }
    return testStablePresentationAnchor();
}
