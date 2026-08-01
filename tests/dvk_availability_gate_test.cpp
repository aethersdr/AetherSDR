// Unit tests for the DVK indicator availability policy.
//
// Two regression guards matter here:
//
//  1. The reported bug: on a radio without the DVK entitlement the button was
//     live and every dvk command was refused, so the feature read as broken
//     rather than unlicensed.
//  2. The opposite failure, and the more damaging one — #4210, where a
//     license-derived gate disabled a feature on radios that would have
//     honoured the command. An entitlement we have NOT been told about must
//     never block: the radio has to say no before the UI says no.

#include "gui/DvkAvailabilityGate.h"

#include <cstdio>

using namespace AetherSDR;
using B = DvkIndicatorBlocker;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

B gate(bool txModeIsVoice, bool licenseSeen, bool licenseEnabled)
{
    return dvkIndicatorBlocker(txModeIsVoice, licenseSeen, licenseEnabled);
}

}  // namespace

int main()
{
    // ── Licensed radio: mode is the only gate, exactly as before ────────────
    report("licensed + voice TX mode -> None",
           gate(true, true, true) == B::None);
    report("licensed + non-voice TX mode -> TxModeNotVoice",
           gate(false, true, true) == B::TxModeNotVoice);

    // ── The reported bug: entitlement refused ───────────────────────────────
    report("unlicensed + voice TX mode -> NotLicensed",
           gate(true, true, false) == B::NotLicensed);
    // Entitlement outranks mode: switching to USB will never unlock a feature
    // the radio does not have, so the durable reason is the one to show.
    report("unlicensed + non-voice TX mode -> NotLicensed (entitlement wins)",
           gate(false, true, false) == B::NotLicensed);

    // ── The #4210 guard: unknown entitlement must fail OPEN ─────────────────
    // No "license feature" status for DVK has arrived: pre-subscription window,
    // firmware that never emits one, or a non-Flex backend. Behaviour must be
    // identical to a licensed radio — mode gate only.
    report("entitlement unseen + voice -> None (never blocks on unknown)",
           gate(true, false, false) == B::None);
    report("entitlement unseen + non-voice -> TxModeNotVoice (not NotLicensed)",
           gate(false, false, false) == B::TxModeNotVoice);
    // enabled=true while seen=false is a nonsense pairing from a caller reading
    // a default-constructed LicenseFeatureState; it must still fail open.
    report("entitlement unseen but enabled flag set -> None",
           gate(true, false, true) == B::None);

    // ── Tooltip names the missing subscription ──────────────────────────────
    // The whole point of the gate: a dimmed button has to say what it needs, or
    // it is just as mute as the silently-refused command it replaced.
    report("unlicensed tooltip names an active SmartSDR+ subscription",
           dvkIndicatorTooltip(B::NotLicensed)
               == QStringLiteral("Digital Voice Keyer — requires an active "
                                 "SmartSDR+ subscription"));
    report("mode-gated tooltip stays the normal one",
           dvkIndicatorTooltip(B::TxModeNotVoice)
               == QStringLiteral("Digital Voice Keyer — click to toggle"));
    report("available tooltip stays the normal one",
           dvkIndicatorTooltip(B::None)
               == QStringLiteral("Digital Voice Keyer — click to toggle"));

    // ── The feature name is the FlexLib one ─────────────────────────────────
    report("license feature name matches FlexLib's digital_voice_keyer",
           QString(kDvkLicenseFeature) == QStringLiteral("digital_voice_keyer"));

    std::printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
