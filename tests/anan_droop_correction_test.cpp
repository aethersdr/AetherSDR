#include "core/backends/anan/AnanDroopCorrection.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace AetherSDR::anan;

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "anan_droop_correction_test: %s\n", message);
    return 1;
}

int testApplyDroopCorrectionDbAddsElementwise()
{
    DroopCorrectionTable table{};
    table.fill(2.5f);
    std::vector<float> bins(kDroopCorrectionFftSize, -10.0f);
    applyDroopCorrectionDb(bins, table);
    for (const float v : bins) {
        if (v != -7.5f)
            return fail("expected -10.0f + 2.5f == -7.5f elementwise");
    }
    return 0;
}

int testApplyDroopCorrectionDbNoOpOnSizeMismatch()
{
    DroopCorrectionTable table{};
    table.fill(99.0f);
    std::vector<float> bins(kDroopCorrectionFftSize - 1, -10.0f);   // wrong size
    const std::vector<float> before = bins;
    applyDroopCorrectionDb(bins, table);
    if (bins != before)
        return fail("a size mismatch must leave binsDbfs byte-for-byte unchanged");
    return 0;
}

int testApplyDroopCorrectionDbZeroFallbackIsNumericallyInert()
{
    std::vector<float> bins(kDroopCorrectionFftSize);
    for (std::size_t i = 0; i < bins.size(); ++i)
        bins[i] = static_cast<float>(i) - 500.0f;
    const std::vector<float> before = bins;
    applyDroopCorrectionDb(bins, kDroopCorrectionZero);
    if (bins != before)
        return fail("the all-zero fallback table must not change any bin");
    return 0;
}

int testDroopCorrectionZeroIsAllZero()
{
    for (const float v : kDroopCorrectionZero) {
        if (v != 0.0f)
            return fail("kDroopCorrectionZero must be all-zero");
    }
    return 0;
}

int testApplyEdgeFadeLeavesTheMiddleUntouched()
{
    std::vector<float> bins(kDroopCorrectionFftSize, -110.0f);
    const std::vector<float> before = bins;
    applyEdgeFade(bins);   // default tailFraction=0.03f -> 30 bins/side at 1024
    for (std::size_t i = 30; i < bins.size() - 30; ++i) {
        if (bins[i] != before[i])
            return fail("applyEdgeFade must not touch bins outside the tail zone");
    }
    return 0;
}

int testApplyEdgeFadeIsContinuousAtTheBoundary()
{
    std::vector<float> bins(kDroopCorrectionFftSize);
    for (std::size_t i = 0; i < bins.size(); ++i)
        bins[i] = -110.0f - 0.01f * static_cast<float>(i % 7);   // mild per-bin texture
    applyEdgeFade(bins);
    // Bin 29 (just inside the tail zone, adjacent to the untouched boundary
    // at bin 30) must land close to the boundary value -- the whole point
    // of the raised-cosine window is zero slope at that seam.
    if (std::fabs(bins[29] - bins[30]) > 0.5f)
        return fail("left tail must blend smoothly into the untouched boundary bin");
    if (std::fabs(bins[bins.size() - 30] - bins[bins.size() - 31]) > 0.5f)
        return fail("right tail must blend smoothly into the untouched boundary bin");
    return 0;
}

int testApplyEdgeFadeReachesFullFadeAtTheTrueEdge()
{
    std::vector<float> bins(kDroopCorrectionFftSize, -100.0f);
    applyEdgeFade(bins, 0.03f, 12.0f);
    const float boundary = -100.0f;   // untouched, so still the input value
    if (std::fabs(bins[0] - (boundary - 12.0f)) > 1.0e-3f)
        return fail("bin 0 must land exactly boundary - fadeDb at the true edge");
    if (std::fabs(bins[bins.size() - 1] - (boundary - 12.0f)) > 1.0e-3f)
        return fail("the last bin must land exactly boundary - fadeDb at the true edge");
    return 0;
}

int testApplyEdgeFadeIsMonotonicTowardTheEdge()
{
    std::vector<float> bins(kDroopCorrectionFftSize, -100.0f);
    applyEdgeFade(bins);
    for (std::size_t i = 1; i < 30; ++i) {
        if (bins[i] < bins[i - 1])
            return fail("the left fade must not dip below a bin closer to the true edge");
    }
    return 0;
}

int testApplyEdgeFadeNoOpOnTooSmallAnArray()
{
    std::vector<float> bins(4, -100.0f);
    const std::vector<float> before = bins;
    applyEdgeFade(bins, 0.5f, 12.0f);   // tailFraction*2 would exceed the array
    if (bins != before)
        return fail("applyEdgeFade must be a no-op when the array is too small for the tail width");
    return 0;
}


// A straight ramp over the table, so the value at any fraction of the span
// is known exactly: t[j] = 0.5 * j.
DroopCorrectionTable rampTable()
{
    DroopCorrectionTable table{};
    for (std::size_t j = 0; j < table.size(); ++j)
        table[j] = 0.5f * static_cast<float>(j);
    return table;
}

int testResampledApplyIsExactAtTheTableSize()
{
    const DroopCorrectionTable table = rampTable();
    std::vector<float> exact(kDroopCorrectionFftSize, -10.0f);
    std::vector<float> resampled = exact;
    applyDroopCorrectionDb(exact, table);
    applyDroopCorrectionDbResampled(resampled, table);
    if (resampled != exact)
        return fail("at kDroopCorrectionFftSize points the resampled apply must equal the exact one");
    return 0;
}

int testResampledApplyFollowsTheCurveAtOtherCounts()
{
    // 1840 is a 2000-pixel panel's kept span; 3000 is wider than the table.
    const DroopCorrectionTable table = rampTable();
    for (const std::size_t n : {std::size_t{1840}, std::size_t{3000}, std::size_t{700}}) {
        std::vector<float> points(n, 0.0f);
        applyDroopCorrectionDbResampled(points, table);
        for (std::size_t i = 0; i < n; ++i) {
            const double want = 0.5 * static_cast<double>(i) * (kDroopCorrectionFftSize - 1)
                / static_cast<double>(n - 1);
            if (std::fabs(points[i] - want) > 1.0e-3)
                return fail("a resampled correction must read the table at the same fraction of the span");
        }
        if (points.front() != table.front() || points.back() != table.back())
            return fail("the resampled correction must pin both span edges to the table's ends");
    }
    return 0;
}

int testResampleToDroopGridReadsAnyCountOntoTheTableGrid()
{
    // A frame that is a straight line across the span, at a count that is
    // not the table's: the table grid must see the same line.
    const std::size_t n = 2500;
    std::vector<float> frame(n);
    for (std::size_t i = 0; i < n; ++i)
        frame[i] = -100.0f + 50.0f * static_cast<float>(i) / static_cast<float>(n - 1);
    DroopCorrectionTable out{};
    if (!resampleToDroopGrid(frame, out))
        return fail("resampleToDroopGrid must accept a 2500-point frame");
    for (std::size_t k = 0; k < out.size(); ++k) {
        const double want = -100.0 + 50.0 * static_cast<double>(k) / (kDroopCorrectionFftSize - 1);
        if (std::fabs(out[k] - want) > 1.0e-3)
            return fail("resampleToDroopGrid must read the frame at the same fraction of the span");
    }

    // At the table's own size it is a straight copy.
    std::vector<float> same(kDroopCorrectionFftSize);
    for (std::size_t i = 0; i < same.size(); ++i)
        same[i] = static_cast<float>(i) * 0.25f - 7.0f;
    if (!resampleToDroopGrid(same, out))
        return fail("resampleToDroopGrid must accept a kDroopCorrectionFftSize frame");
    for (std::size_t k = 0; k < out.size(); ++k) {
        if (out[k] != same[k])
            return fail("at kDroopCorrectionFftSize points resampleToDroopGrid must copy exactly");
    }

    // Too short to resample: refused, output untouched.
    const DroopCorrectionTable before = out;
    if (resampleToDroopGrid(std::vector<float>(1, 3.0f), out) || out != before)
        return fail("a one-point frame must be refused and leave the output untouched");
    return 0;
}

int testResampleToDroopGridAveragesAWiderFrame()
{
    // Twice the table's points, alternating +1/-1: noise the table grid
    // cannot resolve. Averaged over each table cell it cancels; sampled at
    // the cell centre it would come back at up to full size.
    std::vector<float> frame(2 * kDroopCorrectionFftSize);
    for (std::size_t i = 0; i < frame.size(); ++i)
        frame[i] = (i % 2 == 0) ? 1.0f : -1.0f;
    DroopCorrectionTable out{};
    if (!resampleToDroopGrid(frame, out))
        return fail("resampleToDroopGrid must accept a 2048-point frame");
    // The two end points have no room for a cell and are sampled as they are.
    float worst = 0.0f;
    for (std::size_t k = 1; k + 1 < out.size(); ++k)
        worst = std::max(worst, std::fabs(out[k]));
    std::printf("anan_droop_correction_test: alternating 2048-point frame, "
                "largest table value %.4f\n", static_cast<double>(worst));
    if (worst > 0.01f)
        return fail("a frame wider than the table must be averaged over each table cell, "
                    "not sampled at its centre");
    return 0;
}

}  // namespace

int main()
{
    if (const int result = testApplyDroopCorrectionDbAddsElementwise(); result != 0)
        return result;
    if (const int result = testApplyDroopCorrectionDbNoOpOnSizeMismatch(); result != 0)
        return result;
    if (const int result = testApplyDroopCorrectionDbZeroFallbackIsNumericallyInert(); result != 0)
        return result;
    if (const int result = testDroopCorrectionZeroIsAllZero(); result != 0)
        return result;
    if (const int result = testApplyEdgeFadeLeavesTheMiddleUntouched(); result != 0)
        return result;
    if (const int result = testApplyEdgeFadeIsContinuousAtTheBoundary(); result != 0)
        return result;
    if (const int result = testApplyEdgeFadeReachesFullFadeAtTheTrueEdge(); result != 0)
        return result;
    if (const int result = testApplyEdgeFadeIsMonotonicTowardTheEdge(); result != 0)
        return result;
    if (const int result = testApplyEdgeFadeNoOpOnTooSmallAnArray(); result != 0)
        return result;
    if (const int result = testResampledApplyIsExactAtTheTableSize(); result != 0)
        return result;
    if (const int result = testResampledApplyFollowsTheCurveAtOtherCounts(); result != 0)
        return result;
    if (const int result = testResampleToDroopGridReadsAnyCountOntoTheTableGrid(); result != 0)
        return result;
    if (const int result = testResampleToDroopGridAveragesAWiderFrame(); result != 0)
        return result;
    std::printf("anan_droop_correction_test: all checks passed\n");
    return 0;
}
