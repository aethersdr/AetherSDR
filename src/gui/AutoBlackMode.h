#pragma once

#include <algorithm>

// The waterfall Black Level control's mode arithmetic, as pure functions.
//
// The control has three positions — Off (manual level), SW (this client's
// noise-floor estimate), HW (the radio's per-tile level) — but HW only exists on
// a radio that computes one (RadioCapabilities::hasRadioSideWaterfallAutoBlack).
//
// The design this encodes (#4606): the stored value is the operator's INTENT and
// the capability is a MASK over it. Intent is never rewritten by connecting a
// radio that cannot serve it, so a Flex user who chose HW still has HW after a
// session on an HL2. Only a deliberate click changes intent.
//
// Extracted here because SpectrumOverlayMenu cannot be linked into a test on its
// own — it drags in SpectrumWidget, KiwiSdrManager, SliceModel and the rest of
// the GUI. Both bugs this file's tests pin shipped green through a full CI run.

namespace AetherSDR::AutoBlackMode {

inline constexpr int kOff = 0;
inline constexpr int kSoftware = 1;
inline constexpr int kHardware = 2;

// Intent keeps the full range even while HW is masked off — that is what lets it
// survive a session on a radio without one.
inline int clampIntent(int mode)
{
    return std::clamp(mode, kOff, kHardware);
}

// Reachable positions: Off/SW/HW, or Off/SW when the radio computes no level.
inline int modeCount(bool radioSideAvailable)
{
    return radioSideAvailable ? 3 : 2;
}

// Intent masked by the capability — what the button shows and what the app acts
// on. A masked HW resolves to SW rather than Off: the intent behind HW was "pick
// the floor for me", and the client-side estimate is the one that still can.
inline int effective(int intent, bool radioSideAvailable)
{
    const int clamped = clampIntent(intent);
    if (clamped == kHardware && !radioSideAvailable) {
        return kSoftware;
    }
    return clamped;
}

// One click advances from what the operator can SEE, not from a masked intent —
// otherwise a masked HW would advance to HW-as-SW again and read as a dead
// button. A click is a real choice on this radio, so the caller stores the
// result as the new intent; that is the one case where losing a stashed HW is
// correct.
inline int nextOnClick(int intent, bool radioSideAvailable)
{
    return (effective(intent, radioSideAvailable) + 1)
        % modeCount(radioSideAvailable);
}

// The source flag in the same masked form, for the renderer and for anything
// told to the radio.
inline bool effectiveRadioSide(bool intentRadioSide, bool radioSideAvailable)
{
    return intentRadioSide && radioSideAvailable;
}

// Whether the Off/SW/HW cycle owns the shared Black Level button and slider.
//
// It does not while a pan is displaying KiwiSDR: there the button is a one-shot
// "Auto" and the slider is the Kiwi floor in dBm over a -260..29 range, neither
// of which has anything to do with this cycle. Writing the cycle's label and its
// 0..100 offset into them relabels "Auto" as "SW" and jams an out-of-range value
// into the floor slider.
//
// A predicate rather than a check at each call site: until #4606 every caller
// happened to be kiwi-guarded, and the first one that was not shipped the bug.
inline bool ownsSharedWidgets(bool kiwiWaterfallControlMode)
{
    return !kiwiWaterfallControlMode;
}

}  // namespace AetherSDR::AutoBlackMode
