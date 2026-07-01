#include "gui/DssRenderer.h"

#include <QVector>

#include <cmath>
#include <cstdio>

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

} // namespace

int main()
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
