#pragma once

#include "CwSidetoneSinkBackend.h"

#include <portaudio.h>

#include <atomic>

namespace MasterSDR {

class CwSidetoneGenerator;

// PortAudio-based sidetone backend.  Direct callback path: the audio
// device's driver calls our paCallback() at native buffer intervals
// (typically 64–128 frames on Linux PipeWire / macOS CoreAudio,
// ~1.5–3 ms).  No Qt event loop, no timer, no buffer copy other than
// the silence-init in the callback.
//
// Net perceived latency on Linux PipeWire / macOS CoreAudio:
// sub-5 ms key-down → audible-onset, vs ~25 ms with QAudioSink.
//
// Built only when HAVE_PORTAUDIO.
class CwSidetonePortAudioSink : public CwSidetoneSinkBackend {
public:
    CwSidetonePortAudioSink();
    ~CwSidetonePortAudioSink() override;

    bool start(const QAudioDevice& device,
               int desiredRateHz,
               CwSidetoneGenerator* generator) override;
    void stop() override;
    bool isRunning() const override { return m_stream != nullptr; }
    int  actualRateHz() const override { return m_actualRate; }
    const char* name() const override { return "PortAudio"; }

private:
    static int paCallback(const void* input, void* output,
                          unsigned long frameCount,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void* userData);

    PaStream*                          m_stream{nullptr};
    std::atomic<CwSidetoneGenerator*>  m_generator{nullptr};
    int                                m_actualRate{0};
    bool                               m_paInitialized{false};
};

} // namespace MasterSDR
