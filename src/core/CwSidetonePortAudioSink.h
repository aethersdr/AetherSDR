#pragma once

#include "CwSidetoneSinkBackend.h"
#include "CwSidetoneEdgeProbe.h"

#include <portaudio.h>

#include <QString>

#include <atomic>

namespace AetherSDR {

class CwSidetoneGenerator;

// PortAudio sidetone backend: the driver calls paCallback() at native buffer
// intervals (typically 64-128 frames, ~1.5-3 ms on PipeWire/CoreAudio); no Qt
// event loop, timer or copy. Sub-5 ms key-down to onset vs ~25 ms with
// QAudioSink. Built only with HAVE_PORTAUDIO.
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
    QString deviceDescription() const override { return m_deviceDescription; }
    bool fallbackOccurred() const override { return m_fallbackOccurred; }
    QString fallbackReason() const override { return m_fallbackReason; }

private:
    static int paCallback(const void* input, void* output,
                          unsigned long frameCount,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void* userData);

    PaStream*                          m_stream{nullptr};
    std::atomic<CwSidetoneGenerator*>  m_generator{nullptr};
    // Diagnostics: callback liveness + the loudest sample rendered, so a
    // "started but silent" report can distinguish a dead callback from a
    // generator that renders silence. Logged by stop().
    std::atomic<quint64>               m_cbCount{0};
    std::atomic<quint32>               m_cbPeakMicro{0};   // |sample| * 1e6
    // Stream-prime callbacks exempt from underflow counting. Filling a
    // freshly started ring reports paOutputUnderflow on essentially every
    // host, on a stream that has missed no deadline — counting it made the
    // stop() warning fire on every session, keyed or not. (#5200)
    static constexpr quint64 kPrimeCallbacks = 2;

    // paOutputUnderflow / paOutputOverflow counts. The callback used to
    // discard statusFlags, which left the #4890 element-timing tail
    // (2 outliers in 196, +/-8-11 ms) attributable only by guess: an
    // underflow and host wake jitter look identical in the envelope. These
    // separate them — an outlier with a coincident underflow is the audio
    // system missing its deadline, one without is jitter somewhere else.
    std::atomic<quint32>               m_cbUnderflows{0};
    std::atomic<quint32>               m_cbOverflows{0};
    CwSidetoneEdgeProbe                m_edgeProbe;
    int                                m_actualRate{0};
    QString                            m_deviceDescription;
    bool                               m_fallbackOccurred{false};
    QString                            m_fallbackReason;
    bool                               m_paInitialized{false};
};

} // namespace AetherSDR
