#pragma once

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
// radio reports "license feature name=digital_voice_keyer enabled=0". Fails
// OPEN while the entitlement is unknown (`licenseSeen` false: no status yet,
// firmware that never sends one, non-Flex backend) — the radio must say no
// before the UI does (#4210).
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

// Tooltip for the DVK indicator. Names the SmartSDR+ requirement directly
// rather than FlexLib's "Subscribe…" copy. FlexLib gates DVK on the
// subscription Feature record and a FLEX-8600 on fw 4.2.18 reports
// `reason=PLUS`, so `reason` is not branched on.
inline QString dvkIndicatorTooltip(DvkIndicatorBlocker blocker)
{
    if (blocker == DvkIndicatorBlocker::NotLicensed) {
        return QStringLiteral(
            "Digital Voice Keyer — requires an active SmartSDR+ subscription");
    }
    return QStringLiteral("Digital Voice Keyer — click to toggle");
}

}  // namespace AetherSDR
