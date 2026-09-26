#pragma once
#include <array>
#include <span>

#include "core/backends/rtl/RtlCaptureTransaction.h"
#include "core/backends/rtl/RtlViewport.h"
#include "core/dsp/SpectrumTemporalAverage.h"

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
// 1. Computes a continuous 65536-point display FFT with FFTW float32.
// 2. Performs NCO frequency shifting, decimation, and FM/AM/SSB demodulation for AudioEngine
class RtlSdrDdc : public QObject {
    Q_OBJECT

public:
    static constexpr int kSpectrumBinCount = RtlViewport::kRtlSpectrumBins;
    explicit RtlSdrDdc(QObject* parent = nullptr);
    ~RtlSdrDdc() override;

    // Parameter configuration (thread-safe setters called from main thread)
    // Acquisition-context only, at a block boundary. Prepared numeric values;
    // no parsing, planning, allocation or destruction here.
    void applyCapture(double rateHz, double centerHz, double sliceHz,
                      RtlCaptureTransaction::Mode mode, int lowHz, int highHz,
                      double usableLeftHz = -1, double usableRightHz = -1);
    // Acquisition-context only. Retire partial legacy PCM when a receiver
    // parks, resumes or changes its accepted filter without moving hardware.
    void resetReceiveAudio() noexcept;
    // Acquisition only: discard partial spectra across observable IQ gaps or
    // incompatible acquisition changes, including rollback to the same RF.
    void resetSpectrum() noexcept;
    void setSampleRate(double sampleRateHz);
    void setCenterFrequency(double centerHz);
    void setSliceFrequency(double sliceHz);
    void setSliceMode(const QString& mode);
    void setSliceFilter(int lowHz, int highHz);
    void setSpectrumRateFps(int fps);
    void setSpectrumAverage(int average) { m_spectrumAverage.store(std::clamp(average, 0, 100)); }
    void setSpectrumWeightedAverage(bool on) { m_spectrumWeightedAverage.store(on); }
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
    void processIqData(const QVector<std::complex<float>>& samples, bool audio = true, bool measureSquelch = true);
    void processSquelchSpectrum(const QVector<std::complex<float>>& samples);

signals:
    // Emits raw spectrum FFT magnitude data for PanadapterWidget (~30 FPS)
    void spectrumFrameReady(int panId, const QByteArray& frame);

    // Emits raw waterfall row FFT magnitude data for WaterfallWidget (~30 FPS)
    void waterfallRowReady(int panId, const QByteArray& row);

    // Emits 24 kHz float32 PCM audio data for AudioEngine
    void audioFrameReady(const QByteArray& pcm, const QByteArray& preMonitor);

private:
    using DemodMode = RtlCaptureTransaction::Mode;

    void processDisplaySpectrum(std::span<const std::complex<float>> samples);
    void restartSpectrumWindow() noexcept;
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
    std::atomic<int> m_spectrumAverage{0};
    std::atomic<bool> m_spectrumWeightedAverage{false};
    size_t m_detectorCounter = 0;
    bool m_firstDetectorEmitted = false;
    bool m_squelchSpectrumFresh = false;
    std::array<float, 2048> m_spectrumBins{};
    std::atomic<bool> m_audioMuted{false};
    std::atomic<float> m_audioGain{1.0f};
    std::atomic<int> m_audioPanPercent{50};

    // FFT state & rate limiter
    static constexpr size_t kFftSize = 2048; // Established squelch measurement only.
    std::vector<float> m_fftWindow;
    fftwf_complex* m_fftIn{nullptr};
    fftwf_complex* m_fftOut{nullptr};
    fftwf_plan m_fftPlan{nullptr};
    std::atomic<size_t> m_spectrumSampleStride{80'000};  // 2.4 MSPS / 30 FPS = 80,000 samples
    // Display history is always full capture IQ. A viewport change only crops
    // its genuine bins downstream, without altering receiver samples or rate.
    std::vector<std::complex<float>> m_displayHistory;
    std::vector<float> m_displayWindow;
    std::vector<float> m_displayBins;
    fftwf_complex* m_displayIn{nullptr};
    fftwf_complex* m_displayOut{nullptr};
    fftwf_plan m_displayPlan{nullptr};
    size_t m_displayWrite = 0;
    size_t m_displayFilled = 0;
    size_t m_displayUntilFrame = kSpectrumBinCount;
    size_t m_displayStride = 80'000;
    double m_displayRateHz = 2'400'000;
    size_t m_displaySamplesSinceFrame = 0;
    SpectrumTemporalAverage m_displayAverage{kSpectrumBinCount};
    std::size_t m_displayFirstUsable = 0;
    std::size_t m_displayEndUsable = kSpectrumBinCount;
    bool m_displayTransition = true;

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
