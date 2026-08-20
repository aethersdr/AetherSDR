// Pure-math unit tests for AnanDroopCalibrator -- no live radio needed.
// Ports the test cases from this feature's original offline prototype
// (formerly tools/test_anan_droop_calibration.py, since superseded by this
// in-app engine): outlier rejection via median, central-window reference,
// and clamp behavior at the cap.

#include "core/backends/anan/AnanDroopCalibrator.h"

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "anan_droop_calibrator_test: %s\n", message);
    return 1;
}

bool nearlyEqual(float a, float b, float epsilon = 1.0e-4f)
{
    return std::fabs(a - b) <= epsilon;
}

int testMedianPowerCurveRejectsAStrayOutlierCapture()
{
    // Bin 0: all five captures agree (-10 dB). Bin 1: four quiet captures
    // (-10 dB) and one huge outlier (+40 dB, e.g. a stray signal during a
    // live-antenna sweep) -- the median must ignore the outlier entirely,
    // which a mean could not.
    AnanDroopCalibrator::Curve quiet{};
    quiet.fill(-10.0f);
    AnanDroopCalibrator::Curve outlier = quiet;
    outlier[1] = 40.0f;

    QVector<AnanDroopCalibrator::Curve> captures{quiet, quiet, quiet, quiet, outlier};
    const AnanDroopCalibrator::Curve curve = AnanDroopCalibrator::medianPowerCurve(captures);

    if (!nearlyEqual(curve[0], -10.0f))
        return fail("bin 0 should stay -10 dB");
    if (!nearlyEqual(curve[1], -10.0f))
        return fail("bin 1 median should reject the outlier");
    return 0;
}

int testMedianPowerCurvePreservesDeepDroopValues()
{
    // Regression for a real bug: the log10(0) safety floor was originally
    // 1e-12 (-120 dB), chosen as "an obviously tiny epsilon" without
    // accounting for this radio's actual dynamic range -- bench-measured
    // edge droop legitimately reads -160 to -180 dBm, whose linear power is
    // SMALLER than that floor, so every deep-droop bin was silently clamped
    // UP to exactly -120 dB, destroying the very signal being measured (the
    // droop tables came out all-zero at the narrower rates as a result).
    // A bin whose captures agree on a much deeper value than -120 dB must
    // come back near that deep value, not floored.
    AnanDroopCalibrator::Curve deep{};
    deep.fill(-165.0f);
    QVector<AnanDroopCalibrator::Curve> captures{deep, deep, deep, deep, deep};
    const AnanDroopCalibrator::Curve curve = AnanDroopCalibrator::medianPowerCurve(captures);
    if (!nearlyEqual(curve[0], -165.0f, 0.5f))
        return fail("a consistent -165 dB bin must not be floored to -120 dB");
    return 0;
}

int testMedianPowerCurveEmptyIsAllZero()
{
    const AnanDroopCalibrator::Curve curve =
        AnanDroopCalibrator::medianPowerCurve(QVector<AnanDroopCalibrator::Curve>{});
    for (const float v : curve) {
        if (v != 0.0f)
            return fail("an empty capture set must produce an all-zero curve");
    }
    return 0;
}

int testReferenceLevelUsesCentralWindow()
{
    // Flat at 0 dB in the center, drooping hard at both edges -- the
    // reference must come from the flat center, not be dragged down by the
    // edges.
    AnanDroopCalibrator::Curve curve{};
    curve.fill(0.0f);
    for (int k = 0; k < 60; ++k) {
        curve[static_cast<std::size_t>(k)] = -50.0f;
        curve[curve.size() - 1 - static_cast<std::size_t>(k)] = -50.0f;
    }
    const float ref = AnanDroopCalibrator::referenceLevel(curve, 0.15f);
    if (!nearlyEqual(ref, 0.0f))
        return fail("reference should read the flat center, not the drooping edges");
    return 0;
}

int testComputeCorrectionClamps()
{
    AnanDroopCalibrator::Curve curve{};
    curve[0] = 0.0f;
    curve[1] = -5.0f;
    curve[2] = -20.0f;
    curve[3] = -50.0f;
    const anan::DroopCorrectionTable table =
        AnanDroopCalibrator::computeCorrection(curve, /*referenceDb=*/0.0f, /*capDb=*/15.0f);
    if (!nearlyEqual(table[0], 0.0f) || !nearlyEqual(table[1], 5.0f)
        || !nearlyEqual(table[2], 15.0f) || !nearlyEqual(table[3], 15.0f)) {
        return fail("unexpected correction values");
    }
    return 0;
}

int testComputeCorrectionNeverNegative()
{
    // A bin ABOVE the reference (e.g. a stray signal the median did not
    // fully reject) must not produce a negative "correction" that would
    // attenuate a healthy bin.
    AnanDroopCalibrator::Curve curve{};
    curve[0] = 5.0f;
    const anan::DroopCorrectionTable table =
        AnanDroopCalibrator::computeCorrection(curve, /*referenceDb=*/0.0f, /*capDb=*/15.0f);
    if (!nearlyEqual(table[0], 0.0f))
        return fail("correction must clamp to >= 0");
    return 0;
}

int testLoadTablesOnInvalidScopeIsEmpty()
{
    const RadioSettingsScope invalidScope;   // default-constructed: empty family
    const auto tables = AnanDroopCalibrator::loadTables(invalidScope);
    if (!tables.isEmpty())
        return fail("an invalid scope must load nothing");
    return 0;
}

}  // namespace

int main()
{
    if (const int result = testMedianPowerCurveRejectsAStrayOutlierCapture(); result != 0)
        return result;
    if (const int result = testMedianPowerCurveEmptyIsAllZero(); result != 0)
        return result;
    if (const int result = testMedianPowerCurvePreservesDeepDroopValues(); result != 0)
        return result;
    if (const int result = testReferenceLevelUsesCentralWindow(); result != 0)
        return result;
    if (const int result = testComputeCorrectionClamps(); result != 0)
        return result;
    if (const int result = testComputeCorrectionNeverNegative(); result != 0)
        return result;
    if (const int result = testLoadTablesOnInvalidScopeIsEmpty(); result != 0)
        return result;
    std::printf("anan_droop_calibrator_test: all checks passed\n");
    return 0;
}
