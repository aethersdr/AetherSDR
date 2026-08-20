#include "core/backends/anan/AnanDroopCorrection.h"

#include <cstdio>

using namespace AetherSDR::anan;

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "anan_droop_correction_test: %s\n", message);
    return 1;
}

int testTableSizeForEveryValidRate()
{
    for (const int rateKsps : {48, 96, 192, 384, 768, 1536}) {
        const DroopCorrectionTable& table = droopCorrectionTableForRateKsps(rateKsps);
        if (table.size() != kDroopCorrectionFftSize)
            return fail("a valid rate did not return a full-size table");
    }
    return 0;
}

int testUnrecognizedRateReturnsSafeZeroFallback()
{
    // The safety-fallback contract is testable with certainty regardless of
    // whether real bench data has landed yet: an unrecognized rate must
    // never guess.
    const DroopCorrectionTable& table = droopCorrectionTableForRateKsps(9999);
    if (&table != &kDroopCorrectionZero)
        return fail("an unrecognized rate must return kDroopCorrectionZero");
    for (const float v : table) {
        if (v != 0.0f)
            return fail("kDroopCorrectionZero must be all-zero");
    }
    return 0;
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

}  // namespace

int main()
{
    if (const int result = testTableSizeForEveryValidRate(); result != 0)
        return result;
    if (const int result = testUnrecognizedRateReturnsSafeZeroFallback(); result != 0)
        return result;
    if (const int result = testApplyDroopCorrectionDbAddsElementwise(); result != 0)
        return result;
    if (const int result = testApplyDroopCorrectionDbNoOpOnSizeMismatch(); result != 0)
        return result;
    if (const int result = testApplyDroopCorrectionDbZeroFallbackIsNumericallyInert(); result != 0)
        return result;
    std::printf("anan_droop_correction_test: all checks passed\n");
    return 0;
}
