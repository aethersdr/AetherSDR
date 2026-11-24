// RTTY decoder sensitivity mapping (#5028): slider 0..100 -> confidence
// threshold 0.50..0.95 on RttyDecoder's own [0.5, 1.0] confidence scale.
// Pure and constexpr, so the load-bearing rows are also checked at compile time.

#include "gui/RttyDecoderSensitivity.h"

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

static_assert(rttyConfThresholdFor(0) == 0.5f,
              "0 = show everything: the threshold is the confidence floor");
static_assert(rttyConfThresholdFor(100) == 0.95f,
              "100 = only near-certain copy");
static_assert(kRttySensitivityDefault == 0,
              "the default is a provable no-op: confidence is >= 0.5 by "
              "construction, so nothing can fall below the floor threshold — "
              "out-of-the-box behavior is identical to the pane before #5028");
static_assert(rttyConfThresholdFor(kRttySensitivityDefault) == 0.5f,
              "default threshold sits exactly on the confidence floor");

int main()
{
    int failures = 0;
    auto expectNear = [&](int sens, float expected, const char* why) {
        const float got = rttyConfThresholdFor(sens);
        const bool ok = std::fabs(got - expected) < 0.0005f;
        std::printf("[%s] sens=%3d -> %.4f (expected %.4f) %s\n",
                    ok ? " OK " : "FAIL", sens, got, expected, why);
        if (!ok) {
            ++failures;
        }
    };
    expectNear(0,   0.500f, "floor (and the default): every character passes");
    expectNear(23,  0.6035f, "documented: the first value above the red band (0.60), hides every red letter");
    expectNear(45,  0.7025f, "documented: the first value above the orange band (0.70), hides orange too");
    expectNear(78,  0.851f, "documented: the first value above the yellow band (0.85), only green is left");
    expectNear(50,  0.725f, "midpoint");
    expectNear(100, 0.950f, "ceiling");
    expectNear(-5,  0.500f, "clamped below");
    expectNear(250, 0.950f, "clamped above");
    // Monotonic: more sensitivity never lets MORE noise through.
    for (int s = 1; s <= 100; ++s) {
        if (!(rttyConfThresholdFor(s) > rttyConfThresholdFor(s - 1))) {
            std::printf("[FAIL] not strictly increasing at %d\n", s);
            ++failures;
        }
    }
    std::printf("%s (%d failures)\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
