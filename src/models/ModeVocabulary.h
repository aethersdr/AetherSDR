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

// AetherSDR's older neutral vocabulary used CW for upper-side CW. Icom
// reports that same mode explicitly as CWU; CWL is the reverse-side mode.
// (The HL2 did too, until Hl2Backend::setSliceMode began collapsing CWU onto
// CW. All three spellings stay here: Icom still produces CWU, and a guard
// that is only correct while every backend canonicalises is the wrong kind.)
inline bool isCwMode(const QString& mode)
{
    return mode == QLatin1String("CW") || mode == QLatin1String("CWU")
        || mode == QLatin1String("CWL");
}

}  // namespace AetherSDR
