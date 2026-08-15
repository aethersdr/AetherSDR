#pragma once

#include "ITxCaptureSource.h"

#include <QObject>
#include <QString>
#include <QVector>

namespace AetherSDR::txlin {

// ITxCaptureSource over a FlexRadio DAX IQ stream.
//
// STREAM SETUP. The tree already has the whole DAX IQ path: DaxIqModel creates
// and tears down streams, PanadapterStream receives the VITA-49 payload on the
// existing UDP socket, and a worker thread does the byte-swap before the
// samples surface as DaxIqModel::iqSamplesReady. This adapter therefore does
// not create a socket, a stream lifecycle or a parser of its own — it consumes
// that path, which is also the path that has been debugged against real
// hardware.
//
// The published command syntax, verified against FlexLib (Radio.cs:4800,
// DAXIQStream.cs:54, Panadapter.cs:259) per Constitution Principle I:
//
//     stream create type=dax_iq daxiq_channel=<n>
//     stream set 0x<id> daxiq_rate=<24000|48000|96000|192000>
//     display pan set <panId> daxiq_channel=<n>
//
// Note this differs from the syntax in the original feature brief
// (`stream create daxiq=<ch> ip=<ip> port=<port>`) — there is no ip/port on the
// create, because the radio streams to the client's existing VITA-49 socket.
// FlexLib and the shipping code agree; the brief does not.
//
// NO MODEL DEPENDENCY, BY RULE AND ON THE MERITS. This class holds no pointer
// to DaxIqModel and includes none of its headers: samples arrive through
// feedIqSamples() and stream operations leave as request signals, which the
// owning UI connects to the radio. The engine-boundary ratchet (EB3, aetherd
// RFC §2.4) forbids new above-seam code reaching around IRadioBackend to a
// vendor model, and the decoupling it forces is the better design anyway —
// what is left here is exactly the frame assembler and capture gate, with none
// of the DAX plumbing, which is what the HPSDR adapter will want to mirror.
//
// SAMPLE RATE. Always requests the highest rate the pan allows, 192 kHz, for
// the reason set out in CaptureConfig: order-7 products sit three tone-spacings
// beyond each tone, and the capture needs enough clean bandwidth beyond that
// for the band-edge filter skirt never to reach the measurement region.
//
// NO REFERENCE CHANNEL, AND WHY THAT IS PERMANENT ON THIS BACKEND. DAX IQ is
// unidirectional. There is no API to send IQ to the radio for transmission; the
// only inbound path is the TX Audio stream, which carries audio. An SSB
// modulator derives the analytic signal from a single real input via Hilbert
// transform, so the complex envelope's phase is determined by its amplitude and
// an arbitrary complex correction cannot be expressed at all. And the radio's
// TX filter sits downstream of injected audio, so any out-of-band correction
// product is removed while any in-band one is added distortion. This is an
// architectural property of the radio, not a gap to be worked around later.
//
// THREADING. Frames are assembled on the thread that delivers the IQ — today
// the main thread, since DaxIqModel's worker relays across. The assembly is a
// bounded memcpy into a preallocated buffer: no allocation and no DSP, which is
// the constraint that actually matters. The FFT work happens on
// TxLinearityController's analysis thread, fed through a lock-free ring. A
// third capture thread of our own would mean duplicating the VITA-49 and
// byte-swap path that DaxIqWorker already owns, for no benefit.
class FlexDaxIqCapture : public QObject, public ITxCaptureSource {
    Q_OBJECT

public:
    // A FLEX offers four DAX IQ channels. Stated here rather than reached for
    // from DaxIqModel so this file keeps no vendor-model dependency; the UI
    // that populates the channel chooser reads the model's own constant.
    static constexpr int kMaxChannels = 4;

    explicit FlexDaxIqCapture(int channel, QObject* parent = nullptr);
    ~FlexDaxIqCapture() override;

    bool start(const CaptureConfig& config) override;
    void stop() override;

    [[nodiscard]] bool hasReferenceChannel() const override { return false; }
    [[nodiscard]] std::string describe() const override;

    void setFrameCallback(FrameCallback callback) override;
    void setErrorCallback(ErrorCallback callback) override;
    void setGateOpen(bool open) override;

    [[nodiscard]] bool gateOpen() const { return m_gateOpen; }
    [[nodiscard]] int channel() const { return m_channel; }
    [[nodiscard]] double actualSampleRateHz() const { return m_actualRate; }

    // Identity for export stamps, pushed down by the UI so the core never has
    // to know about RadioModel.
    void setRadioDescription(const QString& text) { m_radioDescription = text; }

    // Panadapter watching the feedback port. Binding the DAX IQ channel to it
    // is what centres the IQ window on the transmitted signal; without it the
    // channel keeps whatever pan it was last assigned to and the signal can
    // fall outside the +/-fs/2 window entirely — the same trap WfmDemodulator
    // documents.
    void setPanId(const QString& panId) { m_panId = panId; }

public slots:
    // Interleaved I/Q float32 for `channel`, native endian, as delivered by the
    // radio's DAX IQ stream. Frames outside the gate are discarded here.
    void feedIqSamples(int channel, const QVector<float>& interleaved, int sampleRateHz);

    // The radio dropped the stream underneath us. Treated as a terminal capture
    // error: a keyed radio whose measurement path has died has no reason to
    // still be transmitting.
    void noteStreamRemoved(int channel);

    // The rate the radio actually settled on, which is authoritative over the
    // requested one — a stream whose re-rate is still settling reports the
    // 48 kHz default transiently.
    void noteStreamRate(int channel, int sampleRateHz);

signals:
    void streamStartRequested(int channel, int sampleRateHz);
    void streamStopRequested(int channel);
    // Raw command for the pan binding, in the radio's own syntax.
    void commandReady(const QString& command);

private:
    void emitError(const std::string& reason);

    int m_channel = 1;
    QString m_panId;
    QString m_radioDescription;

    CaptureConfig m_config;
    bool m_running = false;
    bool m_gateOpen = false;
    double m_actualRate = 0.0;

    // Frame assembly. Preallocated at start(); never resized while running, so
    // the delivery path allocates nothing.
    std::vector<std::complex<float>> m_frame;
    std::size_t m_filled = 0;
    // Rolling count of saturated samples in the frame being assembled, so the
    // clipped verdict costs one comparison per sample rather than a second pass.
    std::size_t m_saturated = 0;

    FrameCallback m_frameCallback;
    ErrorCallback m_errorCallback;
};

}  // namespace AetherSDR::txlin
