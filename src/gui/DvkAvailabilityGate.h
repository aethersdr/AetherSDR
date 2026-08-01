#pragma once

#include <QLatin1String>
#include <QString>

namespace AetherSDR {

// The radio's name for the DVK entitlement in a "license feature" status
// message. Authority: FlexLib FeatureLicense.ParseLicenseFeature() maps
// name="digital_voice_keyer" to LicenseFeatDVK (reference/
// FlexLib_API_v4.1.5.39794/FlexLib/FeatureLicense.cs:107-112). Principle I.
inline constexpr QLatin1String kDvkLicenseFeature{"digital_voice_keyer"};

// Why the status-bar DVK indicator is dimmed, or None if it should be live.
//
// Two independent gates, both radio-authoritative (Principle II):
//
//  * TxModeNotVoice — the DVK keys the TX slice, so it follows that slice's
//    mode exactly as CWX does (#4173).
//  * NotLicensed — the radio reports "license feature name=digital_voice_keyer
//    enabled=0", i.e. the operator has no SmartSDR+ entitlement for it. Before
//    this gate the button stayed live and every dvk command was refused by the
//    radio, which read as a dead feature rather than an unlicensed one.
//
// Fail OPEN when the entitlement is unknown. `licenseSeen` is false until a
// "license feature" status for DVK actually arrives — pre-subscription window,
// firmware that never emits one, or a non-Flex backend that has no such notion.
// Treating unknown as unlicensed is the #4210 bug class: a license-derived
// gate that blocked a radio which would have honoured the command just fine.
// The radio must SAY no before the UI says no.
enum class DvkIndicatorBlocker {
    None,           // live
    NotLicensed,    // radio reports the DVK feature disabled
    TxModeNotVoice, // TX slice is CW/DIGU/DIGL, or there is no TX slice
};

inline DvkIndicatorBlocker dvkIndicatorBlocker(bool txModeIsVoice,
                                               bool licenseSeen,
                                               bool licenseEnabled)
{
    // Entitlement outranks mode: a radio without the feature never gains it by
    // switching to USB, so the operator gets the durable reason, not a
    // transient one that implies a mode change would help.
    if (licenseSeen && !licenseEnabled) {
        return DvkIndicatorBlocker::NotLicensed;
    }
    if (!txModeIsVoice) {
        return DvkIndicatorBlocker::TxModeNotVoice;
    }
    return DvkIndicatorBlocker::None;
}

// Tooltip for the DVK indicator, dimmed or not. The gated text names the
// requirement outright instead of echoing FlexLib's marketing copy ("Subscribe
// to SmartSDR+ to use this feature!"), which tells an operator to buy something
// without saying what the button needs.
//
// DVK is a SmartSDR+ feature and the text can say so unconditionally: FlexLib
// gates it through the subscription-backed Feature record, and a FLEX-8600 on
// fw 4.2.18 reports `reason=PLUS` for it. The `reason` field is therefore not
// worth branching on here — it only ever distinguishes which subscription a
// DIFFERENT feature wants, and a wrong-but-specific requirement would be worse
// than this one.
inline QString dvkIndicatorTooltip(DvkIndicatorBlocker blocker)
{
    if (blocker == DvkIndicatorBlocker::NotLicensed) {
        return QStringLiteral(
            "Digital Voice Keyer — requires an active SmartSDR+ subscription");
    }
    return QStringLiteral("Digital Voice Keyer — click to toggle");
}

}  // namespace AetherSDR
