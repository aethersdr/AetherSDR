// What the shipped droop defaults do to the NOISE-FLOOR AUTO-ADJUST, which is
// a different consumer from the panadapter trace and is not covered by looking
// at the display.
//
// Why this test exists. SpectrumWidget::estimateNoiseFloorDbm() averages every
// bin at or below the frame mean, over the WHOLE frame with no edge exclusion,
// and it runs on the radio's bins before any painting -- so the render-side
// kEdgeTaperFraction gradient hides nothing from it. Its result becomes
// m_noiseFloorBaselineDbm, which becomes m_refLevel: the panadapter's entire
// vertical scale. Since #5726 the gate that lets that loop run is open for the
// ANAN (PanAmplitudeModel::binsAbsolute), so seeding droop defaults moves the
// reference level on a radio where it previously could not.
//
// An UNCORRECTED DDC span is the pathological input for that estimator: the
// roll-off puts ~174 of 1024 bins tens of dB below the floor, every one of
// them lands under the pass-1 mean, and every one then drags pass 2 down with
// it. The correction is what pulls them back to the floor they belong at.
//
// Socket-free and Qt-free: it drives the real applyDroopCorrectionDb(),
// applyEdgeFade() and estimateNoiseFloorDbm(), not copies of them.

#include "core/backends/anan/AnanDroopDefaults.h"
#include "gui/NoiseFloorEstimator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::anan;

namespace {

constexpr float kTrueFloorDbm = -120.0f;

int fail(const char* message)
{
    std::fprintf(stderr, "anan_droop_noise_floor_test: %s\n", message);
    return 1;
}

// What the radio actually hands us: a flat noise floor pushed down by the
// DDC chain's own response. `physCapDb` stands in for wherever the real
// datapath stops falling -- swept, because the exact depth is a property of
// the hardware and the conclusion must not depend on guessing it.
std::vector<float> droopedFrame(const DroopCorrectionTable& chain, float physCapDb)
{
    std::vector<float> bins(kDroopCorrectionFftSize);
    for (std::size_t i = 0; i < bins.size(); ++i)
        bins[i] = kTrueFloorDbm - std::min(chain[i], physCapDb);
    return bins;
}

float estimate(const std::vector<float>& bins)
{
    return estimateNoiseFloorDbm(std::span<const float>(bins.data(), bins.size()));
}

int testCorrectionPullsTheFloorEstimateBackToTruth()
{
    const DroopCorrectionTable* table = defaultDroopTableForRate(48);
    if (!table)
        return fail("48 ksps returned no default table");

    for (const float physCapDb : {90.0f, 60.0f, 40.0f}) {
        const std::vector<float> raw = droopedFrame(*table, physCapDb);

        // Uncorrected -- what an uncalibrated radio fed this loop before the
        // defaults shipped.
        const float uncorrected = estimate(raw);

        // Corrected exactly as AnanRxDsp::processIqBlock() does it.
        std::vector<float> corrected = raw;
        applyDroopCorrectionDb(corrected, *table);
        applyEdgeFade(corrected);
        const float withDefaults = estimate(corrected);

        // The uncorrected estimate must be badly wrong -- if it were not,
        // this whole interaction would be a non-event and the rest of the
        // assertions would be vacuous.
        if (std::fabs(uncorrected - kTrueFloorDbm) < 15.0f)
            return fail("uncorrected drooped frame did NOT bias the floor estimate; "
                        "this test can no longer tell the correction apart from nothing");

        // ...and it must be biased LOW, not high: the roll-off only ever
        // pushes bins down.
        if (uncorrected > kTrueFloorDbm)
            return fail("uncorrected estimate came out above the true floor");

        // With the defaults applied the estimate must land near the truth.
        if (std::fabs(withDefaults - kTrueFloorDbm) > 12.0f)
            return fail("corrected frame still does not put the floor estimate "
                        "near the true noise floor");

        // And it must be a strict improvement, by a wide margin.
        if (std::fabs(withDefaults - kTrueFloorDbm) >= std::fabs(uncorrected - kTrueFloorDbm))
            return fail("the correction did not improve the floor estimate");

        std::printf("  chain capped at %5.1f dB: uncorrected %8.2f dBm -> "
                    "with defaults %8.2f dBm (true %.1f)\n",
                    static_cast<double>(physCapDb), static_cast<double>(uncorrected),
                    static_cast<double>(withDefaults), static_cast<double>(kTrueFloorDbm));
    }
    return 0;
}

// The property that makes the above matter at all: the estimator does not
// exclude edge bins, so a backend cannot rely on a display-side fade to keep
// them out of the reference level. If this ever stops being true, the test
// above is measuring something else.
int testEstimatorHasNoEdgeExclusion()
{
    std::vector<float> flat(kDroopCorrectionFftSize, kTrueFloorDbm);
    const float baseline = estimate(flat);

    // Drop only the outermost bins -- well inside any plausible edge margin.
    std::vector<float> edged = flat;
    for (std::size_t i = 0; i < 40; ++i) {
        edged[i] = kTrueFloorDbm - 60.0f;
        edged[edged.size() - 1 - i] = kTrueFloorDbm - 60.0f;
    }
    const float withEdges = estimate(edged);

    if (std::fabs(withEdges - baseline) < 1.0f)
        return fail("edge bins did not move the floor estimate -- the estimator "
                    "now excludes them, so the droop interaction this file "
                    "pins no longer works the way it documents");
    return 0;
}

}  // namespace

int main()
{
    int failures = 0;
    failures += testCorrectionPullsTheFloorEstimateBackToTruth();
    failures += testEstimatorHasNoEdgeExclusion();
    if (failures == 0)
        std::printf("anan_droop_noise_floor_test: all checks passed\n");
    return failures;
}
