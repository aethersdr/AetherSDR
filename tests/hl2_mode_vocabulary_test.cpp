// The two HL2 mode lists, and the invariants between them.
//
// #5580: the mode combo offered Flex's compiled-in fallback set, so RTTY, DFM
// and DSTR sat on the menu of a radio that demodulates none of them —
// modeFromString() turns all three into USB while every readback still agrees
// the mode is RTTY. The operator sees a mode they chose and hears a mode they
// did not, and nothing in the path disagrees with them.
//
// WHY THIS TARGET IS SEPARATE. The natural home for a seam assertion is the
// fake-radio fixture in hl2_backend_test.cpp — which is RETIRED inside a
// commented block in tests/tests.cmake, so an assertion written there would be
// compiled by nothing and green forever. tests.cmake already states the rule
// beside hl2_pan_limits_declaration_test: "a declaration must not be pinned
// only inside something that does not build." Same reasoning, same shape.
//
// WHAT IS AND IS NOT PINNED HERE. The lists and the relations between them are,
// against the SAME accessors production reads rather than a retyped copy — a
// test carrying its own copy of the truth cannot detect the declaration and the
// code diverging. That the emit site in Hl2Backend::emitSliceState really
// assigns publishedModeStrings() to SliceDelta::modeList is a one-line coupling
// a reader can see and this file does not reach; the connected half of that
// seam lived in the retired fixture.
//
// SOCKET-FREE. Nothing binds, nothing connects, no event loop is pumped. The
// receive-only cross-check and the restore cross-check construct a backend and
// call capabilities() and applyRestoredState(), both available on a
// default-constructed one (see hl2_pan_limits_declaration_test).
//
// THE RESTORE HALF (#5755 review, jensenpat). Hiding a mode that is still
// ACCEPTED is only safe if the restore boundary reconciles it onto a spelling
// the menu carries. It did not, and a session saved in NFM came back with the
// slice holding "NFM" against a menu without it: RxApplet::connectSlice and
// VfoWidget::setSlice both rebuild the combo with clear()/addItems() and then
// findText(currentText), moving the selection only on a hit, so the combo sat
// at index 0 -- "LSB" -- with signals blocked and the receiver in FM.
//
// This file cannot instantiate those widgets, so it does not claim to have
// watched a combo. It pins the CONDITION they depend on, which is the half that
// lives below the GUI seam and the half this PR moved: what
// applyRestoredState() leaves in RestoredRadioState::mode -- the value
// pushInitialState() copies to Receiver::mode and emitSliceState() publishes as
// SliceDelta::mode -- is a member of the same publishedModeStrings() it
// publishes as SliceDelta::modeList. findText() cannot miss on a list that
// contains the string. Walked over every accepted spelling rather than NFM
// alone, so a future alias added to knownModeStrings() without a
// canonicalOfferedMode() row fails here instead of in an operator's session.

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2ModeVocabulary.h"
#include "core/backends/RestoredRadioState.h"

#include <QCoreApplication>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <utility>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const QString& label)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", qPrintable(label));
    if (!condition) {
        ++failures;
    }
}
}  // namespace

int main(int argc, char** argv)
{
    // The backend touches AppSettings on construction. Redirect it before
    // QCoreApplication so nothing here reads or writes the live configuration.
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-mode-vocabulary-test"));
    QCoreApplication app(argc, argv);

    const QStringList& accepted  = hl2::knownModeStrings();
    const QStringList& published = hl2::publishedModeStrings();

    check(!published.isEmpty(),
          QStringLiteral("a mode list is published at all — an empty one is the "
                         "bug, because both combos keep the compiled-in "
                         "FlexRadio fallback when it is empty"));

    // THE INVARIANT THE SPLIT EXISTS FOR. Offering a mode the restore boundary
    // would reject is the same fault seen from the other end.
    for (const QString& m : std::as_const(published)) {
        check(accepted.contains(m),
              QStringLiteral("%1 is offered and is an accepted spelling").arg(m));
        check(hl2::isKnownModeString(m.toLower()),
              QStringLiteral("%1 survives the restore guard case-insensitively").arg(m));
    }

    // One entry per mode, on both lists. An alias pair is invisible to a
    // contains() check on either member alone.
    check(QSet<QString>(published.begin(), published.end()).size() == published.size(),
          QStringLiteral("the published list has no duplicate entries"));
    check(QSet<QString>(accepted.begin(), accepted.end()).size() == accepted.size(),
          QStringLiteral("the accepted list has no duplicate entries"));

    // #5580's own three: not offered, and not accepted either, because
    // modeFromString() has no case for them and falls them back to USB.
    for (const QString& absent : {QStringLiteral("RTTY"), QStringLiteral("DFM"),
                                  QStringLiteral("DSTR")}) {
        check(!published.contains(absent),
              QStringLiteral("%1 is not offered").arg(absent));
        check(!accepted.contains(absent),
              QStringLiteral("%1 is not accepted on restore either").arg(absent));
    }

    // Accepted but NOT offered, each for its own reason — see
    // Hl2ModeVocabulary.h. This is the half of the split a future edit
    // collapsing the two lists back together would break first.
    for (const QString& hidden : {QStringLiteral("DRM"), QStringLiteral("WBFM"),
                                  QStringLiteral("WFM"), QStringLiteral("CWU"),
                                  QStringLiteral("NFM")}) {
        check(accepted.contains(hidden),
              QStringLiteral("%1 is still accepted, so a stored document keeps it")
                  .arg(hidden));
        check(!published.contains(hidden),
              QStringLiteral("%1 is not offered in the menu").arg(hidden));
    }

    // Alias pairs: exactly one member of each reaches the menu.
    const QList<QPair<QString, QString>> aliases = {
        {QStringLiteral("CW"), QStringLiteral("CWU")},
        {QStringLiteral("FM"), QStringLiteral("NFM")},
        {QStringLiteral("WBFM"), QStringLiteral("WFM")},
    };
    for (const auto& pair : aliases) {
        const int offered = int(published.contains(pair.first))
                          + int(published.contains(pair.second));
        check(offered <= 1,
              QStringLiteral("%1/%2 are one mode under two names and are not "
                             "both offered").arg(pair.first, pair.second));
    }

    for (const QString& kept : {QStringLiteral("USB"), QStringLiteral("LSB"),
                                QStringLiteral("CW"),  QStringLiteral("CWL"),
                                QStringLiteral("DSB"), QStringLiteral("DIGU"),
                                QStringLiteral("DIGL")}) {
        check(published.contains(kept), QStringLiteral("%1 is offered").arg(kept));
    }

    // NOTHING IS OFFERED WITHOUT A DECLARED TRANSMIT DISPOSITION. Every mode on
    // the menu is either one this radio's modulator can key, or one
    // capabilities() declares receive-only so RadioModel::refuseKeyInReceiveOnly
    // Mode() stops the key. A mode that is neither would be keyable and would
    // transmit an SSB signal labelled something else — the question #5755's
    // review raised, answered against the declaration rather than by assertion.
    hl2::Hl2Backend backend;
    const RadioCapabilities caps = backend.capabilities();
    static const QStringList kTransmittable = {
        QStringLiteral("LSB"), QStringLiteral("USB"), QStringLiteral("CW"),
        QStringLiteral("CWL"), QStringLiteral("DIGU"), QStringLiteral("DIGL"),
    };
    for (const QString& m : std::as_const(published)) {
        check(kTransmittable.contains(m) || caps.receiveOnlyModes.contains(m),
              QStringLiteral("%1 is offered and is either transmittable or "
                             "declared receive-only").arg(m));
    }
    // And the declaration is not vacuous: at least one offered mode is on it.
    check(!caps.receiveOnlyModes.isEmpty(),
          QStringLiteral("receiveOnlyModes is declared at all (#5680)"));

    // ── The restore boundary reconciles what it accepts ──────────────────────
    //
    // ACCEPTED BUT UNDISPLAYABLE, named rather than tolerated. WBFM and DRM
    // have no offered twin to collapse onto, so a document holding either still
    // restores to a mode the menu cannot show. Neither is a regression of
    // #5755: neither was in the FlexRadio fallback list this replaces, so a
    // slice holding one already displayed index 0 before it, and reaching them
    // needs CAT, TCI or a hand-edited document. If this set ever grows, the
    // growth is the finding.
    static const QStringList kAcceptedButUndisplayable = {
        QStringLiteral("WBFM"), QStringLiteral("DRM"),
    };

    // canonicalOfferedMode() as a pure function first, so a failure below
    // separates "the rule is wrong" from "the boundary does not apply it".
    check(hl2::canonicalOfferedMode(QStringLiteral("NFM")) == QLatin1String("FM"),
          QStringLiteral("NFM reconciles onto FM — the entry the menu carries"));
    check(hl2::canonicalOfferedMode(QStringLiteral("nfm")) == QLatin1String("FM"),
          QStringLiteral("and case-insensitively, like the guard beside it"));
    check(hl2::canonicalOfferedMode(QStringLiteral("CWU")) == QLatin1String("CW"),
          QStringLiteral("CWU reconciles onto CW"));
    check(hl2::canonicalOfferedMode(QStringLiteral("USB")) == QLatin1String("USB"),
          QStringLiteral("an offered mode is its own canonical spelling"));

    // NOT A TX LOOPHOLE. modeIsReceiveOnly() is a case-insensitive MEMBERSHIP
    // test, not alias normalisation, so a reconciliation that moved a spelling
    // from a listed name to an unlisted one would delete a TX refusal on a mode
    // still reachable over CAT. Every accepted spelling must therefore agree
    // with its canonical form about receiveOnlyModes — checked, because the
    // list in capabilities() and the rule in the header are edited separately.
    for (const QString& m : std::as_const(accepted)) {
        const bool before = caps.receiveOnlyModes.contains(m, Qt::CaseInsensitive);
        const bool after  = caps.receiveOnlyModes.contains(
            hl2::canonicalOfferedMode(m), Qt::CaseInsensitive);
        check(before == after,
              QStringLiteral("reconciling %1 -> %2 keeps its transmit disposition")
                  .arg(m, hl2::canonicalOfferedMode(m)));
    }

    // THE REGRESSION ITSELF, through the production boundary rather than the
    // rule: a stored document saying NFM must come back as a mode the published
    // list contains, or the combo cannot display what the slice is in.
    //
    // ONE BACKEND for every document below. applyRestoredState() resets its
    // state in full before validating -- "applyRestoredState({}) is a
    // legitimate call meaning this radio has no memory", which RadioModel makes
    // on every engaged connect -- so reusing it is what production does, and a
    // value leaking from one document into the next would be a finding of its
    // own rather than an artefact of this loop.
    hl2::Hl2Backend restoreBackend;
    const auto restoreAs = [&restoreBackend](const QString& stored) {
        RestoredRadioState doc;
        doc.mode = stored;
        restoreBackend.applyRestoredState(doc);
        return restoreBackend.restoredStateForTest().mode;
    };
    {
        const QString restored = restoreAs(QStringLiteral("NFM"));
        check(restored == QLatin1String("FM"),
              QStringLiteral("a session saved in NFM restores as FM — got "
                             "\"%1\"").arg(restored));
        check(published.contains(restored),
              QStringLiteral("and the menu can display it — findText(\"%1\") "
                             "hits the published list").arg(restored));
    }

    // The guard is not widened by the reconciliation. A mode modeFromString()
    // does not map is still DROPPED, exactly as PR #4619's review requires:
    // canonicalOfferedMode() runs after isKnownModeString(), never instead of
    // it. This is the negative control for the two checks above — without it
    // they would also pass if the boundary simply accepted everything.
    for (const QString& rejected : {QStringLiteral("RTTY"), QStringLiteral("DFM"),
                                    QStringLiteral("DSTR"), QStringLiteral("NONSENSE")}) {
        check(restoreAs(rejected).isEmpty(),
              QStringLiteral("%1 is still dropped at the restore boundary")
                  .arg(rejected));
    }

    // EVERY accepted spelling, not just the one the review found. A document
    // may hold any of them; each must come back displayable, or be one of the
    // two named above as having no offered twin.
    for (const QString& m : std::as_const(accepted)) {
        const QString restored = restoreAs(m);
        check(published.contains(restored)
                  || kAcceptedButUndisplayable.contains(restored),
              QStringLiteral("a document holding %1 restores as %2, which the "
                             "menu can display (or is a named residual)")
                  .arg(m, restored));
    }

    if (failures == 0) {
        std::printf("hl2_mode_vocabulary_test: all checks passed\n");
        return 0;
    }
    std::printf("hl2_mode_vocabulary_test: %d failure(s)\n", failures);
    return 1;
}
