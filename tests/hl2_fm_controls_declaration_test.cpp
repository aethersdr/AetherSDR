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
// connects nothing, pumps no event loop, and reaches no radio. It does read one
// production SOURCE file, through AETHER_SOURCE_DIR — see limitation 1 below
// for why, and rf_gain_presentation_test for the same shape already in use.
//
// WHAT THIS FILE CANNOT OBSERVE, said plainly because a declaration test that
// overstates its reach is worse than none:
//
//   1. fmTonePresentation's struct DEFAULT is already Hidden, so the assertion
//      that the HL2 REPORTS Hidden passes just as well against a backend that
//      says nothing at all — it cannot fail on a deletion, only on a change of
//      value. That is a property of the source TEXT, so the source text is
//      what the second tone assertion below reads, the way
//      rf_gain_presentation_test reads its production wiring — as a
//      whitespace-tolerant pattern rather than a literal substring, so a
//      reformat cannot turn it red while the declaration stands. What THAT in
//      turn cannot see is whether the statement it finds is the last one to
//      run: a second assignment further down would beat it unnoticed. The two
//      lines together are what the value assertion alone was claiming.
//      hasFmRepeaterOffset never had this problem — its default is the
//      OPPOSITE value, so deleting its line fails the value assertion
//      directly, and that is why the two facts are asserted separately.
//   2. That the two widgets actually honour these fields. VfoWidget::
//      configureFmToneControls and RxApplet::configureFmToneControls read them
//      directly — a coupling a reader can see in four lines — but reaching it
//      needs a constructed widget, and no test in this tree builds one of
//      these. Nothing here was measured on a radio either.

#include "TestSettingsProfile.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/hl2/Hl2Backend.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>

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

    // NO `caps.family` ASSERTION HERE, DELIBERATELY.
    // docs/architecture/radio-capabilities-map.md states the rule for these
    // targets -- "Every assertion reads a capability -- never `caps.family`,
    // never a backend type" -- on the ground that a family assertion would pass
    // just as happily against the anti-pattern the struct exists to prevent. A
    // family line here was a sanity check and not the subject, so dropping it
    // costs this target nothing; softening the rule to admit it would have cost
    // the rule. The constructed hl2::Hl2Backend above is what makes this the
    // HL2 descriptor; every line below reads a capability.

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
    // AND THAT IS THE WHOLE SAFETY ARGUMENT FOR DECLARING THEM ON
    // receiveModeControl, so the two facts are pinned side by side rather than
    // one of them being left to a comment. FM is on the RECEIVE control list
    // (an operator who picks it out of the mode menu can be steered back out;
    // before that it latched the slice shut) and on the RECEIVE-ONLY list (the
    // radio still refuses to key in it). Those are different lists read by
    // different code: receiveModeControl reaches ModelReceiveControlTarget and
    // RadioResourceAdapter and nothing on the transmit side, while
    // receiveOnlyModes is what RadioModel::refuseKeyInReceiveOnlyMode() reads
    // inside beginTxActivity(). If a future change ever moves FM off the
    // second list — which is what a real FM transmit chain landing would mean
    // — this line fails and the two declarations below become re-openable.
    check(caps.receiveModeControl
              && caps.receiveModeControl->modes.contains(QStringLiteral("FM")),
          "FM is STEERABLE and still unkeyable — receive control and receive-only agree");
    // NFM is on receiveOnlyModes and NOT on receiveModeControl, and the
    // asymmetry is deliberate rather than an oversight. receiveOnlyModes is a
    // membership test run on whatever string a slice holds, so it lists every
    // alias both ways and stays correct however the mode got there;
    // receiveModeControl is a REQUEST surface, and an alias on it would be
    // rewritten by Hl2Backend::setSliceMode and then never match the pending
    // observation ModelReceiveControlTarget is waiting for. Asserted, because
    // "the lists differ" is exactly the shape a careless edit would repair.
    check(!caps.receiveModeControl->modes.contains(QStringLiteral("NFM")),
          "and the NFM alias is receive-ONLY but not requestable — the two lists differ on purpose");

    // ---- no repeater duplex ----
    check(!caps.hasFmRepeaterOffset,
          "the HL2 declines the repeater offset: there is no verb behind it");
    // The default is the opposite value, which is what made the old silence a
    // CLAIM rather than an absence. Pinned from a fresh descriptor so this fact
    // fails on its own if the struct's default ever moves.
    check(RadioCapabilities{}.hasFmRepeaterOffset,
          "hasFmRepeaterOffset still defaults TRUE — silence here is a claim");

    // ---- no tone encode ----
    //
    // TWO ASSERTIONS, BECAUSE THE VALUE ALONE CANNOT FAIL ON A DELETION. The
    // struct default is Hidden too, so the first line pins what the HL2
    // REPORTS — which is what every consumer reads, and worth pinning — but it
    // is not evidence that the declaration was made. The second line is: it
    // reads the statement out of the production source, and it is the one that
    // fails if the assignment is deleted rather than changed.
    check(caps.fmTonePresentation == FmTonePresentation::Hidden,
          "the tone controls are HIDDEN, not offered under the legacy shape");
    QFile backendSource(
        QStringLiteral(AETHER_SOURCE_DIR "/src/core/backends/hl2/Hl2Backend.cpp"));
    check(backendSource.open(QIODevice::ReadOnly),
          "the production backend source is readable — the next assertion needs it");
    const QString backendText = QString::fromUtf8(backendSource.readAll());
    // WHITESPACE-TOLERANT ON PURPOSE. A literal substring match couples this
    // assertion to the FORMATTING of the statement, not to its existence: a
    // clang-format run that wraps after the `=` turns it red while the
    // declaration is entirely intact, and a red that means "the file was
    // reformatted" trains a reader to ignore the one that means "the
    // declaration is gone". The pattern is the same three tokens in the same
    // order, with any run of whitespace (newlines included) between them, so it
    // survives a reflow.
    //
    // AND IT IS ANCHORED AT LINE START, which is the other half and the one
    // that matters more. Unanchored, the pattern matches a COMMENTED-OUT copy
    // of the statement just as happily as a live one -- so the declaration
    // could be commented out, with the prose above it left in place explaining
    // why the HL2 declines tone encode, and this assertion would go on passing
    // while the backend inherited the struct default instead of stating it.
    // That is precisely the failure mode the assertion exists to catch, and it
    // is worse than the reflow one because the file still READS as correct.
    // `^\s*` with MultilineOption requires the statement to begin its own line
    // after nothing but indentation, so a leading `//` no longer satisfies it
    // while a wrapped-but-live statement still does.
    static const QRegularExpression kToneStatement(
        QStringLiteral(R"(^\s*c\.fmTonePresentation\s*=\s*FmTonePresentation::Hidden\s*;)"),
        QRegularExpression::MultilineOption);
    check(kToneStatement.match(backendText).hasMatch(),
          "and Hidden is STATED, not inherited from the struct's identical default");
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
