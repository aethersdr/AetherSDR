#pragma once

#include <QLatin1String>
#include <QString>

namespace AetherSDR {

// The modes that carry human-intelligible voice audio, as the status-bar
// indicators gate on them: the SSB pair, AM/SAM and the FM family. CW, RTTY,
// the DIGx pair and the FreeDV/RADE modes are out — nothing a voice keyer
// should play into, and nothing an acoustic speech model can transcribe.
//
// One list, two callers reading DIFFERENT slices:
//
//  * the DVK indicator asks about the TRANSMIT slice's mode — the keyer keys
//    that slice, so its availability follows it (#4173);
//  * the Copy Assist (ASR) indicator asks about the ACTIVE slice's mode — a
//    receive-side decode of the audio the operator is listening to, exactly as
//    the CW decoder's own gate reads activeSlice() in refreshCwDecodeState()
//    (#4825).
//
// Which slice is the caller's decision; WHICH MODES COUNT is stated once here
// so the two answers cannot drift apart. An empty mode (no such slice) is not
// a voice mode, which is what keeps both gates closed when their slice is
// absent.
inline bool isVoiceMode(const QString& mode)
{
    return mode == QLatin1String("USB") || mode == QLatin1String("LSB")
        || mode == QLatin1String("AM")  || mode == QLatin1String("SAM")
        || mode == QLatin1String("FM")  || mode == QLatin1String("NFM")
        || mode == QLatin1String("DFM");
}

}  // namespace AetherSDR
