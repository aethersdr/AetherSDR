#include "core/backends/anan/AnanDroopCorrection.h"

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
    std::printf("anan_droop_correction_test: all checks passed\n");
    return 0;
}
