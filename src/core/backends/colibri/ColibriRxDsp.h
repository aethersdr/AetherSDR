#pragma once

#include <QElapsedTimer>
#include <QObject>

#include <complex>
#include <memory>
#include <vector>

#include "core/backends/colibri/ColibriSpectrum.h"
#include "core/dsp/WdspChannel.h"

namespace AetherSDR::colibri {

// The ColibriNANO receive DSP stage: turns raw IQ blocks (from
// ColibriDevice::iqBlockReady) into demodulated audio (WdspChannel), a
// panadapter spectrum (ColibriSpectrum), and an S-meter. Buffers the DLL's
// arbitrary-length callback blocks into WdspChannel's fixed processing block.
// Below the seam; ColibriBackend owns one and runs it on the backend's I/O
// thread. Receive-only — this radio has no transmitter.
//
// Modeled on hl2::Hl2RxDsp, with the wire handedness made explicit
// (Config::wireAnalytic) instead of hardwired to the HPSDR convention — see
// processIqBlock() for the two arrangements and the design doc for why the
// slice-shift sign is the same in both.
class ColibriRxDsp : public QObject {
    Q_OBJECT

public:
    explicit ColibriRxDsp(QObject* parent = nullptr);
    ~ColibriRxDsp() override;

    // WDSP's internal DSP rate. Constant at 48 kHz and independent of both the
    // IQ rate and the audio rate — see the note in configure().
    static constexpr int kWdspDspSampleRateHz = 48000;

    struct Config {
        int inputSampleRateHz = 48000;   // ColibriNANO IQ sample rate
        // Demodulated-audio rate. 24 kHz because that is AudioEngine's native
        // RX rate — emitting it directly hands the engine byte-compatible
        // float32 stereo with no resampling. WDSP does the IF->audio
        // decimation, and every Colibri IQ rate divides evenly into it.
        int audioSampleRateHz = 24000;
        int dspBlockSize = 1024;         // WdspChannel input/processing block
        int fftSize = 1024;              // panadapter FFT size
        WdspChannel::Mode mode = WdspChannel::Mode::Usb;
        double filterLowHz = 150.0;
        double filterHighHz = 3000.0;
        // RX AGC. 58.5 dB is the slice default threshold of 65 through the
        // backend's 0..100 -> 0..90 dB map, measured on the air as the level
        // FT8 decodes at: too high clips the audio, too low buries it, and on
        // this receiver the AGC sits AT the ceiling rather than regulating
        // below it (see ColibriBackend::kAgcCeilingDbPerUnit for both
        // measurements).
        int agcMode = 3;
        double maximumAgcGainDb = 58.5;
        // True (expected for this library): the wire is the ANALYTIC
        // convention — a signal above the NCO arrives at a positive frequency.
        // False: the wire is the HPSDR handedness (the conjugate). See
        // processIqBlock() for what each arrangement conjugates.
        bool wireAnalytic = true;
        // false (live): processIq is non-blocking. true: wait for each output
        // block (deterministic for a burst/offline feed).
        bool blockForOutput = false;
    };

    // (Re)build the WdspChannel + ColibriSpectrum for this config. Returns
    // false (and sets error, if given) when the WDSP channel cannot be created.
    Q_INVOKABLE bool configure(const Config& config, std::string* error = nullptr);
    Q_INVOKABLE void setMode(WdspChannel::Mode mode);
    Q_INVOKABLE void setFilter(double lowHz, double highHz);
    Q_INVOKABLE void setAgc(int agcMode, double maximumGainDb);
    // RX frequency shift in Hz relative to the NCO — how the backend tunes the
    // slice inside the passband without moving the DDC.
    Q_INVOKABLE void setShift(double shiftHz);
    // Cap how often a panadapter frame is produced, in frames per second.
    // The FFT is SKIPPED entirely when a frame is not due — at 3.072 MHz the
    // natural rate would be 3000 fps, so limiting at the source is what makes
    // a wide span affordable. fps <= 0 removes the cap.
    Q_INVOKABLE void setSpectrumRateFps(int fps);

    [[nodiscard]] bool isConfigured() const noexcept { return m_channel != nullptr; }

public slots:
    // Feed one IQ block (normalized complex<float>, wire order). Emits
    // spectrumReady per FFT frame and audioReady/meterUpdate per completed
    // WdspChannel block.
    void processIqBlock(const std::vector<std::complex<float>>& iq);

signals:
    void audioReady(const std::vector<float>& stereoPcm);   // interleaved L,R
    void spectrumReady(const std::vector<float>& binsDbfs); // DC-centred dBFS
    void meterUpdate(float dbfs);                           // WDSP signal meter

private:
    // True when the next panadapter frame may be computed. Stays true until
    // one actually completes (a frame spans several callback blocks).
    bool spectrumFrameDue();

    std::unique_ptr<WdspChannel> m_channel;
    std::unique_ptr<ColibriSpectrum> m_spectrum;
    double m_shiftHz = 0.0;   // current slice offset from the NCO, Hz
    Config m_config;

    // Panadapter frame-rate cap. 0 = uncapped. m_spectrumClock is started on
    // the first block and only read/written on the DSP thread.
    int m_spectrumIntervalMs = 0;
    QElapsedTimer m_spectrumClock;
    qint64 m_lastSpectrumMs = 0;
    std::vector<std::complex<float>> m_iqBuffer;   // IQ awaiting a full DSP block
    // The conjugated copy of the block, for whichever consumer needs the
    // opposite handedness to the wire. A member rather than a local: this runs
    // per IQ block on the I/O thread.
    std::vector<std::complex<float>> m_conjugated;
    std::vector<float> m_i, m_q;                    // deinterleaved input scratch
    std::vector<float> m_left, m_right;             // WdspChannel output scratch
    std::vector<float> m_stereo;                    // interleaved audio out
    std::vector<float> m_bins;                      // spectrum scratch
};

}  // namespace AetherSDR::colibri
