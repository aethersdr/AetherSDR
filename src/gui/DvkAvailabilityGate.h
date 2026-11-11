#pragma once

#include "models/VoiceKeyerSource.h"

#include <QLatin1String>
#include <QString>

namespace AetherSDR {

// The radio's name for the DVK entitlement in a "license feature" status
// message. Authority: FlexLib FeatureLicense.ParseLicenseFeature() maps
// name="digital_voice_keyer" to LicenseFeatDVK (reference/
// FlexLib_API_v4.1.5.39794/FlexLib/FeatureLicense.cs:107-112). Principle I.
inline constexpr QLatin1String kDvkLicenseFeature{"digital_voice_keyer"};

// Why the status-bar DVK indicator is dimmed, or None. TxModeNotVoice: DVK
// keys the TX slice and follows its mode like CWX (#4173). NotLicensed: the
// radio reports "license feature name=digital_voice_keyer enabled=0", or it
// answered a `dvk` command with 50004001 (`radioRefused`) — the wiki's reliable
// signal, since `dvk … enabled=` is always 1. Fails OPEN while the entitlement
// is unknown (no status yet, firmware that never sends one, non-Flex backend)
// — the radio must say no before the UI does (#4210).
enum class DvkIndicatorBlocker {
    None,           // live
    NotLicensed,    // radio reports the DVK feature disabled
    TxModeNotVoice, // TX slice is CW/DIGU/DIGL, or there is no TX slice
};

inline DvkIndicatorBlocker dvkIndicatorBlocker(bool txModeIsVoice,
                                               bool licenseSeen,
                                               bool licenseEnabled,
                                               bool radioRefused)
{
    // Entitlement outranks mode: a radio without the feature never gains it by
    // switching to USB, so the operator gets the durable reason, not a
    // transient one that implies a mode change would help.
    if (radioRefused || (licenseSeen && !licenseEnabled)) {
        return DvkIndicatorBlocker::NotLicensed;
    }
    if (!txModeIsVoice) {
        return DvkIndicatorBlocker::TxModeNotVoice;
    }
    return DvkIndicatorBlocker::None;
}

// The same gate once RFC #4214's client-side keyer exists. With the Local keyer
// selected, recordings and playback never touch the radio's DVK, so the radio's
// entitlement is irrelevant and only the TX-mode gate applies. With the Radio
// keyer selected, nothing changes.
inline DvkIndicatorBlocker voiceKeyerIndicatorBlocker(VoiceKeyerSource source,
                                                      bool txModeIsVoice,
                                                      bool licenseSeen,
                                                      bool licenseEnabled,
                                                      bool radioRefused)
{
    if (source == VoiceKeyerSource::Local) {
        // radioRefused is deliberately ignored: a refusal from the radio's DVK
        // says nothing about a keyer that never asks the radio to store or play
        // anything. Only the TX-mode gate survives.
        return txModeIsVoice ? DvkIndicatorBlocker::None
                             : DvkIndicatorBlocker::TxModeNotVoice;
    }
    return dvkIndicatorBlocker(txModeIsVoice, licenseSeen, licenseEnabled, radioRefused);
}

// Whether the keyer surface can be driven at all, before the mode and licence
// gate above. The radio's own DVK is required only when the operator is driving
// it: the client-side keyer stores and plays everything on this computer, so a
// radio with no DVK is no reason to hide it — it only has to be able to
// transmit. A receive-only radio can drive neither.
inline bool voiceKeyerUsable(VoiceKeyerSource source, bool radioHasKeyer, bool canTransmit)
{
    return source == VoiceKeyerSource::Local ? canTransmit : radioHasKeyer;
}

// Why the "Radio DVK" choice in the keyer-source menu cannot be picked, or an
// empty string when it can. Same radio-authoritative, fail-open rules as the
// indicator: a radio with no DVK at all, one that SAYS the entitlement is off,
// or one that has refused a `dvk` command with 50004001, disables the choice;
// an entitlement not yet reported leaves it open.
//
// radioRefused is taken for the same reason the indicator takes it (#6262): a
// refusing radio still reports `dvk ... enabled=1`, so licenseSeen &&
// !licenseEnabled is false and the menu would otherwise offer Radio DVK on a
// radio that cannot store a recording — the operator picks it and gets silence.
inline QString radioVoiceKeyerUnavailableReason(bool hasVoiceKeyer,
                                                bool licenseSeen,
                                                bool licenseEnabled,
                                                bool radioRefused)
{
    if (!hasVoiceKeyer) {
        return QStringLiteral("not available on this radio");
    }
    if (radioRefused || (licenseSeen && !licenseEnabled)) {
        return QStringLiteral("not licensed on this radio (SmartSDR+)");
    }
    return QString();
}

// Whether the keyer panel may open. Wider than voiceKeyerUsable() on purpose:
// the panel's settings button is the only place the keyer source is chosen, so
// a source this radio cannot serve (the radio DVK unlicensed, or a radio with
// none) must still let the operator reach it and pick Local. The TX-mode gate
// still applies, and a radio that can neither transmit nor host a DVK has
// nothing to choose between. With no radio connected it always opens: there
// is no TX slice to have a mode, and the local slots can be previewed and
// arranged offline.
inline bool voiceKeyerPanelOpenable(bool connected, bool txModeIsVoice, bool radioHasKeyer,
                                    bool canTransmit)
{
    return !connected || (txModeIsVoice && (radioHasKeyer || canTransmit));
}

// Why the open panel cannot drive its keyer, for the panel's status line; empty
// when it can. Short enough for the two lines the status area reserves, and it
// names the way out, because the panel is where the source is changed.
inline QString voiceKeyerPanelUnavailableReason(VoiceKeyerSource source,
                                                bool connected,
                                                bool radioHasKeyer,
                                                bool canTransmit,
                                                bool licenseSeen,
                                                bool licenseEnabled,
                                                bool radioRefused)
{
    // Offline nothing can transmit and the radio DVK's slots live on the radio.
    // The local slots can still be previewed and edited
    // (voiceKeyerPanelSlotsEditableOffline()).
    if (!connected) {
        return QStringLiteral("Not connected to a radio.");
    }
    if (source == VoiceKeyerSource::Local) {
        return canTransmit ? QString() : QStringLiteral("This radio cannot transmit.");
    }
    // The rule is the source menu's, so the menu and the panel cannot drift
    // apart on when the radio DVK is usable (#6262); only the wording differs.
    if (radioVoiceKeyerUnavailableReason(radioHasKeyer, licenseSeen, licenseEnabled,
                                         radioRefused).isEmpty()) {
        return QString();
    }
    return radioHasKeyer
        ? QStringLiteral("Radio DVK not licensed (SmartSDR+). Choose Local with ⚙.")
        : QStringLiteral("This radio has no DVK. Choose Local with ⚙.");
}

// Offline, the local keyer's slots are files on this computer: they can be
// previewed on the speakers, renamed, cleared, imported and exported, though
// nothing can be played on the air or recorded (the microphone is captured only
// for a radio session).
inline bool voiceKeyerPanelSlotsEditableOffline(VoiceKeyerSource source, bool connected)
{
    return !connected && source == VoiceKeyerSource::Local;
}

// Tooltip for the DVK indicator. Names the SmartSDR+ requirement directly
// rather than FlexLib's "Subscribe…" copy. FlexLib gates DVK on the
// subscription Feature record and a FLEX-8600 on fw 4.2.18 reports
// `reason=PLUS`, so `reason` is not branched on. It says the radio is not
// licensed, never that the subscription must be active: a lapsed subscriber
// keeps every feature released before the expiration (LicenseEntitlement.h).
// An unlicensed radio still opens the panel (voiceKeyerPanelOpenable), so the
// tooltip says where to go.
inline QString dvkIndicatorTooltip(DvkIndicatorBlocker blocker)
{
    if (blocker == DvkIndicatorBlocker::NotLicensed) {
        return QStringLiteral(
            "Digital Voice Keyer — not licensed on this radio (SmartSDR+). "
            "Click to open, then choose Local with ⚙.");
    }
    return QStringLiteral("Digital Voice Keyer — click to toggle");
}

}  // namespace AetherSDR
