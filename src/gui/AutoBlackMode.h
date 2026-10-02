#pragma once

#include <algorithm>

// Waterfall Black Level mode arithmetic as pure functions. Positions: Off
// (manual), SW (client noise-floor estimate), HW (radio per-tile level, only if
// RadioCapabilities::hasRadioSideWaterfallAutoBlack). The stored value is the
// operator's INTENT and the capability masks it (#4606): connecting a radio
// without HW never rewrites intent; only a click does. Separate from
// SpectrumOverlayMenu so it links into a test.

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
// Not while a pan shows KiwiSDR: there the button is a one-shot "Auto" and the
// slider is the Kiwi floor (-260..29 dBm). Use this predicate at every call
// site (#4606).
inline bool ownsSharedWidgets(bool kiwiWaterfallControlMode)
{
    return !kiwiWaterfallControlMode;
}

}  // namespace AetherSDR::AutoBlackMode
