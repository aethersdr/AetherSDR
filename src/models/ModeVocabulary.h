#pragma once

#include <QLatin1String>
#include <QString>

namespace AetherSDR {

// Canonical mode vocabulary: which slice modes count as voice, and which as
// CW. Radio-family-neutral — every backend reports these spellings — so the
// engine can answer the question without reaching into the UI.
//
// Voice modes are SSB, AM/SAM and the FM family, wide FM included (not CW,
// RTTY, DIGx, FreeDV/RADE). The DVK indicator asks about the TX slice (#4173);
// Copy Assist asks about the active slice. One list so the two can't drift. An
// empty mode (no slice) is not voice.
//
// WFM carries intelligible speech — broadcast FM, weather radio, some satellite
// downlinks — so Copy Assist offers it (#5301). Both spellings are listed: the
// Icom backend reports "WFM", while the HL2 and ANAN use "WBFM" (the HL2
// canonicalises "WFM" onto "WBFM", so a slice can hold either).
//
// Admitting WFM here does not arm the voice keyer on it. The DVK indicator ANDs
// this predicate with the radio's own capability at the call site
// (MainWindow::updateKeyerAvailability): voiceKeyerUsable() needs canTransmit
// for the local keyer or the radio's DVK entitlement for the radio keyer, and
// modeIsReceiveOnly() covers the mode itself. Every backend that offers wide FM
// today blocks it on at least one of those — the IC-705 and the HL2 list it in
// receiveOnlyModes, and the ANAN sets canTransmit = false.
inline bool isVoiceMode(const QString& mode)
{
    return mode == QLatin1String("USB") || mode == QLatin1String("LSB")
        || mode == QLatin1String("AM")  || mode == QLatin1String("SAM")
        || mode == QLatin1String("FM")  || mode == QLatin1String("NFM")
        || mode == QLatin1String("DFM") || mode == QLatin1String("WFM")
        || mode == QLatin1String("WBFM");
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
