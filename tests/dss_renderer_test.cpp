#include "gui/DssRenderer.h"
#include "gui/DssDcEdgeMath.h"

#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "dss_renderer_test: %s\n", message);
    return 1;
}

int strongestBin(const DssRenderer& renderer)
{
    const float* row = renderer.rowDataRing(renderer.headRing());
    int strongest = 0;
    for (int i = 1; i < renderer.cols(); ++i) {
        if (row[i] > row[strongest]) {
            strongest = i;
        }
    }
    return strongest;
}

QVector<float> rowWithPeak(int bin)
{
    QVector<float> bins(DssRenderer::kCols, -120.0f);
    bins[std::clamp(bin, 0, DssRenderer::kCols - 1)] = -30.0f;
    return bins;
}

void appendStableHistoryPeak(DssRenderer& renderer, int bin, int count = 3)
{
    for (int i = 0; i < count; ++i) {
        renderer.appendHistoryRow(rowWithPeak(bin), 14.0, 1.0, -200.0f);
    }
}

int testFrequencyReprojection()
{
    DssRenderer renderer;
    QVector<float> bins(DssRenderer::kCols, -100.0f);
    bins[DssRenderer::kCols / 2] = -40.0f;
    renderer.pushRow(bins, 14.0, 1.0);
    if (std::abs(renderer.rowCenterMhzAtAge(0) - 14.0) > 1.0e-9
        || std::abs(renderer.rowBandwidthMhzAtAge(0) - 1.0) > 1.0e-9) {
        return fail("live DSS row must retain its capture frame");
    }

    const int beforeCount = renderer.rowCount();
    const quint64 beforeGeneration = renderer.rowGeneration();
    renderer.reprojectFrequencyFrame(
        14.0, 1.0,
        14.25, 1.0,
        -200.0f);

    if (renderer.rowCount() != beforeCount) {
        return fail("frequency reprojection must preserve DSS history rows");
    }
    if (renderer.rowGeneration() <= beforeGeneration) {
        return fail("frequency reprojection must mark rows changed for GPU upload");
    }

    const int expectedBin = DssRenderer::kCols / 4;
    const int actualBin = strongestBin(renderer);
    if (std::abs(actualBin - expectedBin) > 3) {
        return fail("frequency reprojection should shift history into the new viewport");
    }
    if (std::abs(renderer.rowCenterMhzAtAge(0) - 14.25) > 1.0e-9
        || std::abs(renderer.rowBandwidthMhzAtAge(0) - 1.0) > 1.0e-9) {
        return fail("reprojected DSS row must adopt its destination frame");
    }

    QVector<float> newFrame(DssRenderer::kCols, -72.0f);
    renderer.pushRow(newFrame, 14.5, 1.0);
    if (std::abs(
            renderer.rowDataRing(renderer.headRing())[DssRenderer::kCols - 1]
            - newFrame[0]) > 0.1f) {
        return fail("direct DSS reprojection must break the old temporal frame");
    }
    if (std::abs(renderer.rowCenterMhzAtAge(0) - 14.5) > 1.0e-9
        || std::abs(renderer.rowCenterMhzAtAge(1) - 14.25) > 1.0e-9) {
        return fail("adjacent DSS rows must keep independent capture frames");
    }

    return 0;
}

int testSupplementalCoverageDoesNotReplaceFft()
{
    DssRenderer renderer;
    DssRenderer baseline;
    QVector<float> fft(DssRenderer::kCols, -95.0f);
    fft[DssRenderer::kCols / 2] = -35.0f;
    QVector<float> supplemental(DssRenderer::kCols, -70.0f);
    baseline.pushRow(fft, 14.2, 0.2);
    renderer.pushRowWithSupplemental(
        fft, 14.2, 0.2,
        supplemental, 14.2, 0.4);

    const int ring = renderer.headRing();
    const int baselineRing = baseline.headRing();
    for (int column = 0; column < DssRenderer::kCols; ++column) {
        if (std::abs(
                renderer.rowDataRing(ring)[column]
                - baseline.rowDataRing(baselineRing)[column])
            > 0.001f) {
            return fail("supplemental coverage must not replace exact FFT bins");
        }
    }
    if (strongestBin(renderer) != strongestBin(baseline)) {
        return fail("supplemental coverage must not replace exact FFT bins");
    }
    if (std::abs(
            renderer.rowSupplementalDataRing(ring)[0] - (-70.0f))
        > 0.1f
        || renderer.rowSupplementalCoverageRing(ring)[0] == 0) {
        return fail("supplemental coverage should be retained in separate channels");
    }
    if (std::abs(renderer.rowSupplementalCenterMhzAtAge(0) - 14.2)
            > 1.0e-9
        || std::abs(renderer.rowSupplementalBandwidthMhzAtAge(0) - 0.4)
            > 1.0e-9) {
        return fail("supplemental coverage must retain its wider frequency frame");
    }
    return 0;
}

int testSupplementalCoverageRejectsTransientSpikes()
{
    DssRenderer renderer;
    QVector<float> fft(DssRenderer::kCols, -95.0f);
    QVector<float> supplemental(DssRenderer::kCols, -110.0f);
    for (int row = 0; row < 2; ++row) {
        renderer.pushRowWithSupplemental(
            fft, 14.2, 0.2,
            supplemental, 14.2, 0.4);
    }

    QVector<float> transient = supplemental;
    const int center = DssRenderer::kCols / 2;
    transient[center] = -20.0f;
    renderer.pushRowWithSupplemental(
        fft, 14.2, 0.2,
        transient, 14.2, 0.4);
    if (std::abs(
            renderer.rowSupplementalDataRing(renderer.headRing())[center]
            - (-110.0f))
        > 0.1f) {
        return fail(
            "supplemental DSS coverage must reject one-row tile spikes");
    }

    // A real carrier that persists for a second row must still emerge; the
    // filter rejects isolated maxima rather than flattening supplemental data.
    renderer.pushRowWithSupplemental(
        fft, 14.2, 0.2,
        transient, 14.2, 0.4);
    if (renderer.rowSupplementalDataRing(renderer.headRing())[center]
        <= -100.0f) {
        return fail(
            "persistent supplemental DSS peaks must survive spike rejection");
    }

    QVector<float> newFrame(DssRenderer::kCols, -75.0f);
    renderer.pushRowWithSupplemental(
        fft, 14.2, 0.2,
        newFrame, 14.5, 0.6);
    if (std::abs(
            renderer.rowSupplementalDataRing(renderer.headRing())[center]
            - (-75.0f))
        > 0.1f) {
        return fail(
            "supplemental DSS frame changes must break temporal smoothing");
    }
    return 0;
}

int testRetainedHistoryZoomRoundTrip()
{
    DssRenderer renderer;
    renderer.setHistoryCapacityRows(8);

    QVector<float> bins(DssRenderer::kCols, -120.0f);
    for (int i = 208; i < 304; ++i) {
        bins[i] = -65.0f + static_cast<float>(i % 11);
    }
    for (int i = 0; i < 6; ++i) {
        renderer.pushRow(bins);
        renderer.appendHistoryRow(bins, 14.0, 1.0, -200.0f);
    }

    std::array<float, DssRenderer::kCols> before{};
    const float* beforeRow = renderer.rowDataRing(renderer.headRing());
    std::copy_n(beforeRow, DssRenderer::kCols, before.begin());
    const int headBefore = renderer.headRing();
    const int countBefore = renderer.rowCount();

    // A direct compact-ring round trip is lossy: the 1 MHz source is reduced
    // to only a few columns in the 100 MHz view, then those columns are
    // magnified on the way back. Frame-stamped retained rows must remain the
    // source for both remaps so the original trace returns intact without
    // resetting the live ring or interrupting its scrolling phase.
    renderer.reprojectFrequencyFrame(
        14.0, 1.0, 14.0, 100.0, -200.0f, true);
    if (renderer.headRing() != headBefore || renderer.rowCount() != countBefore) {
        return fail("DSS zoom must preserve the live ring and scrolling phase");
    }
    const float* wideRow = renderer.rowDataRing(renderer.headRing());
    const quint8* wideCoverage =
        renderer.rowCoverageRing(renderer.headRing());
    if (wideCoverage[0] != 0
        || wideCoverage[DssRenderer::kCols - 1] != 0
        || wideCoverage[DssRenderer::kCols / 2] == 0
        || std::abs(wideRow[0] - (-200.0f)) > 0.1f
        || std::abs(wideRow[DssRenderer::kCols - 1] - (-200.0f)) > 0.1f) {
        return fail("DSS zoom gaps must retain visible floor samples with an "
                    "independent uncovered marker");
    }
    renderer.reprojectFrequencyFrame(
        14.0, 100.0, 14.0, 1.0, -200.0f, true);

    const float* afterRow = renderer.rowDataRing(renderer.headRing());
    const quint8* afterCoverage =
        renderer.rowCoverageRing(renderer.headRing());
    for (int i = 0; i < DssRenderer::kCols; ++i) {
        if (std::abs(afterRow[i] - before[i]) > 0.1f) {
            return fail("retained DSS history must survive an extreme zoom round trip");
        }
        if (afterCoverage[i] == 0) {
            return fail("retained DSS coverage must return after a zoom round trip");
        }
    }

    QVector<float> newFrame(DssRenderer::kCols, -74.0f);
    renderer.pushRow(newFrame);
    const float* firstNewRow = renderer.rowDataRing(renderer.headRing());
    if (std::abs(firstNewRow[0] - newFrame[0]) > 0.1f) {
        return fail("first post-zoom DSS row must not blend with the old frequency frame");
    }

    return 0;
}

int testRetainedHistoryFrameChangeBreaksTemporalBlend()
{
    DssRenderer renderer;
    renderer.setHistoryCapacityRows(8);

    QVector<float> strong(DssRenderer::kCols, -40.0f);
    for (int i = 0; i < 4; ++i) {
        renderer.appendHistoryRow(strong, 14.0, 1.0, -200.0f);
    }
    QVector<float> quiet(DssRenderer::kCols, -120.0f);
    renderer.appendHistoryRow(quiet, 14.0, 0.1, -200.0f);
    renderer.rebuildVisibleFromHistory(0, 14.0, 0.1, -200.0f);

    const float* row = renderer.rowDataRing(renderer.headRing());
    if (std::abs(row[DssRenderer::kCols / 2] - quiet[0]) > 0.1f) {
        return fail("retained DSS smoothing must stop across a frequency-frame change");
    }
    return 0;
}

int testRetainedHistoryOffset()
{
    DssRenderer renderer;
    renderer.setHistoryCapacityRows(12);
    appendStableHistoryPeak(renderer, 100);
    appendStableHistoryPeak(renderer, 220);
    appendStableHistoryPeak(renderer, 340);

    if (renderer.historyCapacityRows() != 12 || renderer.historyRowCount() != 9) {
        return fail("retained DSS history count/capacity is wrong");
    }

    renderer.rebuildVisibleFromHistory(0, 14.0, 1.0, -200.0f);
    if (std::abs(strongestBin(renderer) - 340) > 2) {
        return fail("offset 0 should rebuild the newest retained DSS row");
    }

    renderer.rebuildVisibleFromHistory(3, 14.0, 1.0, -200.0f);
    if (std::abs(strongestBin(renderer) - 220) > 2) {
        return fail("offset 3 should scroll DSS back with the waterfall");
    }

    return 0;
}

int testInputScaleResetPreservesHistory()
{
    DssRenderer renderer;
    renderer.setHistoryCapacityRows(12);
    appendStableHistoryPeak(renderer, 180);
    renderer.rebuildVisibleFromHistory(0, 14.0, 1.0, -200.0f);

    const int visibleRowsBefore = renderer.rowCount();
    const int historyRowsBefore = renderer.historyRowCount();
    const int peakBefore = strongestBin(renderer);
    renderer.resetInputSmoothing();

    if (renderer.rowCount() != visibleRowsBefore
        || renderer.historyRowCount() != historyRowsBefore
        || strongestBin(renderer) != peakBefore) {
        return fail("input scale reset must preserve decoded DSS history");
    }

    return 0;
}

int testInputScaleResetBreaksTemporalBlend()
{
    DssRenderer renderer;

    // Settle a strong peak into the live temporal filter so the retained newest
    // row carries it.
    const int bin = DssRenderer::kCols / 2;
    QVector<float> peak(DssRenderer::kCols, -120.0f);
    peak[bin] = 0.0f;
    for (int i = 0; i < 6; ++i) {
        renderer.pushRow(peak);
    }
    const float settledPeak = renderer.rowDataRing(renderer.headRing())[bin];
    if (settledPeak < -90.0f) {
        return fail("peak did not settle into the live row");
    }

    // A y_pixels scale change resets input smoothing but preserves history. The
    // first row pushed afterward must NOT blend against the retained (old-scale)
    // peak row — a flat floor row should collapse to the floor, not stay lifted
    // by the temporal IIR term.
    renderer.resetInputSmoothing();
    QVector<float> flat(DssRenderer::kCols, -120.0f);
    renderer.pushRow(flat);
    const float afterReset = renderer.rowDataRing(renderer.headRing())[bin];
    if (afterReset > -110.0f) {
        return fail("input scale reset must forget the temporal blend against "
                    "the old-scale row");
    }

    // The break is one-shot: a second flat row is identical (baseline sanity).
    renderer.pushRow(flat);
    const float secondRow = renderer.rowDataRing(renderer.headRing())[bin];
    if (secondRow > -110.0f) {
        return fail("post-reset rows should track the new-scale input");
    }

    return 0;
}

int testDepthShadowOcclusion()
{
    // y grows downward; rebuild() fills each curtain to the floor, so a nearer
    // (earlier) row hides anything below its ridge.

    // A surface receding upward: every row clears the one in front of it, so
    // nothing is occluded.
    if (dssDepthVisibleSegments({100.0, 90.0, 80.0, 70.0})
        != QVector<bool>{true, true, true}) {
        return fail("a strictly rising ridge must never be culled");
    }

    // The far rows sit below the front row's ridge: their segments are behind
    // the front curtain and must not be stroked over it.
    const QVector<bool> behind =
        dssDepthVisibleSegments({50.0, 120.0, 130.0, 140.0});
    if (behind.size() != 3 || !behind.at(0) || behind.at(1) || behind.at(2)) {
        return fail("segments behind a nearer ridge must be culled");
    }

    // A far peak that pokes back above the silhouette becomes visible again.
    const QVector<bool> reemerges =
        dssDepthVisibleSegments({50.0, 120.0, 40.0, 130.0});
    if (reemerges.size() != 3 || !reemerges.at(0) || !reemerges.at(1)
        || !reemerges.at(2)) {
        return fail("a far ridge rising above the silhouette must reappear");
    }

    // Degenerate inputs yield no segments rather than indexing off the end.
    if (!dssDepthVisibleSegments({}).isEmpty()
        || !dssDepthVisibleSegments({10.0}).isEmpty()) {
        return fail("fewer than two depths must yield no segments");
    }

    // Half-pixel slack: a ridge grazing the silhouette stays visible instead
    // of flickering between frames.
    if (dssDepthVisibleSegments({100.0, 100.2, 100.3})
        != QVector<bool>{true, true}) {
        return fail("a grazing ridge must not flicker out");
    }

    return 0;
}

int testRetainedHistoryCapacity()
{
    DssRenderer renderer;
    renderer.setHistoryCapacityRows(6);
    appendStableHistoryPeak(renderer, 80);
    appendStableHistoryPeak(renderer, 180);
    appendStableHistoryPeak(renderer, 280);

    if (renderer.historyRowCount() != 6) {
        return fail("retained DSS history must stay bounded by capacity");
    }

    renderer.rebuildVisibleFromHistory(3, 14.0, 1.0, -200.0f);
    if (std::abs(strongestBin(renderer) - 180) > 2) {
        return fail("retained DSS history should evict rows beyond capacity");
    }

    return 0;
}

int testEmptyHistoryRowsStayAligned()
{
    DssRenderer renderer;
    renderer.setHistoryCapacityRows(4);
    renderer.appendHistoryRow(QVector<float>{}, 14.0, 1.0, -177.0f);

    if (renderer.historyRowCount() != 1) {
        return fail("empty DSS input should still retain a baseline history row");
    }

    renderer.rebuildVisibleFromHistory(0, 14.0, 1.0, -177.0f);
    if (renderer.rowCount() != 1) {
        return fail("baseline DSS history row should rebuild as visible data");
    }

    return 0;
}

int testRetainedHistoryReprojection()
{
    DssRenderer renderer;
    renderer.setHistoryCapacityRows(4);
    renderer.appendHistoryRow(rowWithPeak(DssRenderer::kCols / 2),
                              14.0, 1.0, -200.0f);
    renderer.rebuildVisibleFromHistory(0, 14.25, 1.0, -200.0f);

    const int expectedBin = DssRenderer::kCols / 4;
    if (std::abs(strongestBin(renderer) - expectedBin) > 3) {
        return fail("retained DSS history should reproject into the current viewport");
    }

    return 0;
}

int testMovedFromHistoryCapacityRebuild()
{
    DssRenderer renderer;
    renderer.setHistoryCapacityRows(4);

    DssRenderer saved = std::move(renderer);
    (void)saved;

    renderer.setHistoryCapacityRows(4);
    renderer.appendHistoryRow(rowWithPeak(128), 14.0, 1.0, -200.0f);

    if (renderer.historyCapacityRows() != 4 || renderer.historyRowCount() != 1) {
        return fail("moved-from DSS history storage should rebuild at the same capacity");
    }

    return 0;
}

int testZeroCapacityReleasesRetainedHistory()
{
    DssRenderer renderer;
    renderer.setHistoryCapacityRows(24);
    renderer.appendHistoryRow(rowWithPeak(128), 14.0, 1.0, -200.0f);
    if (renderer.historyStorageBytes() == 0 || renderer.historyRowCount() != 1) {
        return fail("retained DSS history should allocate before release");
    }

    renderer.setHistoryCapacityRows(0);
    if (renderer.historyCapacityRows() != 0
        || renderer.historyRowCount() != 0
        || renderer.historyStorageBytes() != 0) {
        return fail("zero DSS history capacity must release all retained storage");
    }

    return 0;
}

int testRowPlateauStats()
{
    DssRenderer renderer;
    QVector<float> bins(DssRenderer::kCols, -110.0f);
    for (int i = 0; i < bins.size(); ++i) {
        bins[i] += static_cast<float>(i % 17) * 0.2f;
    }
    // Spatial smoothing softens one bin at each edge, leaving an exact
    // 40-bin plateau in the stored row.
    for (int i = 100; i < 142; ++i) {
        bins[i] = -120.0f;
    }
    renderer.pushRow(bins);

    const DssRenderer::RowStats stats = renderer.rowStats(0);
    if (stats.finiteBins != DssRenderer::kCols) {
        return fail("row stats should count every finite DSS bin");
    }
    if (stats.minValueBins != 40) {
        return fail("row stats should count bins clipped to the row minimum");
    }
    if (stats.longestFlatRunBins != 40) {
        return fail("row stats should report the longest localized flat run");
    }
    const DssRenderer::RowStats missing = renderer.rowStats(1);
    if (missing.finiteBins != 0 || missing.longestFlatRunBins != 0) {
        return fail("row stats should reject ages outside the visible history");
    }
    return 0;
}

int testDcEdgeSpikeFlattening()
{
    QVector<float> captured(2255, -68.0f);
    captured[0] = -48.0f;
    captured[1] = -48.0f;
    captured[2] = -56.0f;
    captured[3] = -66.0f;

    if (!AetherSDR::DssDcEdgeMath::viewStartsAtDc(0.4315127865,
                                                  0.8630255730)) {
        return fail("an exact zero-Hz low edge must enable DSS DC repair");
    }
    if (AetherSDR::DssDcEdgeMath::viewStartsAtDc(0.4485127865,
                                                 0.8630255730)) {
        return fail("a 17 kHz positive low edge must not enable DSS DC repair");
    }

    const QVector<float> repaired =
        AetherSDR::DssDcEdgeMath::flattenLeadingSpike(captured);
    for (int i = 0; i < 3; ++i) {
        if (std::abs(repaired[i] + 68.0f) > 0.01f) {
            return fail("captured leading DC spike bins must use the local baseline");
        }
    }
    if (std::abs(repaired[3] - captured[3]) > 0.01f) {
        return fail("DC repair must stop at the first non-outlier bin");
    }

    QVector<float> normal(2255, -68.0f);
    normal[0] = -63.0f;
    if (AetherSDR::DssDcEdgeMath::flattenLeadingSpike(normal) != normal) {
        return fail("normal leading FFT variation must remain unchanged");
    }
    return 0;
}

int testOverhangSurvivesUncoveredRows()
{
    // Reproduces the transmit case: the FFT-derived producer that paces rows
    // during TX appends with NO supplemental, so the front row loses its
    // overhang while the rest of the screen still has it. Driving the span off
    // the front row alone collapsed the whole surface on every over.
    constexpr double kTargetBw = 0.2;
    constexpr double kTileBw = 0.24;
    DssRenderer renderer;
    QVector<float> fft(DssRenderer::kCols, -95.0f);
    QVector<float> tile(DssRenderer::kCols, -100.0f);

    for (int i = 0; i < 8; ++i) {
        renderer.pushRowWithSupplemental(
            fft, 14.1, kTargetBw, tile, 14.1, kTileBw);
    }
    if (std::abs(renderer.newestSupplementalBandwidthMhz(kTargetBw) - kTileBw)
            > 1.0e-9) {
        return fail("a covered front row must report its own overhang");
    }

    // Key up: several supplemental-less rows arrive on top.
    for (int i = 0; i < 5; ++i) {
        renderer.pushRow(fft, 14.1, kTargetBw);
    }
    if (renderer.rowSupplementalBandwidthMhzAtAge(0) != 0.0) {
        return fail("the transmit-era front row should carry no overhang");
    }
    if (std::abs(renderer.newestSupplementalBandwidthMhz(kTargetBw) - kTileBw)
            > 1.0e-9) {
        return fail("overhang must survive uncovered rows at the front while "
                    "covered rows are still on screen");
    }

    // Once every covered row has scrolled out of the VISIBLE ring there really
    // is no overhang on screen, and reporting none is then correct.
    for (int i = 0; i < DssRenderer::kVisibleRows; ++i) {
        renderer.pushRow(fft, 14.1, kTargetBw);
    }
    if (renderer.newestSupplementalBandwidthMhz(kTargetBw) != 0.0) {
        return fail("overhang must lapse once no covered row remains visible");
    }

    // A tile no wider than the viewport is not an overhang, and a degenerate
    // target must not be divided by downstream.
    DssRenderer narrow;
    narrow.pushRowWithSupplemental(fft, 14.1, kTargetBw, tile, 14.1, kTargetBw);
    if (narrow.newestSupplementalBandwidthMhz(kTargetBw) != 0.0) {
        return fail("a tile no wider than the viewport is not an overhang");
    }
    if (renderer.newestSupplementalBandwidthMhz(0.0) != 0.0
        || renderer.newestSupplementalBandwidthMhz(
               std::numeric_limits<double>::quiet_NaN()) != 0.0) {
        return fail("a degenerate viewport width must report no overhang");
    }
    return 0;
}

int testRowSpanTaper()
{
    constexpr float kMax = DssRenderer::kMaxRowSpanFactor;
    const auto span = [](double supp, double target, int pct) {
        return DssRenderer::rowSpanFactorFor(supp, target, pct);
    };

    // A source with no usable overhang must keep the clipped trapezoid at EVERY
    // setting — the slider must never widen into spectrum never captured.
    for (const int pct : {0, 50, 100}) {
        if (span(0.2, 0.2, pct) != 1.0f        // exactly the viewport
            || span(0.1, 0.2, pct) != 1.0f     // narrower than the viewport
            || span(0.0, 0.2, pct) != 1.0f) {
            return fail("no overhang must keep the clipped trapezoid");
        }
    }

    // Degenerate frames must not widen either. A zero/negative target would
    // divide by zero and a NaN would poison the mesh uniform.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const int pct : {0, 100}) {
        if (span(0.23, 0.0, pct) != 1.0f
            || span(0.23, -0.2, pct) != 1.0f
            || span(0.23, nan, pct) != 1.0f
            || span(nan, 0.2, pct) != 1.0f
            || span(inf, 0.2, pct) != 1.0f
            || span(0.23, inf, pct) != 1.0f) {
            return fail("a degenerate frequency frame must not widen the mesh");
        }
    }

    // The percentage scales the AVAILABLE span, not the absolute maximum: with
    // a 1.2x tile, 100 must reach exactly 1.2 and 50 exactly halfway. An
    // absolute reading would put 50 at 1.333 and clamp it back to 1.2, which is
    // the dead-travel bug this mapping exists to avoid.
    if (std::abs(span(0.24, 0.2, 100) - 1.2f) > 0.0001f) {
        return fail("100% must spend the whole available overhang");
    }
    if (std::abs(span(0.24, 0.2, 50) - 1.1f) > 0.0001f) {
        return fail("50% must spend half the available overhang");
    }
    if (span(0.24, 0.2, 0) != 1.0f) {
        return fail("0% must restore the clipped trapezoid");
    }

    // Monotone in the setting, so every step of the control does something.
    float previous = span(0.24, 0.2, 0);
    for (int pct = 10; pct <= 100; pct += 10) {
        const float current = span(0.24, 0.2, pct);
        if (!(current > previous)) {
            return fail("every step of the span control must widen the surface");
        }
        previous = current;
    }

    // An overhang past the cap saturates rather than overhanging pointlessly.
    if (std::abs(span(2.0, 0.2, 100) - kMax) > 0.0001f) {
        return fail("an overhang beyond the cap must saturate");
    }

    // Out-of-range settings clamp rather than extrapolating past the cap or
    // inverting the surface.
    if (span(0.24, 0.2, -50) != 1.0f
        || std::abs(span(0.24, 0.2, 500) - 1.2f) > 0.0001f) {
        return fail("the span setting must clamp to 0..100");
    }

    return 0;
}

int testRowSpanGeometry()
{
    constexpr float kMax = DssRenderer::kMaxRowSpanFactor;

    // Span 1.0 is a strict identity: the CPU fallback and every existing caller
    // depend on today's rendering being untouched when no overhang exists.
    for (const float meshUnit : {0.0f, 0.25f, 0.5f, 1.0f}) {
        if (std::abs(DssRenderer::rowFrequencyUnit(meshUnit, 1.0f) - meshUnit)
                > 0.0001f) {
            return fail("span 1.0 must map mesh columns to themselves");
        }
    }
    for (const float depth : {0.0f, 0.5f, 1.0f}) {
        if (std::abs(DssRenderer::rowScreenCoverage(depth, 1.0f)
                     - DssRenderer::depthScale(depth)) > 0.0001f) {
            return fail("span 1.0 must leave row coverage untouched");
        }
    }
    if (DssRenderer::wedgeFreeDepth(1.0f) != 0.0f) {
        return fail("span 1.0 must close no part of the wedge");
    }

    // The point of the whole change: at the widest useful span the NEAR rows
    // overhang the plot and the DEEPEST row lands exactly on its edge, so no
    // depth is left with a wedge.
    if (!(DssRenderer::rowScreenCoverage(0.0f, kMax) > 1.0f)) {
        return fail("the front row must overhang the plot edges");
    }
    if (std::abs(DssRenderer::rowScreenCoverage(1.0f, kMax) - 1.0f) > 0.0001f) {
        return fail("the deepest row must land exactly on the plot edge");
    }
    if (std::abs(DssRenderer::wedgeFreeDepth(kMax) - 1.0f) > 0.0001f) {
        return fail("the widest span must close the wedge at every depth");
    }
    // Coverage must fall monotonically front to back and never dip below the
    // plot until the wedge-free depth is passed.
    float previous = DssRenderer::rowScreenCoverage(0.0f, kMax);
    for (int step = 1; step <= 20; ++step) {
        const float depth = static_cast<float>(step) / 20.0f;
        const float coverage = DssRenderer::rowScreenCoverage(depth, kMax);
        if (coverage > previous + 0.0001f) {
            return fail("row coverage must narrow with depth");
        }
        if (coverage < 1.0f - 0.0001f) {
            return fail("no row may fall inside the plot at the widest span");
        }
        previous = coverage;
    }

    // A partial overhang closes the wedge from the FRONT, and wedgeFreeDepth
    // must agree with where coverage actually crosses the plot edge.
    constexpr float kFlexSpan = 1.2f;   // a FLEX tile runs about 1.2x
    const float flexSpan = DssRenderer::rowSpanFactorForOverhang(kFlexSpan);
    if (std::abs(flexSpan - kFlexSpan) > 0.0001f) {
        return fail("an overhang below the cap should be used in full");
    }
    if (!(DssRenderer::rowScreenCoverage(0.0f, flexSpan) > 1.0f)
        || !(DssRenderer::rowScreenCoverage(1.0f, flexSpan) < 1.0f)) {
        return fail("a partial overhang must overhang the front and not the back");
    }
    const float crossing = DssRenderer::wedgeFreeDepth(flexSpan);
    if (!(crossing > 0.0f) || !(crossing < 1.0f)) {
        return fail("a partial overhang must close part of the wedge");
    }
    if (std::abs(DssRenderer::rowScreenCoverage(crossing, flexSpan) - 1.0f)
            > 0.0001f) {
        return fail("wedgeFreeDepth must be where coverage reaches the edge");
    }

    // Data past the cap is off-plot; taking it would only overhang further.
    if (std::abs(DssRenderer::rowSpanFactorForOverhang(4.0f) - kMax) > 0.0001f) {
        return fail("overhang beyond the cap must clamp");
    }
    // No overhang, or a degenerate one, must keep the clipped trapezoid rather
    // than widening into spectrum that was never captured.
    if (DssRenderer::rowSpanFactorForOverhang(1.0f) != 1.0f
        || DssRenderer::rowSpanFactorForOverhang(0.5f) != 1.0f
        || DssRenderer::rowSpanFactorForOverhang(
               std::numeric_limits<float>::quiet_NaN()) != 1.0f) {
        return fail("a missing overhang must keep the clipped trapezoid");
    }

    // The projection must be span-INDEPENDENT: widening changes what a row
    // covers, never where a frequency lands. This is what keeps the converging
    // slant, the frequency ruler and every marker exactly where they were.
    for (const float span : {1.0f, 1.2f, kMax}) {
        for (const float depth : {0.0f, 0.4f, 1.0f}) {
            constexpr float kSignalUnit = 0.3f;
            // Solve for the mesh column carrying this frequency, project it,
            // and it must land on the unwidened position.
            const float meshUnit = 0.5f + (kSignalUnit - 0.5f) / span;
            const float freqUnit =
                DssRenderer::rowFrequencyUnit(meshUnit, span);
            const double screenX =
                0.5 + (freqUnit - 0.5) * DssRenderer::depthScale(depth);
            if (std::abs(screenX
                         - DssRenderer::projectPerspective(
                               kSignalUnit, depth).x()) > 0.0001) {
                return fail("widening a row must not move an in-band signal");
            }
        }
    }

    return 0;
}

int testPerspectiveProjection()
{
    const QPointF frontLeft =
        DssRenderer::projectPerspective(0.0f, 0.0f);
    const QPointF backLeft =
        DssRenderer::projectPerspective(0.0f, 1.0f);
    const QPointF backCenter =
        DssRenderer::projectPerspective(0.5f, 1.0f);
    const QPointF backRight =
        DssRenderer::projectPerspective(1.0f, 1.0f);

    if (std::abs(frontLeft.x()) > 0.0001
        || std::abs(frontLeft.y() - 1.0) > 0.0001) {
        return fail("DSS perspective front edge must preserve frequency");
    }
    if (!(backLeft.x() > frontLeft.x())
        || !(backRight.x() < 1.0)
        || std::abs(backCenter.x() - 0.5) > 0.0001) {
        return fail("DSS perspective must converge toward the center");
    }
    if (std::abs(backLeft.x() - 0.2) > 0.0001
        || std::abs(backRight.x() - 0.8) > 0.0001
        || std::abs(backCenter.y()
                    - (1.0 - DssRenderer::kDepthSpanFrac)) > 0.0001) {
        return fail("DSS perspective projection drifted from renderer constants");
    }
    if (DssRenderer::projectPerspective(0.25f, -1.0f)
            != DssRenderer::projectPerspective(0.25f, 0.0f)
        || DssRenderer::projectPerspective(0.25f, 2.0f)
            != DssRenderer::projectPerspective(0.25f, 1.0f)) {
        return fail("DSS perspective depth must be clamped");
    }
    return 0;
}

int testSurfaceProjection()
{
    constexpr float floorDbm = -120.0f;
    constexpr float rangeDb = 80.0f;
    constexpr float zCurve = 0.7f;

    const QPointF baseline =
        DssRenderer::projectPerspective(0.25f, 0.0f);
    const QPointF floorPoint =
        DssRenderer::projectSurface(
            0.25f, 0.0f, floorDbm, floorDbm, rangeDb, zCurve);
    if (floorPoint != baseline) {
        return fail("a floor-level DSS surface point must stay on the baseline");
    }

    const QPointF peakPoint =
        DssRenderer::projectSurface(
            0.25f, 0.0f, floorDbm + rangeDb,
            floorDbm, rangeDb, zCurve);
    if (std::abs(
            peakPoint.y()
            - (baseline.y() - DssRenderer::kFrontMaxRidgeFrac)) > 0.0001) {
        return fail("a full-strength DSS surface point must reach maximum ridge height");
    }

    const QPointF raisedFloorPoint =
        DssRenderer::projectSurface(
            0.25f, 0.0f, -80.0f, -100.0f, rangeDb, zCurve);
    const QPointF loweredFloorPoint =
        DssRenderer::projectSurface(
            0.25f, 0.0f, -80.0f, -120.0f, rangeDb, zCurve);
    if (!(loweredFloorPoint.y() < raisedFloorPoint.y())) {
        return fail("moving the DSS floor must move projected surface height");
    }

    const QPointF farPeak =
        DssRenderer::projectSurface(
            0.25f, 1.0f, floorDbm + rangeDb,
            floorDbm, rangeDb, zCurve);
    const QPointF farBaseline =
        DssRenderer::projectPerspective(0.25f, 1.0f);
    const float expectedFarRidge =
        DssRenderer::kFrontMaxRidgeFrac * DssRenderer::kBackWidthFrac;
    if (std::abs(
            farPeak.y() - (farBaseline.y() - expectedFarRidge)) > 0.0001) {
        return fail("far DSS surface height must narrow with perspective");
    }

    const QPointF bandLeft =
        DssRenderer::projectSurface(
            0.40f, 0.25f, -72.0f, floorDbm, rangeDb, zCurve);
    const QPointF bandRight =
        DssRenderer::projectSurface(
            0.60f, 0.25f, -72.0f, floorDbm, rangeDb, zCurve);
    if (std::abs(bandLeft.y() - bandRight.y()) > 0.0001) {
        return fail("one DSS passband cross-section must have a level edge");
    }
    return 0;
}

} // namespace

int main()
{
    if (int rc = testFrequencyReprojection(); rc != 0) {
        return rc;
    }
    if (int rc = testSupplementalCoverageDoesNotReplaceFft(); rc != 0) {
        return rc;
    }
    if (int rc = testSupplementalCoverageRejectsTransientSpikes(); rc != 0) {
        return rc;
    }
    if (int rc = testRetainedHistoryZoomRoundTrip(); rc != 0) {
        return rc;
    }
    if (int rc = testRetainedHistoryFrameChangeBreaksTemporalBlend(); rc != 0) {
        return rc;
    }
    if (int rc = testRetainedHistoryOffset(); rc != 0) {
        return rc;
    }
    if (int rc = testInputScaleResetPreservesHistory(); rc != 0) {
        return rc;
    }
    if (int rc = testInputScaleResetBreaksTemporalBlend(); rc != 0) {
        return rc;
    }
    if (int rc = testDepthShadowOcclusion(); rc != 0) {
        return rc;
    }
    if (int rc = testRetainedHistoryCapacity(); rc != 0) {
        return rc;
    }
    if (int rc = testEmptyHistoryRowsStayAligned(); rc != 0) {
        return rc;
    }
    if (int rc = testRetainedHistoryReprojection(); rc != 0) {
        return rc;
    }
    if (int rc = testMovedFromHistoryCapacityRebuild(); rc != 0) {
        return rc;
    }
    if (int rc = testZeroCapacityReleasesRetainedHistory(); rc != 0) {
        return rc;
    }
    if (int rc = testRowPlateauStats(); rc != 0) {
        return rc;
    }
    if (int rc = testDcEdgeSpikeFlattening(); rc != 0) {
        return rc;
    }
    if (int rc = testOverhangSurvivesUncoveredRows(); rc != 0) {
        return rc;
    }
    if (int rc = testRowSpanTaper(); rc != 0) {
        return rc;
    }
    if (int rc = testRowSpanGeometry(); rc != 0) {
        return rc;
    }
    if (int rc = testPerspectiveProjection(); rc != 0) {
        return rc;
    }
    if (int rc = testSurfaceProjection(); rc != 0) {
        return rc;
    }

    return 0;
}
