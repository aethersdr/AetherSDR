#pragma once

// CwSidetoneStartPolicy — which device the sidetone backend gets at start()
// (#4978).
//
// startSidetoneStream() resolves a QAudioDevice the same way the RX sink does
// (saved AudioOutputDeviceId if still enumerable, else Qt's default). The two
// backends give a null device opposite meanings:
//
//   PortAudio   null = "resolve your own default output"
//               (Pa_GetDefaultOutputDevice). The intended path for a default
//               selection; a Qt-description-to-PortAudio-name match cannot
//               succeed across PulseAudio/PipeWire vs ALSA naming on Linux.
//   QAudioSink  null = "requested output unavailable -> system default",
//               flagged fallbackOccurred=true, so it always gets the resolved
//               device.
//
// So PortAudio gets a null device exactly when the selection is not explicit.
// A selection is explicit when a device id is saved AND still enumerable, even
// if that device is the system default; that case still takes the name-match
// path (#4978 stays open for it).
//
// The decision is platform-independent; the platform enters only through
// CwSidetoneBackendPolicy.h. Pure, header-only, no Qt types; every case is a
// compile-time assert and a row in tests/cw_sidetone_start_policy_test.cpp.

namespace AetherSDR {

enum class SidetoneStartDevice {
    // Hand the backend the resolved QAudioDevice (saved device, or Qt's
    // default). Always the answer for QAudioSink; the answer for PortAudio
    // only when the selection is explicit.
    Resolved,
    // Hand the backend a null QAudioDevice so it resolves its own default
    // output. PortAudio only, non-explicit selection only.
    BackendDefault,
};

// `savedDeviceSet`         AudioEngine::m_outputDevice is non-null — an
//                          AudioOutputDeviceId is saved.
// `savedDeviceEnumerable`  that id was found in QMediaDevices::audioOutputs()
//                          at start time. False when the device is gone; in
//                          practice startRxStream() has already nulled a
//                          missing saved device before the sidetone starts, so
//                          this arrives as savedDeviceSet=false on the normal
//                          path — the row is kept for the Q_INVOKABLE entry
//                          point and for a hotplug between the two
//                          enumerations.
constexpr bool isExplicitSidetoneSelection(bool savedDeviceSet,
                                           bool savedDeviceEnumerable)
{
    return savedDeviceSet && savedDeviceEnumerable;
}

// `explicitSelection`   isExplicitSidetoneSelection(...) above.
// `backendIsPortAudio`  the constructed sink reports name() == "PortAudio"
//                       (false when HAVE_PORTAUDIO is off, on Windows unless
//                       the operator opted in with CwSidetoneBackend=PortAudio,
//                       and anywhere the operator opted out with
//                       CwSidetoneBackend=QAudioSink). See
//                       CwSidetoneBackendPolicy.h.
constexpr SidetoneStartDevice sidetoneStartDevice(bool explicitSelection,
                                                  bool backendIsPortAudio)
{
    if (backendIsPortAudio && !explicitSelection)
        return SidetoneStartDevice::BackendDefault;
    return SidetoneStartDevice::Resolved;
}

} // namespace AetherSDR
