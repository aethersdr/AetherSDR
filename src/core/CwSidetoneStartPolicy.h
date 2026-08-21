#pragma once

// CwSidetoneStartPolicy — which device does the CW sidetone backend get handed
// at start()? (#4978)
//
// AudioEngine::startSidetoneStream() resolves a QAudioDevice for the sidetone
// the same way the RX sink does: the saved AudioOutputDeviceId if it is still
// enumerable, otherwise Qt's current default output. Before #4978 that
// resolved device was handed to the backend unconditionally. For the PortAudio
// backend that meant every start went through a cross-API NAME match (Qt's
// device description against PortAudio's device names). On Linux those come
// from different audio APIs — PulseAudio/PipeWire descriptions such as
// "Built-in Audio Analog Stereo" versus ALSA names such as
// "HDA Intel PCH: ALC257 Analog (hw:1,0)", "pulse", "default" — so the match
// cannot succeed for analog/USB device descriptions and a box with no saved
// selection silently landed on the QAudioSink push path. The PortAudio
// backend's own default-device branch (device.isNull()) existed all along but
// was unreachable, because no caller ever passed it a null device.
//
// The two backends give a null device OPPOSITE meanings, which is why this is a
// policy and not a one-liner at the call site:
//
//   PortAudio   null  = "resolve your own default output"
//                       (Pa_GetDefaultOutputDevice — the same notion of
//                       default the rest of the system mixer uses). Not a
//                       fallback; the intended path for a default selection.
//   QAudioSink  null  = "requested output unavailable -> system default",
//                       flagged fallbackOccurred=true in the summary. Handing
//                       it a null for a deliberate default selection would
//                       log a fallback that did not happen.
//
// So: a PortAudio backend gets a null device exactly when the selection is not
// explicit; a QAudioSink backend always gets the resolved device.
//
// ── What "explicit" means, and the limit it documents ───────────────────────
//
// A selection is explicit when a device id is SAVED and that id is still
// ENUMERABLE. It is explicit EVEN IF the saved device happens to be the system
// default: the user picked it from the Radio Setup output combo, and the
// escape-hatch follow-up in #4978 (not this file) is where an explicit
// selection that fails the name match gets a second chance. A box whose saved
// device is the system default therefore still takes the name-match path on
// Linux and still falls back to QAudioSink — that is the documented reach of
// the #4978 fix, pinned here so a later edit cannot widen or narrow it by
// accident. #4978 stays open for that case.
//
// Pure and header-only, with no Qt types, so every case is a compile-time
// assertion and a unit-test row (tests/cw_sidetone_start_policy_test.cpp) —
// the #3306 pattern (policy as a pure function over injected facts, platform
// as data) applied to device SELECTION rather than format negotiation. The
// decision itself is platform-independent: the operating system enters only
// through whether a PortAudio backend was constructed at all (HAVE_PORTAUDIO
// is pkg-config-gated, so Windows builds never construct one and always take
// the `Resolved` row). Mirrors QsoRecordStartPolicy / DaxTxPolicy.

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
//                       (false when HAVE_PORTAUDIO is off, or when the user
//                       opted out with CwSidetoneBackend=QAudioSink).
constexpr SidetoneStartDevice sidetoneStartDevice(bool explicitSelection,
                                                  bool backendIsPortAudio)
{
    if (backendIsPortAudio && !explicitSelection)
        return SidetoneStartDevice::BackendDefault;
    return SidetoneStartDevice::Resolved;
}

} // namespace AetherSDR
