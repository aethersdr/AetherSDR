// The panadapter limits the Hermes-Lite 2 actually has, as CAPABILITIES.
//
// Four properties of this radio were true and undeclared: a 48 kHz span floor,
// four discrete rates, one span shared by every receiver, and an uncalibrated
// dBFS axis. A fifth, radioOwnsDbmScale, was worse than undeclared — it was
// inheriting the permissive default, which asserts a command plane this backend
// does not have.
//
// WHY THIS TEST EXISTS AT ALL. A capability declaration is the shape that rots
// silently. Nothing calls it, nothing crashes when it drifts, and the only
// symptom is a control somewhere that lies. So every assertion here compares
// the declaration against the SAME constant or predicate production reads —
// hl2::kIqSampleRatesHz for the rates, Hl2DbReference::isCalibrated() for the
// axis — rather than against a re-typed copy of its values. A test that carries
// its own copy of the truth cannot detect the declaration and the code
// diverging, which is exactly the failure being guarded against.
//
// SOCKET-FREE. Hl2Backend::capabilities() takes its receiver ceiling from
// m_connected ? receiverCeiling() : the id count, so the whole descriptor is
// available on a default-constructed backend. Nothing is bound, nothing is
// connected, no event loop is pumped, and no radio is required. (The connected
// half of this seam — that panBandwidthLimitsChanged really emits
// kIqSampleRatesHz[0] as its lower bound — lived in the fake-EP6 fixture that
// is now retired; see the commented block in tests/tests.cmake. What is pinned
// here is the DECLARATION and the constant it is built from, which is the half
// that can rot without anyone noticing.)
//
// NOTHING HERE WAS MEASURED ON A RADIO. In particular this file makes no claim
// about the 24 dB/s dBm ratchet RadioCapabilities.h describes: that is a runtime
// question, and radioOwnsDbmScale is asserted on the SOURCE fact it is actually
// about — whether there is an echo to wait for.

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2DbReference.h"

#include <QCoreApplication>

#include <algorithm>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* label)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", label);
    if (!condition) {
        ++failures;
    }
}
}  // namespace

int main(int argc, char** argv)
{
    // The backend touches AppSettings on construction (the owned "Hl2" span
    // object). Redirect it before QCoreApplication so no assertion here can
    // read or write the operator's live configuration.
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-pan-limits-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);

    hl2::Hl2Backend backend;
    const RadioCapabilities caps = backend.capabilities();

    check(caps.family == QLatin1String("hl2"),
          "this is the HL2 descriptor (sanity, not the subject)");

    // ---- the dBm scale is not the radio's, because it has no command plane ----
    //
    // `display pan set … min_dbm=…` is Flex wire text. RadioModel::sendCmd drops
    // it at hasCommandPlane(), which is m_wanConn || m_connection, and
    // m_connection is assigned only inside the dynamic_cast<FlexBackend*> branch
    // of RadioModel::setupBackend. FlexBackend::decodePanRange is the only
    // reader of min_dbm anywhere in the tree.
    //
    // radioOwnsDbmScale IS NOT ASSERTED HERE. It is wrong for this radio and it
    // is deliberately left undeclared: bench run d101 measured the auto-floor
    // loop SETTLING on this radio (0.307 dB in 74 s quiescent, 0.0000 dB/s over
    // the second half, re-settling within ~30 s after a 12 dB LNA step), and the
    // early return in SpectrumWidget::applyNoiseFloorAutoAdjust keys on the same
    // flag -- so declaring it would remove a loop that works. See the note in
    // Hl2Backend::capabilities(). Asserting it here would pin a decision this
    // change deliberately does not take.

    // ---- the span floor and the four discrete rates ----
    //
    // kIqSampleRatesHz is THE list: capability advertisement, zoom clamp and
    // snap target are one array in Hl2Backend.h. The assertion is that the
    // capability still IS that array, not that it happens to contain 48000.
    {
        QVector<int> expected;
        for (const int rate : hl2::kIqSampleRatesHz)
            expected.append(rate);
        check(caps.sampleRatesHz == expected,
              "sampleRatesHz is exactly kIqSampleRatesHz, the list production snaps to");
        check(!caps.sampleRatesHz.isEmpty()
                  && caps.sampleRatesHz.first() == hl2::kIqSampleRatesHz[0],
              "the narrowest rate is the span FLOOR, and it is the same constant");
        check(std::is_sorted(caps.sampleRatesHz.cbegin(), caps.sampleRatesHz.cend()),
              "ascending, so first() is genuinely the floor and last() the ceiling");
        // The one DELIBERATE literal in this file. Everything else compares
        // production against production; this pins the COUNT, because "four
        // discrete rates" is a claim made OUTSIDE the code — in the capability
        // map, and in what gets said upstream — and a fifth rate appearing would
        // make that claim stale while every other assertion here still passed.
        check(caps.sampleRatesHz.size() == 4,
              "four DISCRETE rates — there is nothing between them to select");
    }

    // Why that floor is a floor and not a default: the span IS the sample rate,
    // so a narrower window would need samples the DDC never delivered. Revert
    // this and a client is entitled to take a 5 kHz zoom request literally
    // instead of snapping it to a rate.
    check(caps.panSpanFollowsSampleRate,
          "the pan span IS the sample rate, so the rate list is the complete span set");

    // One rate field for the whole board — MetisProtocol::ccConfig packs
    // SampleRate into C1[1:0], with the receiver COUNT in a separate field —
    // so a span change is radio-wide. Revert this and a per-pan span control
    // looks legitimate on a radio where narrowing one window silently retunes
    // the other three.
    check(caps.panSpanIsRadioWide,
          "one DDC rate for the whole radio — span is shared, not per-panadapter");

    // ---- the Y axis is dBFS wearing a dBm label ----
    //
    // Asserted against Hl2DbReference's own predicate rather than a hardcoded
    // false, so the day a per-unit fullScaleDbm is measured and populated the
    // declaration follows it and this assertion keeps holding instead of having
    // to be remembered. The second check is what today's answer is.
    check(caps.reportsCalibratedDbm == hl2::Hl2DbReference{}.isCalibrated(),
          "the dBm axis declaration tracks Hl2DbReference::isCalibrated()");
    check(!caps.reportsCalibratedDbm,
          "and today that means UNCALIBRATED — no per-unit fullScaleDbm exists");

    // The permissive defaults these two fields carry are load-bearing, and a
    // regression that flipped either default would make every silent backend
    // change its claim at once. Pin them from a default-constructed descriptor,
    // beside the HL2's overrides, so the two facts fail separately.
    check(RadioCapabilities{}.radioOwnsDbmScale,
          "radioOwnsDbmScale still defaults TRUE (the legacy shape)");
    check(RadioCapabilities{}.reportsCalibratedDbm,
          "reportsCalibratedDbm also defaults TRUE — see its comment for why");
    check(!RadioCapabilities{}.panSpanFollowsSampleRate
              && !RadioCapabilities{}.panSpanIsRadioWide,
          "both span-shape fields default FALSE — a radio without the constraint");

    std::printf("%s: %d failure(s)\n", argv[0], failures);
    return failures == 0 ? 0 : 1;
}
