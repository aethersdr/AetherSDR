#pragma once

#include <QAudioDevice>
#include <QString>

namespace AetherSDR {

class CwSidetoneGenerator;

// Abstract audio-sink backend for the CW sidetone; the same CwSidetoneGenerator
// drives every backend.
//   - CwSidetoneQAudioSink: QAudioSink + push timer; cross-platform, needs a
//     50 ms buffer for Pulse/PipeWire.
//   - CwSidetonePortAudioSink: direct callback, sub-5 ms on PipeWire/CoreAudio;
//     HAVE_PORTAUDIO only, not the Windows default (#5713).
// AudioEngine owns one via unique_ptr, chosen by CwSidetoneBackendPolicy.h.
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
