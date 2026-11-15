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
#include "models/ModeVocabulary.h"

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
    const QStringList receiveOnlyModes{QStringLiteral("wfm"), QStringLiteral("WBFM")};
    for (const QString& mode : {QStringLiteral("WFM"), QStringLiteral("WBFM")}) {
        const bool txModeIsVoice = isVoiceTransmitMode(mode, receiveOnlyModes);
        report("wide FM remains available to receive-only Copy Assist", isVoiceMode(mode));
        report("receive-only wide FM blocks the Local keyer and its shortcuts",
               voiceKeyerIndicatorBlocker(VoiceKeyerSource::Local, txModeIsVoice,
                                          false, false, false) == B::TxModeNotVoice);
        report("receive-only wide FM blocks the licensed Radio keyer",
               voiceKeyerIndicatorBlocker(VoiceKeyerSource::Radio, txModeIsVoice,
                                          true, true, false) == B::TxModeNotVoice);
        report("receive-only wide FM keeps the connected keyer panel closed",
               !voiceKeyerPanelOpenable(true, txModeIsVoice, true, true));
        report("entering receive-only wide FM triggers the mode-change abort gate",
               !txModeIsVoice);
    }
    for (const QString& mode : {QStringLiteral("USB"), QStringLiteral("LSB"),
                                QStringLiteral("AM"), QStringLiteral("SAM"),
                                QStringLiteral("FM"), QStringLiteral("NFM"),
                                QStringLiteral("DFM")}) {
        report("transmittable voice modes retain keyer availability",
               isVoiceTransmitMode(mode, receiveOnlyModes));
    }
    report("backend-declared receive-only voice modes are all excluded",
           !isVoiceTransmitMode(QStringLiteral("SAM"), {QStringLiteral("SAM")}));
    report("an empty mode never becomes a transmit voice mode",
           !isVoiceTransmitMode(QString(), receiveOnlyModes));
    report("data never becomes a transmit voice mode",
           !isVoiceTransmitMode(QStringLiteral("DIGU"), receiveOnlyModes));
    report("TX-mode exclusions come from capabilities rather than a wide-FM blacklist",
           isVoiceTransmitMode(QStringLiteral("WFM"), {}));

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
    // It says the radio is not licensed, never that a subscription must be
    // active (#6924), and since the unlicensed indicator still opens the panel,
    // it also says how to reach the client-side keyer from there.
    report("unlicensed tooltip names SmartSDR+ and the way to Local",
           dvkIndicatorTooltip(B::NotLicensed)
               == QStringLiteral("Digital Voice Keyer — not licensed on this radio "
                                 "(SmartSDR+). Click to open, then choose Local with ⚙."));
    report("mode-gated tooltip stays the normal one",
           dvkIndicatorTooltip(B::TxModeNotVoice)
               == QStringLiteral("Digital Voice Keyer — click to toggle"));
    report("available tooltip stays the normal one",
           dvkIndicatorTooltip(B::None)
               == QStringLiteral("Digital Voice Keyer — click to toggle"));

    // ── RFC #4214: the client-side keyer ignores the radio's entitlement ────
    using VS = VoiceKeyerSource;
    report("local keyer + unlicensed + voice -> None (no license needed)",
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
    report("radio choice closed on a radio with no DVK, whatever the license says",
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

    // ── The panel opens wider than the keyer can be driven ──────────────────
    // The source is chosen from the panel's settings button and nowhere else,
    // so a Radio choice the radio cannot serve must still open the panel, or an
    // operator whose saved choice is Radio is locked out of picking Local.
    report("panel opens on a radio with no DVK that can transmit",
           voiceKeyerPanelOpenable(true, true, false, true));
    report("panel opens on a radio with a DVK, license aside",
           voiceKeyerPanelOpenable(true, true, true, false));
    report("panel stays shut on a radio with neither a DVK nor transmit",
           !voiceKeyerPanelOpenable(true, true, false, false));
    report("panel stays shut outside a voice mode",
           !voiceKeyerPanelOpenable(true, false, true, true));
    report("panel opens with no radio connected",
           voiceKeyerPanelOpenable(false, false, false, false));

    // ── And says why it cannot drive the keyer ──────────────────────────────
    const auto why = [](VS source, bool hasKeyer, bool canTx, bool seen, bool enabled,
                        bool refused) {
        return voiceKeyerPanelUnavailableReason(source, true, hasKeyer, canTx, seen, enabled, refused);
    };
    report("no reason for a licensed radio DVK",
           why(VS::Radio, true, true, true, true, false).isEmpty());
    report("no reason while the entitlement is unreported (fails open)",
           why(VS::Radio, true, true, false, false, false).isEmpty());
    report("unlicensed radio DVK names SmartSDR+ and points to Local",
           why(VS::Radio, true, true, true, false, false)
               == QStringLiteral("Radio DVK not licensed (SmartSDR+). Choose Local with ⚙."));
    report("a refused dvk command reads as unlicensed",
           why(VS::Radio, true, true, true, true, true).contains(QStringLiteral("SmartSDR+")));
    report("Radio on a radio with no DVK points to Local",
           why(VS::Radio, false, true, true, true, false)
               == QStringLiteral("This radio has no DVK. Choose Local with ⚙."));
    report("no reason for Local on a radio that transmits, license aside",
           why(VS::Local, true, true, true, false, true).isEmpty());
    report("Local on a radio that cannot transmit says so",
           why(VS::Local, true, false, true, true, false)
               == QStringLiteral("This radio cannot transmit."));
    report("offline, both sources say not connected rather than blame the radio",
           voiceKeyerPanelUnavailableReason(VS::Local, false, true, false, false, false, false)
                   == QStringLiteral("Not connected to a radio.")
               && voiceKeyerPanelUnavailableReason(VS::Radio, false, true, false, false, false,
                                                   false)
                      == QStringLiteral("Not connected to a radio."));
    report("offline, only the local slots stay editable",
           voiceKeyerPanelSlotsEditableOffline(VS::Local, false)
               && !voiceKeyerPanelSlotsEditableOffline(VS::Radio, false)
               && !voiceKeyerPanelSlotsEditableOffline(VS::Local, true));

    // ── The feature name is the FlexLib one ─────────────────────────────────
    report("license feature name matches FlexLib's digital_voice_keyer",
           QString(kDvkLicenseFeature) == QStringLiteral("digital_voice_keyer"));

    // One session on a radio that reports its DVK enabled but refuses every dvk
    // command with 50004001 (#6262), keyer source Automatic (#7152). Nothing
    // sent at connect detects the refusal: `sub dvk all` is the only dvk
    // command then, and its reply is not examined. So the first DVK action of
    // each session still goes to the radio and is refused; that refusal moves
    // Automatic to Local, and a disconnect clears it, so the next session
    // starts on the radio again.
    {
        using S = VoiceKeyerSourceSetting;
        using R = VoiceKeyerSource;
        const bool hasKeyer = true;
        const bool canTransmit = true;
        // The license status this radio sends, if it sends one: enabled.
        for (const bool licenseSeen : {false, true}) {
            const bool licenseEnabled = licenseSeen;

            // Connected, nothing refused yet: Automatic is on the radio DVK and
            // nothing blocks it, so the first F-key or REC goes to the radio.
            const R atConnect = resolveVoiceKeyerSource(S::Auto, hasKeyer, licenseSeen,
                                                        licenseEnabled, false);
            const bool firstActionGoesToRadio =
                atConnect == R::Radio
                && voiceKeyerIndicatorBlocker(atConnect, true, licenseSeen, licenseEnabled,
                                              false) == B::None
                && voiceKeyerUsable(atConnect, hasKeyer, canTransmit);

            // The radio refuses it: Automatic moves to Local, which nothing
            // blocks, so the panel drives the local keyer with no stuck message.
            const R afterRefusal = resolveVoiceKeyerSource(S::Auto, hasKeyer, licenseSeen,
                                                           licenseEnabled, true);
            const bool movesToLocal =
                afterRefusal == R::Local
                && voiceKeyerIndicatorBlocker(afterRefusal, true, licenseSeen, licenseEnabled,
                                              true) == B::None
                && voiceKeyerUsable(afterRefusal, hasKeyer, canTransmit)
                && voiceKeyerPanelUnavailableReason(afterRefusal, true, hasKeyer, canTransmit,
                                                    licenseSeen, licenseEnabled, true)
                       .isEmpty();

            // Disconnect clears the refusal (DvkModel::reset): the next
            // session starts on the radio DVK again.
            const bool nextSessionOnRadio =
                resolveVoiceKeyerSource(S::Auto, hasKeyer, licenseSeen, licenseEnabled, false)
                == R::Radio;

            report(licenseSeen
                       ? "auto_session_refusing_radio_reported_licensed_first_action_then_local"
                       : "auto_session_refusing_radio_no_license_report_first_action_then_local",
                   firstActionGoesToRadio && movesToLocal && nextSessionOnRadio);
        }

        // With Radio chosen explicitly the refusal never moves the keyer: the
        // panel stays on the radio DVK, blocked, and names the way out.
        const R forced = resolveVoiceKeyerSource(S::Radio, true, false, false, true);
        report("forced_radio_after_refusal_stays_blocked_with_a_reason",
               forced == R::Radio
                   && voiceKeyerIndicatorBlocker(forced, true, false, false, true)
                          == B::NotLicensed
                   && voiceKeyerPanelUnavailableReason(forced, true, true, true, false, false, true)
                          == QStringLiteral(
                              "Radio DVK not licensed (SmartSDR+). Choose Local with ⚙."));
    }

    std::printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
