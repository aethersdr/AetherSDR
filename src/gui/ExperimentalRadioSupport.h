#pragma once

#include <QString>

#include <optional>

namespace AetherSDR {

struct ExperimentalRadioDescriptor {
    QString displayName;
    QString noticeSettingKey;
};

inline bool experimentalRadioIdentityPending(const QString& family, const QString& model)
{
    return family.trimmed().compare(QLatin1String("icom"), Qt::CaseInsensitive) == 0
        && (model.trimmed().isEmpty()
            || model.trimmed().compare(
                   QLatin1String("Unknown Icom"), Qt::CaseInsensitive) == 0);
}

// How long the experimental chrome waits for a connected radio to identify.
enum class ExperimentalRadioIdentityHold {
    None,          // classify now
    UntilTimeout,  // ordinary connect: bounded by the identity fallback timer
    UntilWakeEnds, // a wake reconnect owns identity; its end re-evaluates
};

inline ExperimentalRadioIdentityHold experimentalRadioIdentityHold(
    const QString& family, const QString& model, bool radioWakeActive,
    bool identityWaitExpired)
{
    if (identityWaitExpired || !experimentalRadioIdentityPending(family, model)) {
        return ExperimentalRadioIdentityHold::None;
    }
    // A woken radio keeps retrying identity well past the ordinary window;
    // RadioModel's wake settles on the identified model or disconnects.
    return radioWakeActive ? ExperimentalRadioIdentityHold::UntilWakeEnds
                           : ExperimentalRadioIdentityHold::UntilTimeout;
}

inline std::optional<ExperimentalRadioDescriptor> experimentalRadioDescriptor(
    const QString& family, const QString& model = {})
{
    const QString normalized = family.trimmed().toLower();
    if (normalized == QLatin1String("icom")) {
        // The backend derives this canonical model name from the radio's
        // verified CI-V 19 00 model ID (0xB6). Keep the CI-V byte and its
        // mapping below the radio seam; the GUI consumes only the normalized
        // capability identity. Other and unknown Icom models remain
        // experimental.
        if (model.trimmed().compare(
                QLatin1String("IC-7300MK2"), Qt::CaseInsensitive) == 0) {
            return std::nullopt;
        }
        return ExperimentalRadioDescriptor{
            QStringLiteral("Icom"),
            QStringLiteral("ShowExperimentalRadioNoticeIcomV1")};
    }
    if (normalized == QLatin1String("hl2")) {
        return ExperimentalRadioDescriptor{
            QStringLiteral("Hermes-Lite 2"),
            QStringLiteral("ShowExperimentalRadioNoticeHl2V1")};
    }
    if (normalized == QLatin1String("anan")) {
        return ExperimentalRadioDescriptor{
            QStringLiteral("ANAN-G2"),
            QStringLiteral("ShowExperimentalRadioNoticeAnanV1")};
    }
    return std::nullopt;
}

// transmitAvailable must reflect the CONNECTED backend's actual
// RadioCapabilities::canTransmit, not an assumption -- ANAN is the first
// family this notice covers where it's false (RX-only by construction in
// this phase; see AnanBackend's own class comment), and claiming transmit
// support that doesn't exist would be a false claim in an operator-facing
// safety/expectations notice, not just imprecise wording.
inline QString experimentalRadioNoticeText(const QString& displayName, bool transmitAvailable)
{
    const QString functions = transmitAvailable
        ? QStringLiteral("Core receive and transmit functions are available")
        : QStringLiteral("Core receive functions are available (this backend is receive-only)");
    return QStringLiteral(
               "Support for %1 radios is still experimental. %2, but some controls, "
               "meters, and radio-specific features may be incomplete or behave "
               "differently than expected.\n\n"
               "If you encounter a problem, use Help \u2192 File an Issue and include your "
               "radio model and firmware version. Your reports help us improve support for "
               "this radio family.")
        .arg(displayName, functions);
}

} // namespace AetherSDR
