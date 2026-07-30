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
                                 | Domain::TxSetpoints;
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
    state.extensionSchemaVersion = 1;
    state.extension = QJsonObject{{QStringLiteral("lnaDbByBand"),
                                   QJsonObject{{QStringLiteral("40m"), 19}}}};
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
        check(restored.extensionSchemaVersion == 1
                  && restored.extension.value(QStringLiteral("lnaDbByBand"))
                             .toObject()
                             .value(QStringLiteral("40m"))
                             .toInt()
                         == 19,
              "the extension document round-trips opaquely");
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
