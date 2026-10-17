#pragma once

#include <QLatin1String>
#include <QString>

namespace AetherSDR {

// Canonical mode vocabulary: which slice modes count as voice, and which as
// CW. Radio-family-neutral — every backend reports these spellings — so the
// engine can answer the question without reaching into the UI.
//
// Voice modes are SSB, AM/SAM and the FM family (not CW, RTTY, DIGx,
// FreeDV/RADE). The DVK indicator asks about the TX slice (#4173); Copy Assist
// asks about the active slice. One list so the two can't drift. An empty mode
// (no slice) is not voice.
inline bool isVoiceMode(const QString& mode)
{
    return mode == QLatin1String("USB") || mode == QLatin1String("LSB")
        || mode == QLatin1String("AM")  || mode == QLatin1String("SAM")
        || mode == QLatin1String("FM")  || mode == QLatin1String("NFM")
        || mode == QLatin1String("DFM");
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
