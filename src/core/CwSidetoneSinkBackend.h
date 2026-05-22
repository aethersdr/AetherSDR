#pragma once

#include <QAudioDevice>
#include <QString>

namespace AetherSDR {

class CwSidetoneGenerator;

// Abstract audio-sink backend for the CW sidetone path.  The same
// CwSidetoneGenerator drives every backend; what changes is the audio
// API used to push samples to the device.
//
// Two implementations:
//   - CwSidetoneQAudioSink  — Qt Multimedia QAudioSink + push-mode timer.
//                             Cross-platform, but pays the OS-mixer
//                             tax (50 ms buffer required to keep
//                             Pulse/PipeWire pull-mode happy).
//   - CwSidetonePortAudioSink — PortAudio direct callback path.  Sub-5 ms
//                               latency on Linux PipeWire and macOS
//                               CoreAudio.  Built only when HAVE_PORTAUDIO.
//
// AudioEngine owns one of these via std::unique_ptr; the factory picks
// based on build flag + AppSettings["CwSidetoneBackend"].
class CwSidetoneSinkBackend {
public:
    virtual ~CwSidetoneSinkBackend() = default;

    // Open the audio device and begin pushing sidetone samples generated
    // by `generator`.  Caller owns the generator and guarantees its
    // lifetime exceeds this backend.  Returns false on any failure
    // (device init, format negotiation, library error).
    virtual bool start(const QAudioDevice& device,
                       int desiredRateHz,
                       CwSidetoneGenerator* generator) = 0;

    // Close the audio device and release library resources.  Safe to
    // call even when the backend isn't running.
    virtual void stop() = 0;

    // True between successful start() and stop().
    virtual bool isRunning() const = 0;

    // Sample rate the device actually negotiated.  Undefined when not
    // running.  Used by AudioEngine to inform the generator so its
    // phase increments match.
    virtual int actualRateHz() const = 0;

    // Short backend name for logs ("QAudioSink" / "PortAudio").
    virtual const char* name() const = 0;

    // Runtime details used by the default-on audio summary log.
    virtual QString deviceDescription() const { return {}; }
    virtual bool fallbackOccurred() const { return false; }
    virtual QString fallbackReason() const { return {}; }
};

} // namespace AetherSDR
