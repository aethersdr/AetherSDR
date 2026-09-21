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
#include "gui/PanZoomModeGate.h"

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
    // THE HL2'S radioOwnsDbmScale IS NOT ASSERTED HERE. Its DEFAULT is, below,
    // which is a different fact — the one that would change every silent
    // backend's claim at once if it moved. It is wrong for this radio and it
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
    //
    // The record is PRESENT, which is a claim in its own right: absent means
    // "no backend has been read on the question", and an assertion that only
    // read the fields would pass on a default-constructed nullopt turning into
    // a silent false. Check presence first, then each field.
    check(caps.panSpanModel.has_value(),
          "the HL2 DECLARES a span model — absence would mean nobody had read it");
    check(caps.panSpanModel && caps.panSpanModel->followsSampleRate,
          "the pan span IS the sample rate, so the rate list is the complete span set");

    // One rate field for the whole board — MetisProtocol::ccConfig packs
    // SampleRate into C1[1:0], with the receiver COUNT in a separate field —
    // so a span change is radio-wide. Revert this and a per-pan span control
    // looks legitimate on a radio where narrowing one window silently retunes
    // the other three.
    check(caps.panSpanModel && caps.panSpanModel->radioWide,
          "one DDC rate for the whole radio — span is shared, not per-panadapter");

    // ---- the Y axis is dBFS wearing a dBm label ----
    //
    // THIS ASSERTION PINS TODAY'S ANSWER AND WILL NEED REVISITING. An earlier
    // version of this comment claimed the opposite — that asserting against
    // Hl2DbReference's own predicate rather than a hardcoded false means "the
    // day a per-unit fullScaleDbm is populated the declaration follows it and
    // this assertion keeps holding". It does not, and aethersdr-agent showed
    // why on #5726: production reads the BACKEND's m_dbRef, while the
    // right-hand side here is a default-constructed Hl2DbReference{} whose
    // m_fullScaleDbm is 0.0 by definition. On the day a measurement lands the
    // two sides diverge and this FAILS.
    //
    // Reading the reference off the backend instance would make the original
    // claim true. m_dbRef is private with no accessor, and inventing a test
    // seam so a comment can be accurate is the worse trade — so the comment is
    // corrected instead. Today both sides are false and the check is right.
    check(caps.panAmplitude.has_value(),
          "the HL2 DECLARES an amplitude model — this is a read backend, not a "
          "silent one, and dbmAxisIsCalibrated() must not be answering from the "
          "absent-means-legacy branch");
    check(caps.panAmplitude
              && caps.panAmplitude->calibratedDbm == hl2::Hl2DbReference{}.isCalibrated(),
          "the dBm axis declaration matches Hl2DbReference on a fresh radio");
    check(caps.dbmAxisIsCalibrated() == hl2::Hl2DbReference{}.isCalibrated(),
          "and the accessor reports the declared field, not its absent default");
    // STILL UNCALIBRATED, AND THIS PR BRIEFLY SAID OTHERWISE. A draft of this
    // change flipped the expectation to `caps.dbmAxisIsCalibrated()` on the
    // grounds that the reference is now DERIVED rather than per-unit. The
    // derivation is real and the absolute offset it produces is the point of
    // the PR — but `calibratedDbm` is not "a figure exists", it is
    // RadioCapabilities.h's licence to compare this radio's levels with
    // another station's, and only a measurement earns that. Hl2DbReference::
    // isCalibrated() now reports whether setFullScaleDbm has been called, and
    // nothing in src/ calls it, so the honest declaration is unchanged.
    check(!caps.dbmAxisIsCalibrated(),
          "the axis is still dBFS wearing a dBm label — the reference is "
          "DERIVED, and derived is not measured");

    // WHAT THESE THREE CANNOT SEE, said plainly because this file's own thesis
    // is that a test carrying its own copy of the truth cannot detect the
    // declaration and the code diverging.
    //
    // Hl2DbReference{}.isCalibrated() is false on a default-constructed
    // reference by construction -- the measured flag starts clear and only
    // setFullScaleDbm sets it -- so it is unconditionally false. Comparing
    // the declaration against it therefore compares false with false: replace
    // `amplitude.calibratedDbm = m_dbRef.isCalibrated()` in
    // Hl2Backend::capabilities() with a literal `false` and every assertion
    // above still passes. So "the day a per-unit fullScaleDbm is populated the
    // declaration follows it" is NOT something this test observes — and worse
    // than that, per aethersdr-agent on #5726: when that day comes the two
    // sides of the comparison diverge and this assertion FAILS, because
    // production reads the backend's m_dbRef and this reads a
    // default-constructed one.
    //
    // Nothing better is reachable without a seam to set fullScaleDbm on a
    // pre-connect backend, and inventing one for a test is a worse trade than
    // stating the limit. Raised by aethersdr-agent on #5725.

    // The permissive defaults these fields carry are load-bearing, and a
    // regression that flipped either would make every silent backend change its
    // claim at once. Pin them from a default-constructed descriptor, beside the
    // HL2's overrides, so the facts fail separately.
    //
    // THE TWO ACCESSORS FALL OPPOSITE WAYS ON THE SAME ABSENT RECORD. That is
    // the point of the record and the single thing a conversion could silently
    // regress, so both directions are asserted here rather than inferred.
    check(RadioCapabilities{}.radioOwnsDbmScale,
          "radioOwnsDbmScale still defaults TRUE (the legacy shape)");
    check(!RadioCapabilities{}.panAmplitude.has_value(),
          "a descriptor nobody has written declares NO amplitude model");
    check(RadioCapabilities{}.dbmAxisIsCalibrated(),
          "absent -> CALIBRATED: the legacy claim is kept, matching the bool "
          "default this replaced");
    check(!RadioCapabilities{}.panBinsAbsolute(),
          "absent -> NOT absolute: the opposite fall, matching the bool default "
          "this replaced. The auto-floor gate's OR stays permissive through "
          "radioOwnsDbmScale instead");
    check(!RadioCapabilities{}.panSpanModel.has_value(),
          "and no span model either — a radio without the constraint, and one "
          "nobody has read, are the same descriptor only because neither is "
          "allowed to claim anything");

    // ---- band/segment zoom: the gate, and that one predicate serves both ----
    //
    // Same thesis as the rest of this file -- a control that lies is the
    // symptom of a declaration nobody reads. `band_zoom=`/`segment_zoom=` are
    // Flex wire text; RadioModel::sendCmd drops them at hasCommandPlane() and
    // emits commandDropped(). SpectrumWidget::setBandSegmentZoomAvailable()
    // disabled the two buttons for that reason, and the keyboard/MIDI/
    // FlexControl/RC28/automation paths into MainWindow::togglePanZoomModeForPan
    // and MainWindow::setPanZoomMode went around it.
    //
    // The input is READ OFF THIS RADIO'S OWN DECLARATION rather than retyped
    // as a literal false: the gate's middle rung is
    // RadioCapabilities::panZoomModes.has_value(), so that is what is asked
    // here. Engage the record in Hl2Backend::capabilities() and this section
    // follows it instead of agreeing with a stale copy -- which is the failure
    // mode the header of this file is about. (It used to ask
    // `caps.family == "flex"`, which was the family-string branch #5554's
    // standing notice forbids; the record is the sanctioned shape and does not
    // move the capability-bool ratchet, since that counts direct bool members
    // of RadioCapabilities and an std::optional is not one.)
    check(!caps.panZoomModes.has_value(),
          "this radio declares NO band/segment zoom -- absent, not a false "
          "bool, so 'nobody set it' and 'considered no' are not the same "
          "record");

    const bool hl2DeclaresPanZoomModes = caps.panZoomModes.has_value();

    check(!panZoomModeWritable(/*connected=*/true, hl2DeclaresPanZoomModes,
                               /*panKnown=*/true),
          "REFUSED on this radio even connected with a pan: band/segment zoom "
          "is Flex wire text and this backend declares no band/segment zoom");
    check(panZoomModeRefusal(/*connected=*/true, hl2DeclaresPanZoomModes,
                             /*panKnown=*/true)
              == PanZoomModeRefusal::NotDeclared,
          "and it is refused for the CAPABILITY, not for a missing pan or a "
          "missing connection -- the reason a caller would show the operator, "
          "and the ONLY rung the call sites announce with "
          "showUnsupportedControlNotice()");
    check(!bandSegmentZoomAvailable(/*connected=*/true, hl2DeclaresPanZoomModes),
          "the B/S buttons are disabled on this radio for the same reason");

    // THE CONTAINMENT, which is the half that could drift: the availability
    // the buttons show must be the admissibility the command paths test, with
    // the pan taken as present. Asserted over every input rather than by
    // comment, so a future edit that special-cases one side fails here.
    for (bool connected : {false, true}) {
        for (bool declared : {false, true}) {
            check(bandSegmentZoomAvailable(connected, declared)
                      == panZoomModeWritable(connected, declared,
                                             /*panKnown=*/true),
                  "button availability and write admissibility agree on every "
                  "(connected, declared) pair");
            // A write is never admitted without a pan, whatever the rest says.
            check(!panZoomModeWritable(connected, declared, /*panKnown=*/false),
                  "no resolvable pan is always a refusal");
        }
    }

    // A radio that DOES declare it, for contrast: the gate refuses this one for
    // a property of the radio, not because it refuses everything.
    check(panZoomModeWritable(/*connected=*/true,
                              /*panZoomModesDeclared=*/true,
                              /*panKnown=*/true),
          "a connected radio that declares the record, with a pan, IS admitted "
          "(positive control -- the refusal above is a capability decision, "
          "not a dead predicate)");
    check(panZoomModeRefusal(/*connected=*/false,
                             /*panZoomModesDeclared=*/true,
                             /*panKnown=*/true)
              == PanZoomModeRefusal::NotConnected,
          "and a disconnected one is refused for being disconnected");

    // WHAT THIS SECTION CANNOT SEE, said as plainly as the dBm note above.
    //
    // It pins the PREDICATE and the containment between its two spellings. It
    // does NOT observe that MainWindow::togglePanZoomModeForPan and
    // MainWindow::setPanZoomMode call it: no registered test target links
    // MainWindow*.cpp, so an edit that deletes the call from either one leaves
    // every assertion here green. What stands behind those two call sites is
    // that there is now only one predicate to call -- the button-availability
    // computation in MainWindow::onConnectionStateChanged reads the same
    // function -- so the drift this guards against is the predicate changing
    // under a caller, not a caller quietly dropping it.
    //
    // Closing the remaining half needs a MainWindow seam that does not exist
    // today. Stated rather than left for the next reader to assume otherwise.

    std::printf("%s: %d failure(s)\n", argv[0], failures);
    return failures == 0 ? 0 : 1;
}
