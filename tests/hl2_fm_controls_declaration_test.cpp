// The FM repeater and tone controls the Hermes-Lite 2 does NOT have.
//
// Two fields decide whether those controls are live, and on this radio both
// answered wrongly in different ways:
//
//   * hasFmRepeaterOffset was never declared, so it INHERITED the struct's
//     permissive default (true) and the offset spin, the ±steps and the simplex
//     button came up enabled. IRadioBackend::setSliceRepeaterOffsetDir and
//     setSliceFmRepeaterOffset are virtuals with empty bodies and Hl2Backend
//     overrides neither, so those controls moved a number that reached nothing.
//
//   * fmTonePresentation WAS declared, as Legacy — the capability map's row was
//     wrong about this half. Legacy is the value that fills the tone-mode combo
//     from legacyFmToneModes(), i.e. it offers CTCSS ENCODE, on a backend that
//     has no CTCSS encoder anywhere and cannot key an FM carrier at all.
//
// THE ARGUMENT RESTS ON receiveOnlyModes, NOT ON THE MODULATOR, and that is a
// deliberate choice this target enforces. The HL2's transmit modulator is
// selected at BUILD time by AETHER_HL2_TX_TXA (default ON = a WDSP TXA channel,
// whose chain does carry fmmod; OFF = the in-tree phasing SSB modulator), so
// any assertion phrased about the modulator would be half wrong in every build.
// What holds in both is that FM and NFM are declared receive-only, which is
// what RadioModel::refuseKeyInReceiveOnlyMode() reads.
//
// WHY THESE TWO AND NOT FM ITSELF. Nothing here says the HL2 cannot RECEIVE FM;
// it can, and this test asserts that it still declares FM receive-only rather
// than absent. What is withdrawn is a transmit-side control surface for a mode
// the backend already refuses to key in.
//
// WHY A SEPARATE TARGET. Same reason tests.cmake gives beside
// hl2_pan_limits_declaration_test: the fake-radio fixture that would naturally
// carry an HL2 seam assertion — hl2_backend_test — is retired inside a
// commented block, so an assertion written there would be compiled by nothing
// and green forever, and "a declaration must not be pinned only inside
// something that does not build."
//
// SOCKET-FREE. Constructs a backend and reads capabilities(); binds nothing,
// connects nothing, pumps no event loop, and reaches no radio.
//
// WHAT THIS FILE CANNOT OBSERVE, said plainly because a declaration test that
// overstates its reach is worse than none:
//
//   1. fmTonePresentation's struct DEFAULT is already Hidden. So the assertion
//      that the HL2 reports Hidden would also pass if the line were deleted
//      entirely. It pins the VALUE, not the fact that it is stated. Nothing
//      reachable from here distinguishes declared-Hidden from inherited-Hidden
//      — that is a property of the source text, and RadioCapabilities.h's
//      "a backend that omits one silently declares it absent" rule is what
//      covers it. hasFmRepeaterOffset does not have that problem: its default
//      is the OPPOSITE value, which is asserted below so the two facts fail
//      separately.
//   2. That the two widgets actually honour these fields. VfoWidget::
//      configureFmToneControls and RxApplet::configureFmToneControls read them
//      directly — a coupling a reader can see in four lines — but reaching it
//      needs a constructed widget, and no test in this tree builds one of
//      these. Nothing here was measured on a radio either.

#include "TestSettingsProfile.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/hl2/Hl2Backend.h"

#include <QCoreApplication>

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
    // object). Redirect it before QCoreApplication so nothing here can read or
    // write the operator's live configuration.
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-fm-controls-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);

    hl2::Hl2Backend backend;
    const RadioCapabilities caps = backend.capabilities();

    check(caps.family == QLatin1String("hl2"),
          "this is the HL2 descriptor (sanity, not the subject)");

    // ---- the reason both controls are dead, read from production ----
    //
    // Not a re-typed premise: receiveOnlyModes is the SAME list the key-on
    // guard reads (RadioModel::refuseKeyInReceiveOnlyMode). If FM ever leaves
    // it — which is what a real TX chain landing would mean — this assertion is
    // the one that fails, and the two declarations below become re-openable
    // questions rather than settled ones.
    check(caps.receiveOnlyModes.contains(QStringLiteral("FM"))
              && caps.receiveOnlyModes.contains(QStringLiteral("NFM")),
          "FM and NFM are RECEIVE-ONLY here — the radio will not key in them");

    // ---- no repeater duplex ----
    check(!caps.hasFmRepeaterOffset,
          "the HL2 declines the repeater offset: there is no verb behind it");
    // The default is the opposite value, which is what made the old silence a
    // CLAIM rather than an absence. Pinned from a fresh descriptor so this fact
    // fails on its own if the struct's default ever moves.
    check(RadioCapabilities{}.hasFmRepeaterOffset,
          "hasFmRepeaterOffset still defaults TRUE — silence here is a claim");

    // ---- no tone encode ----
    check(caps.fmTonePresentation == FmTonePresentation::Hidden,
          "the tone controls are HIDDEN, not offered under the legacy shape");
    // What Legacy would have offered, read from the same function the widgets
    // populate the combo from rather than described in a comment. The point of
    // the assertion is that this list is not empty: Legacy is an OFFER of CTCSS
    // transmit, which is the thing that was untrue.
    check(legacyFmToneModes().contains(QStringLiteral("ctcss_tx")),
          "Legacy offers CTCSS ENCODE — that is what the HL2 was claiming");
    // Nothing on this radio populates a CTCSS vocabulary or DTCS code set, and
    // the Ctcss presentation is the only one that reads them. Empty here is
    // consistent with Hidden; a non-empty list would mean the declaration and
    // the data disagreed.
    check(caps.fmToneModes.isEmpty() && caps.fmDtcsCodes.isEmpty(),
          "and no tone vocabulary or DTCS codes are published to go with it");

    std::printf("%s: %d failure(s)\n", argv[0], failures);
    return failures == 0 ? 0 : 1;
}
