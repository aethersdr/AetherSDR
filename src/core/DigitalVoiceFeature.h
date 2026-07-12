#pragma once

#include "DigitalVoiceModeRegistry.h"

#include <QString>
#include <QStringList>

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

} // namespace AetherSDR
