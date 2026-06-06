// Regression tests for SliceRecreatePolicy (#3212).
//
// The #3212 bug: on reconnect the radio's GUIClientID session restore brought
// back our panadapter without a slice, "slice list" returned empty, and
// RadioModel unconditionally issued "display panafall create" — leaving a
// second, slice-less panadapter. These tests pin the decision so the duplicate
// PAN cannot silently come back, and lock in that the recreated slice is placed
// at the restored pan's center (not at a possibly-wrong-band LastFrequency).

#include "models/SliceRecreatePolicy.h"

#include <cstdio>
#include <cmath>

using namespace AetherSDR;
using namespace AetherSDR::SliceRecreatePolicy;

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

bool freqEq(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
}

// The exact scenario captured in the issue's support bundle:
//   - radio restored pan 0x40000000 centered on 20m (14.282408 MHz)
//   - "slice list" returned empty (slice was NOT restored)
//   - AppSettings LastFrequency was 28.305 MHz (10m) — a DIFFERENT band
// The fix must reuse the restored pan (no second pan) AND place the slice at the
// pan's center, never at the stale 10m LastFrequency.
void testIssue3212BundleScenario()
{
    Inputs in;
    in.hasRestoredPan = true;
    in.restoredPanCenterMhz = 14.282408;
    in.lastFreqMhz = 28.305000;   // stale, on a different band
    in.lastMode = QStringLiteral("USB");

    const Decision d = decide(in);

    check(d.action == Action::ReuseRestoredPan,
          "3212: restored pan is reused, NOT a second panafall created");
    check(freqEq(d.freqMhz, 14.282408),
          "3212: slice is placed at the restored pan center (14.282408)");
    check(!freqEq(d.freqMhz, 28.305000),
          "3212: slice is NOT placed at the stale 10m LastFrequency");
    check(d.mode == QStringLiteral("USB"), "3212: mode preserved from LastMode");
    check(d.antenna == QStringLiteral("ANT1"), "3212: antenna defaults to ANT1");
}

// First-ever connect / true standalone: no pan exists. We must create a new pan
// and seed it from LastFrequency (or the 14.225 default).
void testNoRestoredPanCreatesNew()
{
    Inputs in;
    in.hasRestoredPan = false;
    in.lastFreqMhz = 7.150000;
    in.lastMode = QStringLiteral("LSB");

    const Decision d = decide(in);

    check(d.action == Action::CreateNewPan,
          "no pan: a fresh panadapter is created");
    check(freqEq(d.freqMhz, 7.150000),
          "no pan: new slice seeded from LastFrequency");
    check(d.mode == QStringLiteral("LSB"), "no pan: mode from LastMode");
}

void testNoRestoredPanNoSettingsUsesDefaults()
{
    Inputs in;
    in.hasRestoredPan = false;
    in.lastFreqMhz = 0.0;     // unset
    in.lastMode = QString();  // unset

    const Decision d = decide(in);

    check(d.action == Action::CreateNewPan, "cold start: creates a pan");
    check(freqEq(d.freqMhz, 14.225000), "cold start: defaults to 14.225 MHz");
    check(d.mode == QStringLiteral("USB"), "cold start: defaults to USB");
    check(d.antenna == QStringLiteral("ANT1"), "cold start: defaults to ANT1");
}

// Edge: pan restored but its center hasn't been reported by the radio yet
// (claimed this instant, "display pan ... center=" still in flight). We still
// reuse the pan — never create a second one — and fall back to LastFrequency.
void testRestoredPanCenterNotYetKnownFallsBackToLastFreq()
{
    Inputs in;
    in.hasRestoredPan = true;
    in.restoredPanCenterMhz = 0.0;   // not reported yet
    in.lastFreqMhz = 21.300000;
    in.lastMode = QStringLiteral("USB");

    const Decision d = decide(in);

    check(d.action == Action::ReuseRestoredPan,
          "center unknown: still reuse the pan (no duplicate)");
    check(freqEq(d.freqMhz, 21.300000),
          "center unknown: fall back to LastFrequency");
}

void testRestoredPanCenterUnknownNoSettingsFallsBackToDefault()
{
    Inputs in;
    in.hasRestoredPan = true;
    in.restoredPanCenterMhz = 0.0;
    in.lastFreqMhz = 0.0;
    in.lastMode = QString();

    const Decision d = decide(in);

    check(d.action == Action::ReuseRestoredPan,
          "center+settings unknown: still reuse the pan");
    check(freqEq(d.freqMhz, 14.225000),
          "center+settings unknown: fall back to 14.225 default");
    check(d.mode == QStringLiteral("USB"), "center unknown: default mode USB");
}

// The restored-pan center always wins over LastFrequency, even when both are on
// the same band — the pan center is radio-authoritative for what's on screen.
void testRestoredPanCenterWinsOverLastFrequency()
{
    Inputs in;
    in.hasRestoredPan = true;
    in.restoredPanCenterMhz = 14.250000;
    in.lastFreqMhz = 14.074000;   // same band, different freq
    in.lastMode = QStringLiteral("USB");

    const Decision d = decide(in);

    check(freqEq(d.freqMhz, 14.250000),
          "pan center wins over LastFrequency even on the same band");
}

}  // namespace

int main()
{
    testIssue3212BundleScenario();
    testNoRestoredPanCreatesNew();
    testNoRestoredPanNoSettingsUsesDefaults();
    testRestoredPanCenterNotYetKnownFallsBackToLastFreq();
    testRestoredPanCenterUnknownNoSettingsFallsBackToDefault();
    testRestoredPanCenterWinsOverLastFrequency();

    if (failures == 0) {
        std::printf("\nAll slice_recreate_policy tests passed.\n");
        return 0;
    }
    std::printf("\n%d slice_recreate_policy test(s) FAILED.\n", failures);
    return 1;
}
