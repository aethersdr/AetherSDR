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

// AetherSDR's older neutral vocabulary used CW for upper-side CW. Icom and
// HL2 report that same mode explicitly as CWU; CWL is the reverse-side mode.
inline bool isCwMode(const QString& mode)
{
    return mode == QLatin1String("CW") || mode == QLatin1String("CWU")
        || mode == QLatin1String("CWL");
}

// Whether an ALREADY-OPEN Copy Assist panel should be torn down, as
// MainWindow::updateKeyerAvailability() decides it. Pure, so the decision can
// be pinned by a test — updateKeyerAvailability() itself cannot be, since
// MainWindow is not linkable from the gating-test target.
//
// The asymmetry that shapes every clause below: updateKeyerAvailability() only
// ever HIDES — showing is user-driven — and hiding calls setAsrEnabled(false),
// which drops the audio tap and unticks Enable. So a hide the operator did not
// ask for costs them a running transcription and cannot be undone by any later
// refresh, while declining to hide costs a click. Every uncertain case
// therefore resolves to false.
//
//  * `panelVisible` — nothing to tear down otherwise.
//  * `haveActiveSlice` — an ABSENT slice is not a mode change. A single-slice
//    band recall empties slices() (band_persistence drops and re-creates the
//    slice under the same id: KiwiRebindTracker.h, #4158), and a disconnect
//    clears them all.
//  * `activeSliceMode` — the actual gate: the operator selected a slice that
//    carries nothing transcribable.
//  * `bandRecallInFlight` — the same recall reaching this code one slice
//    further along. With a second slice on the pan, onSliceRemoved() re-selects
//    it rather than taking the empty-slices branch, so the gate is handed a
//    live CW/DIGx slice mid-rebuild and `haveActiveSlice` no longer protects
//    it. BandRecallSelectionGuard already owns "this pan is rebuilding, treat
//    radio-driven selection as synchronization-only" (#4932 review).
inline bool shouldAutoHideCopyAssist(bool panelVisible,
                                     bool haveActiveSlice,
                                     const QString& activeSliceMode,
                                     bool bandRecallInFlight)
{
    return panelVisible && haveActiveSlice && !bandRecallInFlight
        && !isVoiceMode(activeSliceMode);
}

}  // namespace AetherSDR
