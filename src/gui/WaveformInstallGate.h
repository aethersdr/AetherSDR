#pragma once

#include "models/ModelCapabilities.h"   // RadioPlatform, isKnownNonWfpPlatform

namespace AetherSDR {

// Why "Install -> Docker Waveform Image..." is blocked, or None (#4210).
// Follows live WFP runtime state plus the no-WFP-hardware check, NOT the
// FlexLib "wfp" license feature: a radio with WFP powered and ready can report
// that feature disabled and still accept installs (#4186).
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
