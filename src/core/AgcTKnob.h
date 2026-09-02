#pragma once

// AGC-T is ONE operator knob backed by TWO radio properties, selected by the
// slice's AGC mode (FlexLib Slice.cs: `AGCThreshold` <-> `agc_threshold` while
// AGC is slow/med/fast; `AGCOffLevel` <-> `agc_off_level` while AGC is off --
// see docs/agc-t-calibration-design.md). The GUI slider has honoured that
// split since #1183; the controller surfaces -- the MIDI/StreamDeck/Ulanzi
// parameter registry, the FlexControl/TMate2 wheel funnel, the keyboard
// steps -- wrote `agc_threshold` unconditionally, so with AGC off a controller
// moved a property the radio was not listening to while the on-screen slider,
// bound to `agc_off_level`, stayed put (#5384). Every non-GUI AGC-T surface
// routes through these helpers so the mode decision lives in exactly one
// place.
//
// Ranges: `agc_off_level` is 0..100 on every backend
// (SliceModel::setAgcOffLevel clamps there for the Flex and external paths
// alike); the threshold keeps its per-backend span -- 0..100 on a Flex slice,
// KiwiSdrProtocol's dB span while an external receiver replaces the slice
// audio.

#include "core/KiwiSdrProtocol.h"
#include "models/SliceModel.h"

#include <QString>

namespace AetherSDR::AgcTKnob {

// True while the slice's AGC is off: the knob then means the fixed gain
// (`agc_off_level`), not the AGC knee (`agc_threshold`). Reads the
// receive-side mode so an external receiver's own AGC state is honoured
// (Principle II).
inline bool usesOffLevel(const SliceModel* slice)
{
    return slice && slice->receiveAgcMode() == QStringLiteral("off");
}

inline int minimum(const SliceModel* slice)
{
    if (usesOffLevel(slice)) {
        return 0;
    }
    return slice && slice->externalReceiveReplacementActive()
        ? KiwiSdrProtocol::kAgcThresholdMinDb
        : 0;
}

inline int maximum(const SliceModel* slice)
{
    if (usesOffLevel(slice)) {
        return 100;
    }
    return slice && slice->externalReceiveReplacementActive()
        ? KiwiSdrProtocol::kAgcThresholdMaxDb
        : 100;
}

// The knob's current value, read from whichever property the mode selects.
inline int level(const SliceModel* slice)
{
    if (!slice) {
        return 0;
    }
    return usesOffLevel(slice) ? slice->receiveAgcOffLevel()
                               : slice->receiveAgcThreshold();
}

// Write the knob into whichever property the mode selects. The SliceModel
// setters clamp and de-duplicate; nothing is re-asserted here.
inline void setLevel(SliceModel* slice, int value)
{
    if (!slice) {
        return;
    }
    if (usesOffLevel(slice)) {
        slice->setAgcOffLevel(value);
    } else {
        slice->setAgcThreshold(value);
    }
}

} // namespace AetherSDR::AgcTKnob
