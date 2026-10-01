#pragma once

// Which device the sidetone backend gets at start() (#4978). startSidetoneStream()
// resolves a QAudioDevice like the RX sink (saved AudioOutputDeviceId if still
// enumerable, else Qt's default). A null device means:
//   PortAudio   "resolve your own default" (Pa_GetDefaultOutputDevice); Qt and
//               ALSA names can't be matched on Linux.
//   QAudioSink  "requested output unavailable" (fallbackOccurred=true), so it
//               always gets the resolved device.
// PortAudio gets null exactly when the selection is not explicit (a saved id
// that is still enumerable, even if it is the default). Platform-independent;
// pure header, every case a static_assert and a row in
// tests/cw_sidetone_start_policy_test.cpp.

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

// `savedDeviceSet`        an AudioOutputDeviceId is saved (m_outputDevice set).
// `savedDeviceEnumerable` that id is in audioOutputs() at start. startRxStream()
//                         normally nulls a missing device first; this row covers
//                         the Q_INVOKABLE entry and a hotplug in between.
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
