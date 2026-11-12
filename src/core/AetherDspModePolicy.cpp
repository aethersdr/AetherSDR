#include "AetherDspModePolicy.h"

namespace AetherSDR {

bool aetherDspModeRequiresDisable(QStringView mode)
{
    // A slice can be audible before its initial mode arrives. Do not change
    // the operator's NR selection while that mode is still unknown.
    if (mode.isEmpty()) {
        return false;
    }

    // Speech NR is safe automatically only for known analog voice modes;
    // waveform authors can register arbitrary digital mode names. This RX
    // processing list is independent of voice-keyer/Copy Assist availability.
    return !(mode == u"USB" || mode == u"LSB" || mode == u"DSB"
        || mode == u"AM" || mode == u"SAM"
        || mode == u"FM" || mode == u"NFM" || mode == u"DFM"
        || mode == u"WFM" || mode == u"WBFM");
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

    // Keep the remembered method until the queued engine enable is observed.
    // This also preserves the intended restart state if the app closes in the
    // short interval between a mode change and the audio-thread callback.
    if (!restricted && !m_autoDisabledMethod.isEmpty()
        && enabled == m_autoDisabledMethod) {
        m_autoDisabledMethod.clear();
    }

    // Disable an audible method under restriction. Re-disable also fires when
    // the still-remembered method is the one enabled — a restriction that recurs
    // after a restore but before the queued enable is observed (rapid mode
    // churn) would otherwise be skipped and leave the method processing a
    // CW/digital stream. `!enabled.isEmpty()` already means a method is on, so
    // there is nothing to re-emit once it is off.
    if (restricted && !m_userOverride && !enabled.isEmpty()
        && (m_autoDisabledMethod.isEmpty() || m_autoDisabledMethod == enabled)) {
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
