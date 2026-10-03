#pragma once

#include <QLatin1String>
#include <QString>

namespace AetherSDR {

// Voice modes for the status-bar gates: SSB, AM/SAM and the FM family (not CW,
// RTTY, DIGx, FreeDV/RADE). The DVK indicator asks about the TX slice (#4173);
// Copy Assist asks about the active slice, like refreshCwDecodeState() (#4825).
// One list so the two can't drift. An empty mode (no slice) is not voice.
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

// Whether an already-open Copy Assist panel should be torn down (the pure half
// of MainWindow::updateKeyerAvailability(), which only ever hides). Hiding stops
// the transcription and can't be undone by a refresh, so uncertain cases return
// false: no visible panel; no active slice (band recall drops and re-creates
// slices, #4158; disconnect); or a band recall in flight, where a live CW/DIGx
// slice can be selected mid-rebuild (BandRecallSelectionGuard, #4932). Only a
// non-voice active slice mode hides.
inline bool shouldAutoHideCopyAssist(bool panelVisible,
                                     bool haveActiveSlice,
                                     const QString& activeSliceMode,
                                     bool bandRecallInFlight)
{
    return panelVisible && haveActiveSlice && !bandRecallInFlight
        && !isVoiceMode(activeSliceMode);
}

}  // namespace AetherSDR
