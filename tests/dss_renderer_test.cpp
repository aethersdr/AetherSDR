#include "gui/DssRenderer.h"
#include "gui/DssDcEdgeMath.h"

#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    renderer.pushRow(bins);

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
    if (int rc = testPerspectiveProjection(); rc != 0) {
        return rc;
    }
    if (int rc = testSurfaceProjection(); rc != 0) {
        return rc;
    }

    return 0;
}
