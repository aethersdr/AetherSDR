#pragma once

#include "models/ModelCapabilities.h"   // RadioPlatform

namespace AetherSDR {

// Platforms with no on-radio WFP hardware at all (6000-series Microburst /
// DeepEddy). This is a genuine hard incompatibility for Docker waveform
// deployment, distinct from the "wfp" license feature (see below).
inline bool isKnownNonWfpPlatform(RadioPlatform platform)
{
    return platform == RadioPlatform::Microburst
        || platform == RadioPlatform::DeepEddy;
}

// Why the "Install -> Docker Waveform Image..." action is blocked, or None if
// it should be enabled.
//
// Pure policy (#4210): install availability follows the radio's LIVE WFP
// runtime state plus the no-WFP-hardware platform check — and deliberately NOT
// the FlexLib "wfp" license feature. That feature reflects a SmartSDR+/EA-style
// entitlement that is decoupled from whether the radio will actually accept a
// file-upload install: a radio with WFP powered + ready (even one already
// running an installed Docker waveform) can report the feature disabled, so
// gating on it wrongly blocked further installs (#4186 regression from #3585).
enum class DockerWaveformInstallBlocker {
    None,                   // enabled
    NotConnected,           // no radio connected
    UnsupportedPlatform,    // Microburst/DeepEddy — no WFP hardware
    RuntimeStatusUnknown,   // radio hasn't reported WFP status yet
    WfpNotReady,            // WFP not powered and/or not ready
};

inline DockerWaveformInstallBlocker dockerWaveformInstallBlocker(
    bool connected, RadioPlatform platform,
    bool wfpStatusSeen, bool wfpPowered, bool wfpReady)
{
    if (!connected) {
        return DockerWaveformInstallBlocker::NotConnected;
    }
    if (isKnownNonWfpPlatform(platform)) {
        return DockerWaveformInstallBlocker::UnsupportedPlatform;
    }
    if (!wfpStatusSeen) {
        return DockerWaveformInstallBlocker::RuntimeStatusUnknown;
    }
    if (!wfpPowered || !wfpReady) {
        return DockerWaveformInstallBlocker::WfpNotReady;
    }
    return DockerWaveformInstallBlocker::None;
}

}  // namespace AetherSDR
