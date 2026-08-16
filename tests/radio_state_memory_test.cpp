// RadioStateMemory + the radio-scoped feature-document store (RFC #4603 PR 2).
//
// The invariants under test:
//  - engagement is capability-shaped: empty ClientSettingsDomains ⇒ inert
//    (nothing stored, nothing loaded — the Flex/Sim guarantee)
//  - the operating-state document round-trips, atomically, per (family, radio)
//  - load is gated per DECLARED domain, so a capability downgrade cannot
//    smuggle state past the gate even when an older document carries it
//  - reads fall back exact-radio → family-wide → empty
//  - two radios of the same family never share state
//  - a newer document schema still yields its known fields
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/RadioStateMemory.h"
#include "core/SettingsDatabase.h"
#include "core/SettingsPaths.h"

#include <QCoreApplication>
#include <QJsonObject>

#include <iostream>

using namespace AetherSDR;
using Domain = RadioCapabilities::ClientSettingsDomain;

namespace {

int g_failures = 0;

void check(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    if (!condition) {
        ++g_failures;
    }
}

RadioCapabilities hl2Caps()
{
    RadioCapabilities caps;
    caps.family = QStringLiteral("hl2");
    caps.clientSettingsDomains = Domain::Tuning | Domain::Passband
                                 | Domain::SpanRate | Domain::RfGain
                                 | Domain::TxSetpoints | Domain::Agc;
    return caps;
}

RestoredRadioState sampleState()
{
    RestoredRadioState state;
    state.rfFrequencyHz = 7'074'000.0;
    state.mode = QStringLiteral("USB");
    state.filterLowHz = 100.0;
    state.filterHighHz = 2'900.0;
    state.sampleRateHz = 192'000;
    state.agcMode = QStringLiteral("slow");
    state.agcThreshold = 40;
    state.extensionSchemaVersion = 1;
    // The extension's top level is domain sub-objects (the per-domain gate);
    // each sub-object's contents are backend-owned.
    state.extension = QJsonObject{
        {QStringLiteral("rfGain"),
         QJsonObject{{QStringLiteral("lnaDbByBand"),
                      QJsonObject{{QStringLiteral("40m"), 19}}}}},
        {QStringLiteral("txSetpoints"),
         QJsonObject{{QStringLiteral("driveByBand"),
                      QJsonObject{{QStringLiteral("40m"), 42}}}}}};
    return state;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-radio-state-memory-test"));
    if (!profile.isValid()) {
        std::cerr << "[FAIL] create temporary home\n";
        return 1;
    }
    QCoreApplication app(argc, argv);

    auto& settings = AppSettings::instance();
    settings.load();

    const RadioSettingsScope radioA(QStringLiteral("hl2"),
                                    QStringLiteral("AA:BB:CC:DD:EE:FF"));
    const RadioSettingsScope radioB(QStringLiteral("hl2"),
                                    QStringLiteral("AA:BB:CC:DD:EE:00"));

    // ---- inert for an empty declaration (the Flex/Sim guarantee) ----------
    {
        RadioCapabilities flexLike;
        flexLike.family = QStringLiteral("flex");
        check(!RadioStateMemory::shouldEngage(flexLike),
              "empty domain declaration does not engage");
        check(!RadioStateMemory::store(radioA, flexLike, sampleState()),
              "store with an empty declaration writes nothing");
        check(RadioStateMemory::load(radioA, flexLike).isEmpty(),
              "load with an empty declaration restores nothing");
        check(radioA.feature(RadioStateMemory::featureName()).isEmpty(),
              "no document exists after the inert store");
    }

    // ---- round-trip per radio --------------------------------------------
    {
        const RadioCapabilities caps = hl2Caps();
        check(RadioStateMemory::shouldEngage(caps), "declared domains engage");
        check(RadioStateMemory::store(radioA, caps, sampleState()),
              "store succeeds for a declared backend");

        const RestoredRadioState restored = RadioStateMemory::load(radioA, caps);
        check(restored.rfFrequencyHz == 7'074'000.0
                  && restored.mode == QStringLiteral("USB"),
              "tuning round-trips");
        check(restored.filterLowHz == 100.0 && restored.filterHighHz == 2'900.0,
              "passband round-trips");
        check(restored.sampleRateHz == 192'000, "span/rate round-trips");
        check(restored.agcMode == QStringLiteral("slow")
                  && restored.agcThreshold == 40,
              "AGC mode and threshold round-trip (#4909)");
        check(restored.extensionSchemaVersion == 1
                  && restored.extension.value(QStringLiteral("rfGain"))
                             .toObject()
                             .value(QStringLiteral("lnaDbByBand"))
                             .toObject()
                             .value(QStringLiteral("40m"))
                             .toInt()
                         == 19
                  && restored.extension
                         .contains(QStringLiteral("txSetpoints")),
              "the extension document round-trips (contents opaque)");
    }

    // ---- two radios of one family stay independent ------------------------
    {
        const RadioCapabilities caps = hl2Caps();
        RestoredRadioState other = sampleState();
        other.rfFrequencyHz = 14'074'000.0;
        check(RadioStateMemory::store(radioB, caps, other),
              "second radio stores its own document");
        check(RadioStateMemory::load(radioA, caps).rfFrequencyHz == 7'074'000.0,
              "radio A keeps its own frequency");
        check(RadioStateMemory::load(radioB, caps).rfFrequencyHz == 14'074'000.0,
              "radio B keeps its own frequency");
    }

    // ---- per-domain gating on load ----------------------------------------
    {
        RadioCapabilities tuningOnly;
        tuningOnly.family = QStringLiteral("hl2");
        tuningOnly.clientSettingsDomains = Domain::Tuning;
        const RestoredRadioState gated = RadioStateMemory::load(radioA, tuningOnly);
        check(gated.rfFrequencyHz == 7'074'000.0 && gated.mode == "USB",
              "a declared domain loads");
        check(gated.filterLowHz == 0.0 && gated.filterHighHz == 0.0
                  && gated.sampleRateHz == 0 && gated.extension.isEmpty(),
              "undeclared domains stay 'not restored' even though the stored "
              "document carries them");
        // The AGC threshold's "not restored" is -1, not 0: zero is a value the
        // operator can select, so a gated-out domain must not look like a
        // deliberate AGC-T of 0.
        check(gated.agcMode.isEmpty() && gated.agcThreshold == -1,
              "an undeclared Agc domain is absent, not a threshold of 0");
    }

    // ---- a threshold of ZERO survives the round-trip -----------------------
    // The sentinel is -1 precisely so this case works: with 0 as "absent" an
    // operator who ran the AGC-T at the bottom of the slider would get 65 back.
    {
        const RadioCapabilities caps = hl2Caps();
        const RadioSettingsScope zeroRadio(QStringLiteral("hl2"),
                                           QStringLiteral("00:00:00:00:00:0A"));
        RestoredRadioState state = sampleState();
        state.agcMode = QStringLiteral("off");
        state.agcThreshold = 0;
        check(RadioStateMemory::store(zeroRadio, caps, state),
              "a zero AGC threshold stores");
        const RestoredRadioState back = RadioStateMemory::load(zeroRadio, caps);
        check(back.agcThreshold == 0 && back.agcMode == QStringLiteral("off"),
              "a deliberate AGC threshold of 0 is not mistaken for 'absent'");
    }

    // ---- per-domain gating on store ---------------------------------------
    {
        RadioCapabilities tuningOnly;
        tuningOnly.family = QStringLiteral("hl2");
        tuningOnly.clientSettingsDomains = Domain::Tuning;
        RestoredRadioState state = sampleState();
        check(RadioStateMemory::store(radioB, tuningOnly, state),
              "tuning-only store succeeds");
        const RestoredRadioState back = RadioStateMemory::load(radioB, hl2Caps());
        check(back.rfFrequencyHz == 7'074'000.0 && back.sampleRateHz == 0
                  && back.extension.isEmpty(),
              "an undeclared domain is never written, so a later full "
              "declaration finds nothing to restore for it");
    }

    // ---- family-wide fallback ---------------------------------------------
    {
        const RadioCapabilities caps = hl2Caps();
        const RadioSettingsScope familyWide(QStringLiteral("hl2"), QString());
        RestoredRadioState familyDefault = sampleState();
        familyDefault.rfFrequencyHz = 10'000'000.0;
        check(RadioStateMemory::store(familyWide, caps, familyDefault),
              "family-wide default document stores");
        const RadioSettingsScope unseenRadio(QStringLiteral("hl2"),
                                             QStringLiteral("11:22:33:44:55:66"));
        check(RadioStateMemory::load(unseenRadio, caps).rfFrequencyHz
                  == 10'000'000.0,
              "an unseen radio falls back to the family-wide default");
        check(RadioStateMemory::load(radioA, caps).rfFrequencyHz == 7'074'000.0,
              "a radio with its own document is NOT shadowed by the family row");
    }

    // ---- newer document schema still yields known fields -------------------
    {
        const RadioCapabilities caps = hl2Caps();
        QJsonObject future{{QStringLiteral("rfFrequencyHz"), 21'074'000.0},
                           {QStringLiteral("mode"), QStringLiteral("DIGU")},
                           {QStringLiteral("fieldFromTheFuture"), true}};
        const RadioSettingsScope futureRadio(QStringLiteral("hl2"),
                                             QStringLiteral("FE:ED:FA:CE:00:01"));
        check(futureRadio.setFeature(RadioStateMemory::featureName(),
                                     RadioStateMemory::kSchemaVersion + 1, future),
              "a newer-schema document can be planted");
        check(RadioStateMemory::load(futureRadio, caps).rfFrequencyHz
                  == 21'074'000.0,
              "known fields load from a newer-schema document");
    }

    // ---- the extension gate splits per domain (PR #4614 review) -----------
    // The regression K5PTB specified: RfGain-only declaration, stored ext
    // carrying both maps — TxSetpoints data must NOT come back.
    {
        RadioCapabilities rfGainOnly;
        rfGainOnly.family = QStringLiteral("hl2");
        rfGainOnly.clientSettingsDomains = Domain::RfGain;
        const RestoredRadioState gated = RadioStateMemory::load(radioA, rfGainOnly);
        check(gated.extension.contains(QStringLiteral("rfGain")),
              "a declared ext domain's sub-object loads");
        check(!gated.extension.contains(QStringLiteral("txSetpoints")),
              "an undeclared ext domain's sub-object is NOT handed over");
    }

    // ---- store is read-only toward newer documents (PR #4614 review) ------
    // The load→capture→store cycle against a v2 document must refuse, not
    // truncate-and-downgrade.
    {
        const RadioCapabilities caps = hl2Caps();
        const RadioSettingsScope newerRadio(QStringLiteral("hl2"),
                                            QStringLiteral("FE:ED:FA:CE:00:02"));
        QJsonObject future{{QStringLiteral("rfFrequencyHz"), 28'074'000.0},
                           {QStringLiteral("v2Field"), QStringLiteral("keep")}};
        check(newerRadio.setFeature(RadioStateMemory::featureName(),
                                    RadioStateMemory::kSchemaVersion + 1, future),
              "a newer-schema document is planted");
        RestoredRadioState captured = RadioStateMemory::load(newerRadio, caps);
        check(captured.rfFrequencyHz == 28'074'000.0,
              "the newer document's known fields load");
        captured.rfFrequencyHz = 28'500'000.0;
        check(!RadioStateMemory::store(newerRadio, caps, captured),
              "store REFUSES to overwrite a newer-schema document");
        int storedVersion = 0;
        const QJsonObject after =
            newerRadio.feature(RadioStateMemory::featureName(), &storedVersion);
        check(storedVersion == RadioStateMemory::kSchemaVersion + 1
                  && after.value(QStringLiteral("v2Field")).toString()
                         == QStringLiteral("keep")
                  && after.value(QStringLiteral("rfFrequencyHz")).toDouble()
                         == 28'074'000.0,
              "the newer document survives byte-for-byte — no truncation, no "
              "version downgrade");
    }

    // ---- a newer FAMILY-WIDE row never blocks per-radio capture -----------
    // (PR #4614 re-review, Ozy311): the store guard must judge the exact row
    // it replaces — the fallback would let a newer family-wide default refuse
    // every per-radio capture in the family.
    {
        const RadioCapabilities caps = hl2Caps();
        const RadioSettingsScope familyWide(QStringLiteral("hl2"), QString());
        QJsonObject newerFamilyDoc{{QStringLiteral("rfFrequencyHz"), 1'840'000.0}};
        check(familyWide.setFeature(RadioStateMemory::featureName(),
                                    RadioStateMemory::kSchemaVersion + 1,
                                    newerFamilyDoc),
              "a newer-schema FAMILY-WIDE document is planted");
        const RadioSettingsScope freshRadio(QStringLiteral("hl2"),
                                            QStringLiteral("DE:AD:BE:EF:00:03"));
        RestoredRadioState state = sampleState();
        check(RadioStateMemory::store(freshRadio, caps, state),
              "a per-radio capture still succeeds under a newer family row");
        check(RadioStateMemory::load(freshRadio, caps).rfFrequencyHz
                  == 7'074'000.0,
              "the per-radio document was really written");
        // Clean up the newer family row so later cases keep their semantics.
        check(familyWide.removeFeature(RadioStateMemory::featureName()),
              "the planted family row is removed again");
    }

    // ---- a corrupt document is loud, and falls back (PR #4614 review) -----
    {
        auto& s = AppSettings::instance();
        check(s.setRadioFeature(QStringLiteral("hl2"), QString(),
                                QStringLiteral("CorruptProbe"), 1,
                                QJsonObject{{QStringLiteral("fromFamily"), true}}),
              "family-wide fallback document for the corrupt case stores");
        // Plant a genuinely corrupt EXACT row underneath the API: the raw
        // row writer stores any string, simulating on-disk damage.
        {
            SettingsDatabase raw;
            check(raw.open(SettingsPaths::databasePath()),
                  "a raw handle opens the store");
            check(raw.upsertRadioFeature(QStringLiteral("hl2"),
                                         QStringLiteral("CO:RR:UP:TE:D0:01"),
                                         QStringLiteral("CorruptProbe"), 1,
                                         QStringLiteral("{not json")),
                  "a corrupt exact row is planted");
            raw.close();
        }
        const QJsonObject viaFallback = s.radioFeature(
            QStringLiteral("hl2"), QStringLiteral("CO:RR:UP:TE:D0:01"),
            QStringLiteral("CorruptProbe"));
        check(viaFallback.value(QStringLiteral("fromFamily")).toBool(),
              "a corrupt exact row is logged and falls back to the family "
              "document instead of reading as 'nothing stored'");
    }

    // ---- persistence survives a fresh load --------------------------------
    {
        settings.reset();
        settings.load();
        check(RadioStateMemory::load(radioA, hl2Caps()).rfFrequencyHz
                  == 7'074'000.0,
              "operating state survives a store reload");
    }

    return g_failures == 0 ? 0 : 1;
}
