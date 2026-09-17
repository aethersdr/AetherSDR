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
// receive-only cross-check constructs a backend and reads capabilities(), which
// is available on a default-constructed one (see hl2_pan_limits_declaration_test).

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2ModeVocabulary.h"

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

    if (failures == 0) {
        std::printf("hl2_mode_vocabulary_test: all checks passed\n");
        return 0;
    }
    std::printf("hl2_mode_vocabulary_test: %d failure(s)\n", failures);
    return 1;
}
