#include "AetherDspModePolicy.h"

#include "models/ModeVocabulary.h"

namespace AetherSDR {

bool aetherDspModeRequiresDisable(QStringView mode)
{
    // A slice can be audible before its initial mode arrives. Do not change
    // the operator's NR selection while that mode is still unknown.
    if (mode.isEmpty()) {
        return false;
    }

    // Automatic speech NR is limited to the shared analog-voice table;
    // waveform authors can register arbitrary digital mode names, and DFM is
    // FM data.
    return !isAnalogVoiceMode(mode);
}

bool aetherDspMixRequiresDisable(const QList<AetherDspSliceAudioState>& slices)
{
    for (const AetherDspSliceAudioState& slice : slices) {
        if (slice.receiving && !slice.muted && slice.gain > 0.0f
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

    // Keep the remembered method until a method is observed running in a
    // compatible mode: the queued restore itself, or a different method a
    // profile or preset load switched on without a user gesture. While the
    // queued enable is still in flight nothing is enabled, so the memory also
    // preserves the intended restart state if the app closes in that interval.
    if (!restricted && !m_autoDisabledMethod.isEmpty() && !enabled.isEmpty()) {
        m_autoDisabledMethod.clear();
    }

    // Disable any audible method under restriction unless the user chose it
    // there. That covers a restriction that recurs after a restore but before
    // the queued enable is observed (rapid mode churn), and a different method
    // a profile or preset load switched on while restricted: either would
    // otherwise keep processing a CW/digital stream. The method disabled last
    // is the one remembered. `!enabled.isEmpty()` already means a method is
    // on, so there is nothing to re-emit once it is off.
    if (restricted && !m_userOverride && !enabled.isEmpty()) {
        m_autoDisabledMethod = enabled;
        return {ActionKind::Disable, enabled};
    }

    return {};
}

void AetherDspModePolicy::noteUserOverride()
{
    // A manual selection owns persistence and the next restriction, even if
    // the queued automatic restore has not yet been observed in voice mode.
    m_autoDisabledMethod.clear();
    if (!m_restricted) {
        return;
    }
    m_userOverride = true;
}

QString AetherDspModePolicy::methodForPersistence(QStringView enabledMethod) const
{
    return m_autoDisabledMethod.isEmpty()
        ? enabledMethod.toString()
        : m_autoDisabledMethod;
}

} // namespace AetherSDR
