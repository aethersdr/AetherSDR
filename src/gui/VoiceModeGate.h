#pragma once

#include "models/ModeVocabulary.h"

#include <QString>

namespace AetherSDR {

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
