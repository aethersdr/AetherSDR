#include "AetherDspModePolicy.h"

namespace AetherSDR {

bool aetherDspModeRequiresDisable(QStringView mode)
{
    return mode == u"DIGU" || mode == u"DIGL" || mode == u"RTTY"
        || mode == u"CW" || mode == u"CWL" || mode == u"NT";
}

bool aetherDspMixRequiresDisable(const QList<AetherDspSliceAudioState>& slices)
{
    for (const AetherDspSliceAudioState& slice : slices) {
        if (!slice.muted && slice.gain > 0.0f
            && aetherDspModeRequiresDisable(slice.mode)) {
            return true;
        }
    }
    return false;
}

AetherDspModePolicy::Action AetherDspModePolicy::update(
    bool restricted, QStringView enabledMethod)
{
    const QString enabled = enabledMethod.toString();
    if (restricted != m_restricted) {
        m_restricted = restricted;
        if (!restricted) {
            m_userOverride = false;
            if (!m_autoDisabledMethod.isEmpty()) {
                return {ActionKind::Enable, m_autoDisabledMethod};
            }
            return {};
        }
    }

    // Keep the remembered method until the queued engine enable is observed.
    // This also preserves the intended restart state if the app closes in the
    // short interval between a mode change and the audio-thread callback.
    if (!restricted && !m_autoDisabledMethod.isEmpty()
        && enabled == m_autoDisabledMethod) {
        m_autoDisabledMethod.clear();
    }

    if (restricted && !m_userOverride && m_autoDisabledMethod.isEmpty()
        && !enabled.isEmpty()) {
        m_autoDisabledMethod = enabled;
        return {ActionKind::Disable, enabled};
    }

    return {};
}

void AetherDspModePolicy::noteUserOverride()
{
    if (!m_restricted) {
        return;
    }
    m_userOverride = true;
    m_autoDisabledMethod.clear();
}

QString AetherDspModePolicy::methodForPersistence(QStringView enabledMethod) const
{
    return m_autoDisabledMethod.isEmpty()
        ? enabledMethod.toString()
        : m_autoDisabledMethod;
}

} // namespace AetherSDR
