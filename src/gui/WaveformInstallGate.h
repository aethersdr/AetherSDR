#pragma once

#include "models/ModelCapabilities.h"   // RadioPlatform, isKnownNonWfpPlatform

#include <QCoreApplication>
#include <QPointer>
#include <QString>

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

// What to tell the operator for a Docker install blocker; empty for None.
// One wording for every install path (the Waveforms window, AetherGPS).
inline QString dockerInstallBlockerMessage(DockerWaveformInstallBlocker blocker, bool wfpPowered,
                                           bool wfpReady)
{
    switch (blocker) {
    case DockerWaveformInstallBlocker::None:
        return {};
    case DockerWaveformInstallBlocker::NotConnected:
        return QCoreApplication::translate("WaveformInstallGate",
            "Connect to a radio before installing Docker waveform images.");
    case DockerWaveformInstallBlocker::UnsupportedPlatform:
        return QCoreApplication::translate("WaveformInstallGate",
            "This radio platform does not support on-radio Docker waveform deployment.");
    case DockerWaveformInstallBlocker::RuntimeStatusUnknown:
        return QCoreApplication::translate("WaveformInstallGate",
            "WFP runtime status has not been reported by this radio.");
    case DockerWaveformInstallBlocker::WfpNotReady:
        break;
    }
    return QCoreApplication::translate("WaveformInstallGate", "%1, %2")
        .arg(wfpPowered ? QCoreApplication::translate("WaveformInstallGate", "WFP ON")
                        : QCoreApplication::translate("WaveformInstallGate", "WFP OFF"),
             wfpReady ? QCoreApplication::translate("WaveformInstallGate", "READY")
                      : QCoreApplication::translate("WaveformInstallGate", "NOT READY"));
}

// Why AetherGPS's "Install AetherGPS on the radio" checkbox is unavailable,
// or None. The relay ships as a legacy waveform for a radio without a
// Waveform Processor (the FLEX-6000) and as a container for the rest, which
// the Docker gate governs. Unlike the health tools it needs no LAN address
// to install: AetherGPS reaches it on the LAN or through the tailnet shim.
enum class AetherGpsBlocker {
    None,
    NotConnected,
    NoWaveforms,   // this radio family installs no waveforms
    Container,     // the container path, held by the Docker gate
};

inline AetherGpsBlocker aetherGpsBlocker(bool connected, bool hasWaveforms, bool nativePackage,
                                         DockerWaveformInstallBlocker docker)
{
    if (!connected) {
        return AetherGpsBlocker::NotConnected;
    }
    if (!hasWaveforms) {
        return AetherGpsBlocker::NoWaveforms;
    }
    if (!nativePackage && docker != DockerWaveformInstallBlocker::None) {
        return AetherGpsBlocker::Container;
    }
    return AetherGpsBlocker::None;
}

}  // namespace AetherSDR
