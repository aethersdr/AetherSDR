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
    renderer.setHistoryCapacityRows(8);
    renderer.appendHistoryRow(rowWithPeak(100), 14.0, 1.0, -200.0f);
    renderer.appendHistoryRow(rowWithPeak(220), 14.0, 1.0, -200.0f);
    renderer.appendHistoryRow(rowWithPeak(340), 14.0, 1.0, -200.0f);

    if (renderer.historyCapacityRows() != 8 || renderer.historyRowCount() != 3) {
        return fail("retained DSS history count/capacity is wrong");
    }

    renderer.rebuildVisibleFromHistory(0, 14.0, 1.0, -200.0f);
    if (std::abs(strongestBin(renderer) - 340) > 2) {
        return fail("offset 0 should rebuild the newest retained DSS row");
    }

    renderer.rebuildVisibleFromHistory(1, 14.0, 1.0, -200.0f);
    if (std::abs(strongestBin(renderer) - 220) > 2) {
        return fail("offset 1 should scroll DSS back with the waterfall");
    }

    return 0;
}

int testRetainedHistoryCapacity()
{
    DssRenderer renderer;
    renderer.setHistoryCapacityRows(2);
    renderer.appendHistoryRow(rowWithPeak(80), 14.0, 1.0, -200.0f);
    renderer.appendHistoryRow(rowWithPeak(180), 14.0, 1.0, -200.0f);
    renderer.appendHistoryRow(rowWithPeak(280), 14.0, 1.0, -200.0f);

    if (renderer.historyRowCount() != 2) {
        return fail("retained DSS history must stay bounded by capacity");
    }

    renderer.rebuildVisibleFromHistory(1, 14.0, 1.0, -200.0f);
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
    renderer.pushRow(rowWithPeak(128));
    renderer.appendCurrentRowToHistory(14.0, 1.0);

    if (renderer.historyCapacityRows() != 4 || renderer.historyRowCount() != 1) {
        return fail("moved-from DSS history storage should rebuild at the same capacity");
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

    return 0;
}
