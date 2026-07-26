#pragma once

#include <QObject>

#include <complex>
#include <memory>
#include <vector>

#include "core/backends/hl2/Hl2Spectrum.h"
#include "core/dsp/WdspChannel.h"

namespace AetherSDR::hl2 {

// The HL2 receive DSP stage: turns raw IQ blocks (from MetisClient::iqBlockReady)
// into demodulated audio (WdspChannel), a panadapter spectrum (Hl2Spectrum), and
// an S-meter. Buffers the odd 126-sample EP6 blocks into WdspChannel's fixed
// processing block. Below the seam; the eventual Hl2Backend owns one and runs it
// on the backend's I/O thread. This stage is receive-only — Hl2TxDsp is its
// transmit counterpart — and it MUTES on transmit, clocking its audio channel
// with silence so the pipeline cannot fill with our own signal.
class Hl2RxDsp : public QObject {
    Q_OBJECT

public:
    explicit Hl2RxDsp(QObject* parent = nullptr);
    ~Hl2RxDsp() override;

    // WDSP's internal DSP rate. Constant at 48 kHz and independent of both the
    // HL2 IQ rate and the audio rate — see the note in configure().
    static constexpr int kWdspDspSampleRateHz = 48000;

    struct Config {
        int inputSampleRateHz = 48000;   // HL2 IQ sample rate
        // Demodulated-audio rate. 24 kHz because that is AudioEngine's native
        // RX rate (AudioEngine::DEFAULT_SAMPLE_RATE); emitting it directly means
        // the relay hands the engine byte-compatible float32 stereo with no
        // resampling. WDSP does the IF->audio decimation, and every HL2 IQ rate
        // (48/96/192/384 kHz) divides evenly into it.
        int audioSampleRateHz = 24000;
        int dspBlockSize = 1024;         // WdspChannel input/processing block
        int fftSize = 1024;              // panadapter FFT size
        WdspChannel::Mode mode = WdspChannel::Mode::Usb;
        double filterLowHz = 150.0;
        double filterHighHz = 3000.0;
        // RX AGC. WdspChannel's own defaults are mode 3 (medium) and a 120 dB
        // ceiling — 120 dB is the TOP of WDSP's AGC gain range, so leaving it
        // there runs the HL2 wide open and slams the demodulated audio past
        // full scale (measured: peak 3.19, 10% of samples clipping on WWV).
        // 39 dB is the slice model's default threshold of 65 through the
        // 0..100 -> 0..60 dB map (see Hl2Backend::setSliceAgc). Measured clean
        // on live hardware; the previous 65 dB clipped 60% of samples.
        int agcMode = 3;
        double maximumAgcGainDb = 39.0;   // = slice default 65 * 0.6
        // false (live): processIq is non-blocking — real-time input paces WDSP's
        // async worker and audio flows with ~1 block latency. true: processIq
        // waits for each output block (deterministic for a burst/offline feed).
        bool blockForOutput = false;
    };

    // (Re)build the WdspChannel + Hl2Spectrum for this config. Returns false (and
    // sets error, if given) when the WDSP channel cannot be created.
    Q_INVOKABLE bool configure(const Config& config, std::string* error = nullptr);
    Q_INVOKABLE void setMode(WdspChannel::Mode mode);
    Q_INVOKABLE void setFilter(double lowHz, double highHz);
    // Runtime AGC change. agcMode is the WDSP RXA AGC mode; maximumGainDb is
    // the AGC ceiling. Kept in m_config so a later reconfigure() (rate change)
    // rebuilds the channel with the operator's current AGC rather than the
    // construction-time default.
    Q_INVOKABLE void setAgc(int agcMode, double maximumGainDb);
    // RX frequency shift in Hz relative to the NCO — how the backend tunes the
    // slice inside the passband without moving the DDC. Kept in m_config so a
    // later reconfigure() rebuilds the channel with the operator's offset.
    Q_INVOKABLE void setShift(double shiftHz);
    // Mute the DEMODULATOR while transmitting.
    //
    // Suppressing audio further downstream is not enough: this pipeline keeps
    // demodulating our own transmission, WDSP's filters and buffers fill with
    // it, and the moment the mute lifts that backlog drains to the speakers —
    // heard as the tail end of a transmission playing back after unkey.
    //
    // Muted, the SPECTRUM still runs on real IQ so the panadapter keeps
    // updating, but the audio channel is clocked with SILENCE. The pipeline
    // therefore stays running at constant latency and contains nothing but
    // silence when transmit ends.
    Q_INVOKABLE void setAudioMuted(bool muted);
    [[nodiscard]] bool isConfigured() const noexcept { return m_channel != nullptr; }

public slots:
    // Feed one IQ block (normalized complex<float>). Emits spectrumReady per FFT
    // frame and audioReady/meterUpdate per completed WdspChannel block.
    void processIqBlock(const std::vector<std::complex<float>>& iq);

signals:
    void audioReady(const std::vector<float>& stereoPcm);   // interleaved L,R
    void spectrumReady(const std::vector<float>& binsDbfs); // DC-centred dBFS
    void meterUpdate(float dbfs);                           // audio-RMS S-meter

private:
    std::unique_ptr<WdspChannel> m_channel;
    std::unique_ptr<Hl2Spectrum> m_spectrum;
    double m_shiftHz = 0.0;   // current slice offset from the NCO, Hz
    Config m_config;

    bool m_audioMuted = false;
    std::vector<std::complex<float>> m_iqBuffer;   // IQ awaiting a full DSP block
    std::vector<float> m_i, m_q;                    // deinterleaved input scratch
    std::vector<float> m_left, m_right;             // WdspChannel output scratch
    std::vector<float> m_stereo;                    // interleaved audio out
    std::vector<float> m_bins;                      // spectrum scratch
};

}  // namespace AetherSDR::hl2
