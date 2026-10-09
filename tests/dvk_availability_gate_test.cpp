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

B gate(bool txModeIsVoice, bool licenseSeen, bool licenseEnabled,
       bool radioRefused = false)
{
    return dvkIndicatorBlocker(txModeIsVoice, licenseSeen, licenseEnabled, radioRefused);
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

    // ── A 50004001 refusal is the radio saying no (#6244) ───────────────────
    // `dvk … enabled=` is always 1, so the wiki names the refusal code as the
    // license signal. It blocks even before any license status has arrived.
    report("radio refused + entitlement unseen -> NotLicensed",
           gate(true, false, false, true) == B::NotLicensed);
    report("radio refused overrides a license status that said enabled",
           gate(true, true, true, true) == B::NotLicensed);
    report("radio refused + non-voice -> NotLicensed (entitlement wins)",
           gate(false, false, false, true) == B::NotLicensed);

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

    // ── RFC #4214: the client-side keyer ignores the radio's entitlement ────
    using VS = VoiceKeyerSource;
    report("local keyer + unlicensed + voice -> None (no licence needed)",
           voiceKeyerIndicatorBlocker(VS::Local, true, true, false, false) == B::None);
    report("local keyer + unlicensed + non-voice -> TxModeNotVoice",
           voiceKeyerIndicatorBlocker(VS::Local, false, true, false, false) == B::TxModeNotVoice);
    report("radio keyer + unlicensed + voice -> NotLicensed (unchanged)",
           voiceKeyerIndicatorBlocker(VS::Radio, true, true, false, false) == B::NotLicensed);
    report("radio keyer + unseen entitlement + voice -> None (still fails open)",
           voiceKeyerIndicatorBlocker(VS::Radio, true, false, false, false) == B::None);
    // A refusal (#6262) is the reliable not-licensed signal, and it must not
    // follow the operator to the client-side keyer: Local never asks the radio
    // to store or play anything, so a refused `dvk` command cannot gate it.
    report("radio keyer + refusal + voice -> NotLicensed",
           voiceKeyerIndicatorBlocker(VS::Radio, true, false, true, true) == B::NotLicensed);
    report("local keyer + refusal + voice -> None (refusal does not reach Local)",
           voiceKeyerIndicatorBlocker(VS::Local, true, false, true, true) == B::None);
    report("local keyer + refusal + non-voice -> TxModeNotVoice (mode still gates)",
           voiceKeyerIndicatorBlocker(VS::Local, false, false, true, true) == B::TxModeNotVoice);

    // ── Keyer-source menu: the Radio DVK choice follows the same gate ───────
    report("radio choice open when licensed",
           radioVoiceKeyerUnavailableReason(true, true, true, false).isEmpty());
    report("radio choice open while the entitlement is unreported (fails open)",
           radioVoiceKeyerUnavailableReason(true, false, false, false).isEmpty());
    report("radio choice closed when the radio reports no entitlement",
           radioVoiceKeyerUnavailableReason(true, true, false, false).contains(QStringLiteral("SmartSDR+")));
    report("radio choice closed on a radio with no DVK, whatever the licence says",
           radioVoiceKeyerUnavailableReason(false, true, true, false)
               == QStringLiteral("not available on this radio"));
    // The case the refusal flag exists for: a refusing radio still reports
    // enabled=1, so without it the menu would offer Radio DVK on a radio that
    // cannot store a recording and the operator would get silence.
    report("radio choice closed after the radio refuses a dvk command",
           radioVoiceKeyerUnavailableReason(true, true, true, true)
               .contains(QStringLiteral("SmartSDR+")));

    // ── Which capability gates which keyer ──────────────────────────────────
    // The radio's own DVK is required only when the operator drives it. Local
    // stores and plays everything on this computer, so it needs the radio to
    // transmit and nothing else — this is what made the feature Flex-only.
    report("local keyer on a radio with no DVK is usable",
           voiceKeyerUsable(VS::Local, false, true));
    report("radio keyer on a radio with no DVK is not",
           !voiceKeyerUsable(VS::Radio, false, true));
    report("local keyer on a receive-only radio is not usable",
           !voiceKeyerUsable(VS::Local, true, false));
    report("radio keyer with a DVK is usable",
           voiceKeyerUsable(VS::Radio, true, true));

    // ── The feature name is the FlexLib one ─────────────────────────────────
    report("license feature name matches FlexLib's digital_voice_keyer",
           QString(kDvkLicenseFeature) == QStringLiteral("digital_voice_keyer"));

    std::printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
