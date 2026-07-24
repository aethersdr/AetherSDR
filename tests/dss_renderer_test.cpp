#include "gui/DssRenderer.h"

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

} // namespace

int main()
{
    if (int rc = testFrequencyReprojection(); rc != 0) {
        return rc;
    }
    if (int rc = testRetainedHistoryOffset(); rc != 0) {
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

    return 0;
}
