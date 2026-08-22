// Ulanzi Dial mapping store — one-shot claim of the pre-#4611 flat keys.
//
// The property under test is the one the house migration idiom exists to
// guarantee (AGENTS.md "Settings Migration"; same shape as the LocalMemoryBank
// legacy import): after migration the JSON document is the SOLE authority, so
// a binding the operator later clears stays cleared instead of being
// resurrected by a stale `UlanziDial_action_*` row.
//
// Break it on purpose to see these earn their keep: skip the `s.remove()`
// calls in migrateLegacyKeys() and "legacy keys are gone" fails; consult the
// legacy key from the read path and "a cleared binding stays cleared" fails.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/UlanziDialMappings.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

QJsonObject storedDocument()
{
    const QByteArray raw = AppSettings::instance()
                               .value(UlanziDialMappings::rootSettingsKey())
                               .toString().toUtf8();
    return QJsonDocument::fromJson(raw).object();
}

const QStringList kPills = {QStringLiteral("top_left"), QStringLiteral("top_middle"),
                            QStringLiteral("side_rt"),  QStringLiteral("dial_press")};

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("ulanzi-mapping-migration-test"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        std::cout << "[FAIL] could not create an isolated settings profile\n";
        return 1;
    }

    bool ok = true;
    auto& s = AppSettings::instance();
    s.load();

    // ── a legacy binding is adopted, and its flat key deleted ─────────────
    s.setValue(UlanziDialMappings::legacyUnderscoreKey(QStringLiteral("top_left")),
               QStringLiteral("shortcut:mox_toggle"));
    s.setValue(UlanziDialMappings::legacySlashKey(QStringLiteral("side_rt")),
               QStringLiteral("shortcut:ptt_hold"));
    s.save();

    const int adopted = UlanziDialMappings::migrateLegacyKeys(kPills);
    ok &= expect(adopted == 2, "both legacy key forms are adopted");
    ok &= expect(UlanziDialMappings::actionForPill(QStringLiteral("top_left"))
                     == QStringLiteral("shortcut:mox_toggle"),
                 "underscore-form legacy value lands in the document");
    ok &= expect(UlanziDialMappings::actionForPill(QStringLiteral("side_rt"))
                     == QStringLiteral("shortcut:ptt_hold"),
                 "slash-form legacy value lands in the document");
    ok &= expect(!s.contains(UlanziDialMappings::legacyUnderscoreKey(QStringLiteral("top_left")))
                     && !s.contains(UlanziDialMappings::legacySlashKey(QStringLiteral("side_rt"))),
                 "legacy keys are gone after the claim");

    // ── the document survives a restart ───────────────────────────────────
    s.reset();
    s.load();
    ok &= expect(UlanziDialMappings::actionForPill(QStringLiteral("top_left"))
                     == QStringLiteral("shortcut:mox_toggle"),
                 "the migrated document reloads from disk");

    // ── migration is one-shot: a second run is a no-op ────────────────────
    ok &= expect(UlanziDialMappings::migrateLegacyKeys(kPills) == 0,
                 "a second migration pass adopts nothing");

    // ── a cleared binding is NOT resurrected ──────────────────────────────
    // The failure the one-shot claim exists to prevent: with a permanent
    // legacy fallback, clearing a mapping would let the old flat key answer
    // the next lookup.
    ok &= expect(UlanziDialMappings::setActionForPill(QStringLiteral("top_left"),
                                                      QStringLiteral("None")),
                 "clearing a binding persists to disk");
    ok &= expect(UlanziDialMappings::actionForPill(QStringLiteral("top_left"))
                     == QStringLiteral("None"),
                 "a cleared binding stays cleared");

    // Even a hand-restored legacy row (an old backup, a --config set) is
    // ignored on the read path; it only gets swept away by the next claim.
    s.setValue(UlanziDialMappings::legacyUnderscoreKey(QStringLiteral("top_left")),
               QStringLiteral("shortcut:tune_toggle"));
    s.save();
    ok &= expect(UlanziDialMappings::actionForPill(QStringLiteral("top_left"))
                     == QStringLiteral("None"),
                 "a resurrected legacy row does not override the document");
    UlanziDialMappings::migrateLegacyKeys(kPills);
    ok &= expect(UlanziDialMappings::actionForPill(QStringLiteral("top_left"))
                     == QStringLiteral("None"),
                 "and the claim does not let it back in either");
    ok &= expect(!s.contains(UlanziDialMappings::legacyUnderscoreKey(QStringLiteral("top_left"))),
                 "the resurrected legacy row is swept away");

    // ── an unbound pill reports nothing, so the caller's default applies ──
    ok &= expect(UlanziDialMappings::actionForPill(QStringLiteral("top_middle")).isEmpty(),
                 "a pill with no entry reports empty, not a stale value");

    // ── whole-document write: other pills are preserved ───────────────────
    UlanziDialMappings::setActionForPill(QStringLiteral("dial_press"),
                                         QStringLiteral("shortcut:mute_toggle"));
    const QJsonObject doc = storedDocument();
    ok &= expect(doc.contains(QStringLiteral("top_left"))
                     && doc.contains(QStringLiteral("side_rt"))
                     && doc.contains(QStringLiteral("dial_press")),
                 "binding one pill preserves the others");

    // ── a corrupt document does not wedge the feature ─────────────────────
    s.setValue(UlanziDialMappings::rootSettingsKey(), QStringLiteral("{not json"));
    s.save();
    ok &= expect(UlanziDialMappings::actionForPill(QStringLiteral("side_rt")).isEmpty(),
                 "a corrupt document reads as empty rather than throwing garbage");
    ok &= expect(UlanziDialMappings::setActionForPill(QStringLiteral("side_rt"),
                                                      QStringLiteral("shortcut:next_slice")),
                 "a corrupt document is replaced by the next successful write");
    ok &= expect(UlanziDialMappings::actionForPill(QStringLiteral("side_rt"))
                     == QStringLiteral("shortcut:next_slice"),
                 "and the rebuilt document reads back");

    // ── rotaryAction tests (Principle V) ──────────────────────────────────
    ok &= expect(UlanziDialMappings::rotaryAction() == QStringLiteral("WheelFrequency"),
                 "default rotaryAction is WheelFrequency");
    ok &= expect(UlanziDialMappings::setRotaryAction(QStringLiteral("WheelVolume")),
                 "setting rotaryAction persists to document");
    ok &= expect(UlanziDialMappings::rotaryAction() == QStringLiteral("WheelVolume"),
                 "rotaryAction reads back from document");

    // Legacy UlanziDialRotaryAction flat key migration
    s.setValue(QStringLiteral("UlanziDialRotaryAction"), QStringLiteral("WheelPower"));
    s.save();

    // Explicit empty rotary_action in document returns default WheelFrequency rather than adopting legacy key
    UlanziDialMappings::setActionForPill(QStringLiteral("rotary_action"), QString());
    ok &= expect(UlanziDialMappings::rotaryAction() == QStringLiteral("WheelFrequency"),
                 "explicit empty rotary_action in document returns default WheelFrequency");

    // When rotary_action key is missing from document, legacy flat key is adopted
    s.remove(UlanziDialMappings::rootSettingsKey());
    s.save();
    s.setValue(QStringLiteral("UlanziDialRotaryAction"), QStringLiteral("WheelPower"));
    s.save();
    ok &= expect(UlanziDialMappings::rotaryAction() == QStringLiteral("WheelPower"),
                 "legacy flat key UlanziDialRotaryAction is adopted when document key is missing");
    ok &= expect(!s.contains(QStringLiteral("UlanziDialRotaryAction")),
                 "legacy flat key UlanziDialRotaryAction is removed after adoption");

    // ── mapping-layer round-trip: unrecognized action id is preserved ─────
    // Intentionally limited to settings-store persistence and round-tripping:
    // verify setRotaryAction accepts and persists an unrecognized action id,
    // and survives a reload as-is without silent fallback or mutation at
    // the settings store layer so callers receive the exact stored string.
    ok &= expect(UlanziDialMappings::setRotaryAction(QStringLiteral("WheelCorruptAction4756")),
                 "setting unrecognized rotary action id succeeds");
    s.reset();
    s.load();
    ok &= expect(UlanziDialMappings::rotaryAction() == QStringLiteral("WheelCorruptAction4756"),
                 "unrecognized rotary action id survives reload and round-trips as-is");

    // ── dispatch seam: unrecognized action id is not a known wheel action ─
    ok &= expect(!UlanziDialMappings::isKnownWheelAction(UlanziDialMappings::rotaryAction()),
                 "stored unrecognized rotary action is not a known wheel action");
    ok &= expect(!UlanziDialMappings::isKnownWheelAction(QStringLiteral("NotAnAction")),
                 "'NotAnAction' is not a known wheel action");
    const QStringList& known = UlanziDialMappings::knownWheelActions();
    bool allKnown = true;
    for (const QString& action : known) {
        if (!UlanziDialMappings::isKnownWheelAction(action)) {
            allKnown = false;
            break;
        }
    }
    ok &= expect(allKnown, "every action in knownWheelActions registry is recognized by isKnownWheelAction");

    // ── drift detection: assert every actionId handled by applyFlexControlWheelAction is registered ─
    // Loads applyFlexControlWheelAction's body out of the source. Shared by
    // the drift check below and the #4658 guard check after it.
    auto loadWheelDispatchBody = []() -> QString {
        // AETHER_SOURCE_DIR comes from CMake so this works for an
        // out-of-source build too; the relative forms are the fallback for a
        // hand-compiled run from the source root or a build directory.
        QStringList candidates = {
            QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_Controllers.cpp"),
            QStringLiteral("src/gui/MainWindow_Controllers.cpp"),
            QStringLiteral("../src/gui/MainWindow_Controllers.cpp"),
            QStringLiteral("../../src/gui/MainWindow_Controllers.cpp")
        };
        QFile file;
        for (const QString& cand : candidates) {
            file.setFileName(cand);
            if (file.exists()) break;
        }
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        const QString content = QString::fromUtf8(file.readAll());
        file.close();

        // Anchor on the DEFINITION, not the first mention: the name also
        // appears at ~:370 as a call inside handleFlexControlTuneSteps, and
        // anchoring there would sweep a thousand lines of unrelated functions
        // into the window and attribute their action-id comparisons to this
        // one.
        const int start = content.indexOf(
            QStringLiteral("void MainWindow::applyFlexControlWheelAction("));
        if (start < 0) return {};
        // Bound on the closing brace in column 0, or give up. A fixed-size
        // window would silently cover part of the chain and still report a
        // pass, which is worse than no check at all.
        const int end = content.indexOf(QStringLiteral("\n}\n"), start);
        if (end < 0) return {};
        return content.mid(start, end - start);
    };

    auto extractDispatchActions = [&loadWheelDispatchBody]() -> QStringList {
        const QString fnBody = loadWheelDispatchBody();
        if (fnBody.isEmpty()) return {};

        // The optional wrapper matters: `actionId == QLatin1String("...")` is
        // used in this very file at ~:2549 and is the dominant form in
        // FlexControlDialog.cpp and HidEncoderManager.cpp. Matching only bare
        // literals would let a branch written that way escape both the
        // registry and this check, and the guard would then drop it silently.
        static const QRegularExpression rx(QStringLiteral(
            "actionId\\s*==\\s*(?:QLatin1String\\(|QStringLiteral\\()?\"([^\"]+)\""));
        QRegularExpressionMatchIterator it = rx.globalMatch(fnBody);
        QStringList list;
        while (it.hasNext()) {
            list.append(it.next().captured(1));
        }
        return list;
    };

    const QStringList dispatchActions = extractDispatchActions();
    ok &= expect(!dispatchActions.isEmpty(),
                 "extracted dispatch action IDs from MainWindow_Controllers.cpp applyFlexControlWheelAction");
    bool allDispatchRegistered = true;
    for (const QString& act : dispatchActions) {
        if (!UlanziDialMappings::isKnownWheelAction(act)) {
            std::cout << "[FAIL] Action '" << act.toStdString()
                      << "' in applyFlexControlWheelAction is missing from UlanziDialMappings::knownWheelActions()\n";
            allDispatchRegistered = false;
        }
    }
    ok &= expect(allDispatchRegistered,
                 "every actionId handled in applyFlexControlWheelAction is registered in UlanziDialMappings::knownWheelActions()");

    // And the other direction, so the two are one set rather than one subset.
    // A registry entry with no branch left behind by a removal is a dead id
    // the guard would wave through; it also replaces a hand-counted size
    // assertion that had to be edited every time an action was added.
    bool allRegisteredDispatched = true;
    for (const QString& action : known) {
        if (!dispatchActions.contains(action)) {
            std::cout << "[FAIL] Action '" << action.toStdString()
                      << "' in UlanziDialMappings::knownWheelActions() has no branch in applyFlexControlWheelAction\n";
            allRegisteredDispatched = false;
        }
    }
    ok &= expect(allRegisteredDispatched,
                 "every action in UlanziDialMappings::knownWheelActions() is handled in applyFlexControlWheelAction");

    // ── #4658: the WheelApf branch must bail on a disengaged filter before it
    // writes apf_level. A level written into a filter that is not in circuit
    // reaches no audio, and on a TMate 2 the overlay then confirms a change
    // that never happened. Source-level, like the drift check above, so the
    // guard survives a refactor of the else-if chain without a GUI harness.
    {
        const QString fnBody = loadWheelDispatchBody();
        // Same optional-wrapper tolerance as the drift check: a branch written
        // as `actionId == QLatin1String("WheelApf")` must not fail this pin.
        static const QRegularExpression apfRx(QStringLiteral(
            "actionId\\s*==\\s*(?:QLatin1String\\(|QStringLiteral\\()?\"WheelApf\""));
        const int apfStart = fnBody.indexOf(apfRx);
        int apfEnd = apfStart < 0 ? -1
                   : fnBody.indexOf(QStringLiteral("} else if (actionId"), apfStart + 1);
        if (apfStart >= 0 && apfEnd < 0) apfEnd = fnBody.size();
        const QString apfBranch = apfStart < 0 ? QString() : fnBody.mid(apfStart, apfEnd - apfStart);
        // The literal negated test, not any mention of apfOn(): an inverted
        // guard (`if (s->apfOn())`) or a comment naming the accessor must not
        // satisfy this pin.
        const int guard = apfBranch.indexOf(QStringLiteral("if (!s->apfOn())"));
        const int write = apfBranch.indexOf(QStringLiteral("setApfLevel("));
        const QString guardRegion = (apfStart >= 0 && guard >= 0 && write > guard)
            ? apfBranch.mid(guard, write - guard) : QString();
        const bool guarded = !guardRegion.isEmpty() && guardRegion.contains(QStringLiteral("return;"));
        ok &= expect(guarded,
                     "WheelApf branch checks apfOn() and returns before setApfLevel() (#4658)");
        // The guard must stay OUTSIDE any #ifdef: the overlay is HAVE_HIDAPI-only,
        // the write is not, and a guard moved inside that block would silently
        // regress every non-hidapi build while the positional check above still
        // passed. "Outside" = every #if opened before the guard line is closed
        // again before it; the overlay's own #ifdef inside the guard body is fine.
        const QString beforeGuard = apfStart < 0 || guard < 0 ? QString() : apfBranch.left(guard);
        const bool guardOutsideIfdef = guarded
            && beforeGuard.count(QStringLiteral("#if")) == beforeGuard.count(QStringLiteral("#endif"));
        ok &= expect(guardOutsideIfdef,
                     "WheelApf guard is not inside a preprocessor conditional (#4658)");
    }

    std::cout << (ok ? "ALL PASS" : "FAILURES") << '\n';
    return ok ? 0 : 1;
}
