#pragma once

#include "DigitalVoiceModeRegistry.h"

#include <QString>
#include <QStringList>

#include <algorithm>
#include <optional>

namespace AetherSDR {

#ifdef AETHER_ENABLE_DIGITAL_VOICE_HELPER
inline constexpr bool kLocalDigitalVoiceWaveformAvailable = true;
#else
inline constexpr bool kLocalDigitalVoiceWaveformAvailable = false;
#endif

inline QStringList filterUnavailableDigitalVoiceModes(QStringList modes)
{
    QStringList filteredModes;
    for (const QString& mode : modes) {
        if (!filteredModes.contains(mode)) {
            filteredModes.append(mode);
        }
    }
    modes = filteredModes;

    if constexpr (!kLocalDigitalVoiceWaveformAvailable) {
        for (const DigitalVoiceModeDescriptor& mode
             : DigitalVoiceModeRegistry::supportedModes()) {
            modes.removeAll(mode.radioMode);
        }
    }
    return modes;
}

// The modes with a keyboard shortcut (mode_<x>) and a MIDI trigger
// (global.mode<X>), in Mode Up / Down order. One list for both, so a mode is
// added in one place.
inline QStringList modeActionModes()
{
    return filterUnavailableDigitalVoiceModes(
        {"USB", "LSB", "CW", "CWL", "AM", "SAM", "FM", "NFM",
         "DFM", "DSTR", "DIGU", "DIGL", "RTTY", "FDVU", "FDVL"});
}

// Whether a slice can be put in radioMode right now. A digital-voice mode is
// accepted only while its local helper runs: SliceModel::setMode() is refused
// by DigitalVoiceModeRegistry::transferSlice() otherwise and the slice keeps
// its mode. Every other mode is left to the radio.
inline bool digitalVoiceModeSelectable(const QString& radioMode)
{
    const std::optional<DigitalVoiceModeId> id =
        DigitalVoiceModeRegistry::modeForRadioMode(radioMode);
    if (!id.has_value()) {
        return true;
    }
    if constexpr (!kLocalDigitalVoiceWaveformAvailable) {
        return false;
    } else {
        const std::optional<DigitalVoiceModeId> active =
            DigitalVoiceModeRegistry::instance().activeMode();
        return active.has_value() && active.value() == id.value();
    }
}

// Modes a radio-side waveform provides (the FreeDV waveform on a Flex). A
// cycle stop only when the radio reports them: an empty mode_list must not
// offer a mode the backend does not have.
inline bool isRadioWaveformMode(const QString& mode)
{
    return mode == QLatin1String("FDVU") || mode == QLatin1String("FDVL");
}

// The Mode Up / Down step: the next entry of modes after currentMode in
// direction (+1 / -1), skipping any the slice cannot be put in right now.
// Stepping onto a refused mode left the slice where it was, so every later
// press asked for the same mode again (#6034). radioModes is the slice's
// mode_list as the radio reports it; when it is not empty, entries the radio
// does not report are skipped too (a FLEX-8400 has no CWL and turns a request
// for it into DIGU). Empty radioModes (no list reported yet, or a backend that
// reports none) keeps the list except the radio-waveform modes. An unknown
// currentMode is treated as index 0 before stepping, preserving the previous
// cycle behavior. Empty when no entry is selectable.
inline QString nextCycledMode(const QStringList& modes, const QString& currentMode,
                              int direction, const QStringList& radioModes = {})
{
    const int count = static_cast<int>(modes.size());
    int idx = std::max(0, static_cast<int>(modes.indexOf(currentMode.toUpper())));
    for (int tries = 0; tries < count; ++tries) {
        idx = (idx + direction + count) % count;
        if (radioModes.isEmpty() ? isRadioWaveformMode(modes[idx])
                                 : !radioModes.contains(modes[idx])) {
            continue;
        }
        if (digitalVoiceModeSelectable(modes[idx])) {
            return modes[idx];
        }
    }
    return {};
}

} // namespace AetherSDR
