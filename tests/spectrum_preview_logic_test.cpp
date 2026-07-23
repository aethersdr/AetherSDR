#include "gui/SpectrumPreviewLogic.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "spectrum_preview_logic_test: %s\n", message);
    return 1;
}

bool nearlyEqual(double a, double b, double epsilon = 1.0e-12)
{
    return std::abs(a - b) <= epsilon;
}

int testCursorAnchoredZoom()
{
    using namespace AetherSDR;
    const FrequencyFrame base{14.1, 0.2};
    constexpr std::array<double, 5> kFractions{
        -0.5, -0.25, 0.0, 0.31, 0.5,
    };
    constexpr std::array<double, 2> kBandwidths{0.08, 0.45};
    for (double fraction : kFractions) {
        const double anchorMhz = frequencyAtFraction(base, fraction);
        for (double bandwidthMhz : kBandwidths) {
            const double centerMhz = centerForAnchoredBandwidth(
                anchorMhz, fraction, bandwidthMhz);
            const FrequencyFrame zoomed{centerMhz, bandwidthMhz};
            if (!nearlyEqual(frequencyAtFraction(zoomed, fraction),
                             anchorMhz)) {
                return fail("cursor anchor moved during zoom");
            }
        }
    }
    if (!nearlyEqual(frequencyCanvasFraction(250.0, 1000), -0.25)
        || !nearlyEqual(frequencyCanvasFraction(-100.0, 1000), -0.5)
        || !nearlyEqual(frequencyCanvasFraction(1200.0, 1000), 0.5)) {
        return fail("frequency canvas fraction should clamp to the canvas");
    }
    return 0;
}

int testFrequencyFrameMapping()
{
    using namespace AetherSDR;
    const FrequencyFrame source{14.0, 0.2};
    const FrequencyFrame pannedTarget{14.05, 0.2};
    const FrequencyFrame zoomedTarget{14.0, 0.1};
    if (!nearlyEqual(sourceUnitPosition(0.5, source, pannedTarget), 0.75)
        || !nearlyEqual(sourceUnitPosition(0.5, source, zoomedTarget), 0.5)
        || !nearlyEqual(sourceUnitPosition(0.0, source, zoomedTarget), 0.25)
        || !nearlyEqual(sourceUnitPosition(1.0, source, zoomedTarget), 0.75)) {
        return fail("frequency frame mapping is incorrect");
    }
    if (sourceUnitPosition(1.0, source, FrequencyFrame{14.2, 0.2}) <= 1.0) {
        return fail("newly exposed frequency should map outside the retained frame");
    }

    const FrequencyFrame vhfBase{146.520000, 0.005};
    const FrequencyFrame vhfTarget{146.520375, 0.0025};
    const FrequencyPreviewTransform transform = frequencyPreviewTransform(
        vhfBase, vhfTarget);
    if (!transform.valid || !nearlyEqual(transform.scale, 0.5)
        || !nearlyEqual(transform.offset, 0.075, 1.0e-10)) {
        return fail("preview transform lost narrow-band VHF precision");
    }
    const double sourceU = sourceUnitPosition(0.37, vhfBase, vhfTarget);
    const double shaderEquivalent = 0.5 + transform.offset
        + (0.37 - 0.5) * transform.scale;
    if (!nearlyEqual(sourceU, shaderEquivalent, 1.0e-10)) {
        return fail("CPU and shader preview transforms diverged");
    }
    if (!std::isnan(sourceUnitPosition(
            0.5, FrequencyFrame{14.0, 0.0}, vhfTarget))) {
        return fail("invalid source frame should fail closed");
    }
    if (FrequencyFrame{std::numeric_limits<double>::infinity(), 0.2}.isValid()
        || FrequencyFrame{14.0, std::numeric_limits<double>::infinity()}
               .isValid()) {
        return fail("non-finite frequency frames should be rejected");
    }
    return 0;
}

int testCommandThrottle()
{
    using namespace AetherSDR;
    FrequencyRangeCommandThrottle throttle;
    const FrequencyRangeCommand first{14.0, 0.2};
    const FrequencyRangeCommand intermediate{14.01, 0.18};
    const FrequencyRangeCommand latest{14.02, 0.16};
    const FrequencyRangeCommand final{14.03, 0.14};

    const std::optional<FrequencyRangeCommand> emittedFirst =
        throttle.request(first, true, false);
    if (!emittedFirst.has_value() || throttle.hasPending()) {
        return fail("first due command should emit immediately");
    }
    if (throttle.request(intermediate, false, false).has_value()
        || throttle.request(latest, false, false).has_value()
        || !throttle.hasPending()) {
        return fail("intermediate commands should coalesce");
    }
    const std::optional<FrequencyRangeCommand> emittedFinal =
        throttle.request(final, false, true);
    if (!emittedFinal.has_value()
        || !nearlyEqual(emittedFinal->centerMhz, final.centerMhz)
        || throttle.hasPending()
        || throttle.takePending().has_value()) {
        return fail("forced release should emit once and cancel delayed output");
    }
    throttle.request(intermediate, false, false);
    throttle.request(latest, false, false);
    const std::optional<FrequencyRangeCommand> timeoutCommand =
        throttle.takePending();
    if (!timeoutCommand.has_value()
        || !nearlyEqual(timeoutCommand->centerMhz, latest.centerMhz)
        || throttle.hasPending()) {
        return fail("timeout should emit only the latest coalesced command");
    }
    if (throttle.request(FrequencyRangeCommand{
            std::numeric_limits<double>::quiet_NaN(), 0.2}, true, false)
            .has_value()) {
        return fail("invalid command should be rejected");
    }
    return 0;
}

int testWaterfallPipelineSelection()
{
    using namespace AetherSDR;
    WaterfallRowFrameReadiness readiness{
        true, true, true, true, true, true,
    };
    if (chooseWaterfallPipeline(readiness)
        != WaterfallPipelineMode::RowFrequencyFrames) {
        return fail("complete row-frame resources should select the GPU preview");
    }

    bool* stages[] = {
        &readiness.requested,
        &readiness.formatSupported,
        &readiness.textureCreated,
        &readiness.samplerCreated,
        &readiness.bindingsCreated,
        &readiness.pipelineCreated,
    };
    for (bool* stage : stages) {
        *stage = false;
        if (chooseWaterfallPipeline(readiness)
            != WaterfallPipelineMode::Legacy) {
            return fail("any row-frame failure must select the legacy pipeline");
        }
        *stage = true;
    }
    return 0;
}

} // namespace

int main()
{
    if (const int result = testCursorAnchoredZoom(); result != 0) {
        return result;
    }
    if (const int result = testFrequencyFrameMapping(); result != 0) {
        return result;
    }
    if (const int result = testCommandThrottle(); result != 0) {
        return result;
    }
    return testWaterfallPipelineSelection();
}
