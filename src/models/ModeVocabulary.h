#pragma once

#include <QLatin1String>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace AetherSDR {

// Canonical mode vocabulary: which slice modes count as voice, and which as
// CW. Radio-family-neutral — every backend reports these spellings — so the
// engine can answer the question without reaching into the UI.
//
// The one analog-voice mode table. AetherDSP's automatic NR gate
// (core/AetherDspModePolicy.cpp) and isVoiceMode() below both read it, so a
// new voice mode is a single edit here. AME is Flex's AM variant, handled
// with AM/SAM/DSB in FlexLib v4.3.16 (Slice.cs); not firmware-confirmed on a
// slice. FMN is the RTL-SDR's narrow FM;
// WFM/WBFM are wide FM's aliases. DFM is FM data and stays out, as do CW,
// the digital waveforms (DSTR, FreeDV, DRM, ...) and an empty mode.
inline bool isAnalogVoiceMode(QStringView mode)
{
    return mode == u"USB" || mode == u"LSB" || mode == u"DSB"
        || mode == u"AM"  || mode == u"AME" || mode == u"SAM"
        || mode == u"FM"  || mode == u"NFM" || mode == u"FMN"
        || mode == u"WFM" || mode == u"WBFM";
}

// Voice modes carry intelligible speech: the shared analog-voice table.
// Copy Assist follows the active slice; the voice keyer follows the TX slice
// and additionally excludes the backend's receive-only modes below. CW, data
// modes (DFM included) and an empty mode (no slice) are not voice.
inline bool isVoiceMode(const QString& mode)
{
    return isAnalogVoiceMode(mode);
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
