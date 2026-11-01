#pragma once

#include "models/ModelCapabilities.h"   // RadioPlatform, isKnownNonWfpPlatform

#include <QPointer>

namespace AetherSDR {

// Whether Install -> Remote Access fetches the shim as a legacy waveform:
// yes for a radio with no Waveform Processor; every other one takes the
// container, which the Docker gate below still governs.
inline bool remoteAccessUsesNativePackage(RadioPlatform platform)
{
    return isKnownNonWfpPlatform(platform);
}

// The radio a package may be installed to once its download or file picker
// finishes: the one it was chosen for, in the same control session, or null.
// The RadioModel object outlives a reconnect, so the pointer alone can't stop
// a FLEX-6000 package reaching a different radio. A template, so this header
// needs no model include; Model is RadioModel.
template <typename Model, typename Session>
Model* radioStillChosen(const QPointer<Model>& model, const Session& chosen)
{
    return model && model->controlSessionCurrent(chosen) ? model.data() : nullptr;
}

// Why "Install radio health tools" (and its readout) is unavailable, or
// None. The agent is a legacy waveform answering on the radio's LAN
// address, so it needs a connected radio that takes waveforms, reached
// directly over its local network (not SmartLink, whose remembered LAN
// address may still look private).
enum class RadioHealthBlocker {
    None,
    NotConnected,
    NoWaveforms,    // this radio family installs no waveforms
    Remote,         // a SmartLink (WAN) session
    NotLan,         // reached at a non-private address (e.g. over a tailnet)
};

inline RadioHealthBlocker radioHealthBlocker(bool connected, bool hasWaveforms,
                                             bool wan, bool lanAddress)
{
    if (!connected) {
        return RadioHealthBlocker::NotConnected;
    }
    if (!hasWaveforms) {
        return RadioHealthBlocker::NoWaveforms;
    }
    if (wan) {
        return RadioHealthBlocker::Remote;
    }
    return lanAddress ? RadioHealthBlocker::None : RadioHealthBlocker::NotLan;
}

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
