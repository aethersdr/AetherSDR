#pragma once

#include <QLatin1String>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// Canonical mode vocabulary: which slice modes count as voice, and which as
// CW. Radio-family-neutral — every backend reports these spellings — so the
// engine can answer the question without reaching into the UI.
//
// Voice modes carry intelligible speech, including wide FM's WFM/WBFM aliases.
// Copy Assist follows the active slice; the voice keyer follows the TX slice
// and additionally excludes the backend's receive-only modes below. CW, data
// modes and an empty mode (no slice) are not voice.
inline bool isVoiceMode(const QString& mode)
{
    return mode == QLatin1String("USB") || mode == QLatin1String("LSB")
        || mode == QLatin1String("AM")  || mode == QLatin1String("SAM")
        || mode == QLatin1String("FM")  || mode == QLatin1String("NFM")
        || mode == QLatin1String("DFM") || mode == QLatin1String("WFM")
        || mode == QLatin1String("WBFM");
}

// The DVK indicator, mode-change abort and playback refusal share this gate.
// A receive-only voice mode remains valid for Copy Assist, but cannot play a
// voice-keyer message. Radio-wide TX/keyer capabilities are checked separately.
inline bool isVoiceTransmitMode(const QString& mode, const QStringList& receiveOnlyModes)
{
    return isVoiceMode(mode) && !receiveOnlyModes.contains(mode, Qt::CaseInsensitive);
}

// CW/CWU use the upper sideband; CWL/CWR explicitly use the reverse sideband.
inline bool isReverseCwMode(const QString& mode)
{
    return mode == QLatin1String("CWL") || mode == QLatin1String("CWR");
}

// Only a command-plane receiver interprets the session CWL offset flag.
// Explicit reverse modes remain authoritative on independent host receivers.
inline int cwReceiveSidebandSign(const QString& mode, bool hasCommandPlane, bool cwlEnabled)
{
    return isReverseCwMode(mode) || (hasCommandPlane && cwlEnabled) ? -1 : 1;
}

inline bool isCwMode(const QString& mode)
{
    return mode == QLatin1String("CW") || mode == QLatin1String("CWU")
        || isReverseCwMode(mode);
}

}  // namespace AetherSDR
