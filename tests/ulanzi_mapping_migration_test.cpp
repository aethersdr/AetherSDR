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
#include <QJsonDocument>
#include <QJsonObject>
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

    std::cout << (ok ? "ALL PASS" : "FAILURES") << '\n';
    return ok ? 0 : 1;
}
