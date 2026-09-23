#pragma once
#include <array>
#include <span>

#include "core/backends/rtl/RtlCaptureTransaction.h"

#include <QObject>
#include <QByteArray>
#include <QVector>
#include <QString>
#include <complex>
#include <vector>
#include <atomic>
#include <fftw3.h>

namespace AetherSDR::rtl {

// Digital Down-Converter (DDC) and Demodulation engine for RtlSdrBackend.
// Executes on the RtlSdrWorker thread:
// 1. Computes 2048-point FFT magnitude spectrum at ~30 FPS using FFTW float32
// 2. Performs NCO frequency shifting, decimation, and FM/AM/SSB demodulation for AudioEngine
class RtlSdrDdc : public QObject {
    Q_OBJECT

public:
    static constexpr int kSpectrumBinCount = 2048;
    explicit RtlSdrDdc(QObject* parent = nullptr);
    ~RtlSdrDdc() override;

    // Parameter configuration (thread-safe setters called from main thread)
    // Acquisition-context only, at a block boundary. Prepared numeric values;
    // no parsing, planning, allocation or destruction here.
    void applyCapture(double rateHz, double centerHz, double sliceHz,
                      RtlCaptureTransaction::Mode mode, int lowHz, int highHz);
    void setSampleRate(double sampleRateHz);
    void setCenterFrequency(double centerHz);
    void setSliceFrequency(double sliceHz);
    void setSliceMode(const QString& mode);
    void setSliceFilter(int lowHz, int highHz);
    void setSpectrumRateFps(int fps);
    void setAudioMute(bool mute);
    void applyMonitor(int gain, int pan, bool mute); // acquisition boundary only
    void setAudioGain(int gainPercent);
    void setAudioPan(int panPercent);
    // Acquisition-only borrowed view, consumed before the next IQ block.
    // Detector cadence is independent of display throttling and allocates no
    // frame. Empty means the previous measurement has already been consumed.
    std::span<const float> takeSquelchSpectrum() noexcept
    {
        if (!m_squelchSpectrumFresh) { return {}; }
        m_squelchSpectrumFresh = false;
        return m_spectrumBins;
    }

public slots:
    // Process incoming complex float IQ samples (runs on worker thread)
    void processIqData(const QVector<std::complex<float>>& samples, bool audio = true);

signals:
    // Emits raw spectrum FFT magnitude data for PanadapterWidget (~30 FPS)
    void spectrumFrameReady(int panId, const QByteArray& frame);

    // Emits raw waterfall row FFT magnitude data for WaterfallWidget (~30 FPS)
    void waterfallRowReady(int panId, const QByteArray& row);

    // Emits 24 kHz float32 PCM audio data for AudioEngine
    void audioFrameReady(const QByteArray& pcm, const QByteArray& preMonitor);

private:
    using DemodMode = RtlCaptureTransaction::Mode;

    void processSpectrum(const QVector<std::complex<float>>& samples);
    void processAudio(const QVector<std::complex<float>>& samples);

    // Written by the main thread and sampled inside the USB callback. These
    // must stay lock-free at the DSP boundary; the audio callback never waits
    // on a GUI-thread mutex.
    std::atomic<double> m_sampleRateHz{2'400'000.0};
    std::atomic<double> m_centerHz{95'200'000.0};
    std::atomic<double> m_sliceHz{95'200'000.0};
    std::atomic<DemodMode> m_mode{DemodMode::Wfm};
    std::atomic<int> m_filterLowHz{-100000};
    std::atomic<int> m_filterHighHz{100000};
    std::atomic<int> m_spectrumFps{30};
    size_t m_detectorCounter = 0;
    bool m_firstDetectorEmitted = false;
    bool m_squelchSpectrumFresh = false;
    std::array<float, 2048> m_spectrumBins{};
    std::atomic<bool> m_audioMuted{false};
    std::atomic<float> m_audioGain{1.0f};
    std::atomic<int> m_audioPanPercent{50};

    // FFT state & rate limiter
    static constexpr size_t kFftSize = kSpectrumBinCount;
    std::vector<std::complex<float>> m_fftAccumulator;
    std::vector<float> m_fftWindow;
    fftwf_complex* m_fftIn{nullptr};
    fftwf_complex* m_fftOut{nullptr};
    fftwf_plan m_fftPlan{nullptr};
    std::atomic<size_t> m_spectrumSampleStride{80'000};  // 2.4 MSPS / 30 FPS = 80,000 samples
    size_t m_spectrumCounter{0};
    bool m_firstSpectrumEmitted{false};

    // NCO & Decimation state
    double m_ncoPhase{0.0};
    std::complex<float> m_ncoPhasor{1.0f, 0.0f};
    uint32_t m_ncoNormalizeCounter{0};
    std::complex<float> m_decimAcc{0.0f, 0.0f};
    int m_decimCount{0};
    std::complex<float> m_prevDecimIq{0.0f, 0.0f};
    float m_deemphState{0.0f};
    float m_audioDecimAcc{0.0f};
    int m_audioDecimCounter{0};
    double m_audioResamplePhase{0.0};
    QByteArray m_audioBuffer;
    QByteArray m_tapBuffer;
    bool m_firstAudioEmitted{false};
};

}  // namespace AetherSDR::rtl
