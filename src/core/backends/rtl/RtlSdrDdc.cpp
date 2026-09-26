#include "core/backends/rtl/RtlSdrDdc.h"
#include "core/dsp/FftwPlannerLock.h"

#include <cmath>
#include <numbers>
#include <algorithm>
#include <QDebug>

namespace AetherSDR::rtl {

RtlSdrDdc::RtlSdrDdc(QObject* parent)
    : QObject(parent)
{
    m_displayHistory.resize(kSpectrumBinCount);
    m_displayWindow.resize(kSpectrumBinCount);
    m_displayBins.resize(kSpectrumBinCount);

    // Initialize Blackman-Harris window for FFT spectrum
    m_fftWindow.resize(kFftSize);
    for (size_t i = 0; i < kFftSize; ++i) {
        const double n = static_cast<double>(i);
        const double N = static_cast<double>(kFftSize);
        m_fftWindow[i] = static_cast<float>(0.35875 - 0.48829 * std::cos(2.0 * std::numbers::pi * n / (N - 1.0))
                                                    + 0.14128 * std::cos(4.0 * std::numbers::pi * n / (N - 1.0))
                                                    - 0.01168 * std::cos(6.0 * std::numbers::pi * n / (N - 1.0)));
    }

    for (size_t i = 0; i < kSpectrumBinCount; ++i) {
        const double angle = 2.0 * std::numbers::pi * i / (kSpectrumBinCount - 1.0);
        m_displayWindow[i] = static_cast<float>(0.35875 - 0.48829 * std::cos(angle)
            + 0.14128 * std::cos(2.0 * angle) - 0.01168 * std::cos(3.0 * angle));
    }

    // The single-precision planner and its allocator are shared with NR4.
    {
        auto lock = fftwfPlannerLock();
        m_fftIn  = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * kFftSize));
        m_fftOut = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * kFftSize));
        m_displayIn = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * kSpectrumBinCount));
        m_displayOut = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * kSpectrumBinCount));
        if (m_displayIn && m_displayOut) {
            m_displayPlan = fftwf_plan_dft_1d(kSpectrumBinCount, m_displayIn, m_displayOut,
                                             FFTW_FORWARD, FFTW_ESTIMATE);
        }
        if (m_fftIn && m_fftOut) {
            m_fftPlan = fftwf_plan_dft_1d(static_cast<int>(kFftSize), m_fftIn, m_fftOut, FFTW_FORWARD, FFTW_ESTIMATE);
        }
    }
}

RtlSdrDdc::~RtlSdrDdc()
{
    auto lock = fftwfPlannerLock();
    if (m_displayPlan) { fftwf_destroy_plan(m_displayPlan); }
    if (m_displayIn) { fftwf_free(m_displayIn); }
    if (m_displayOut) { fftwf_free(m_displayOut); }
    if (m_fftPlan) {
        fftwf_destroy_plan(m_fftPlan);
        m_fftPlan = nullptr;
    }
    if (m_fftIn) {
        fftwf_free(m_fftIn);
        m_fftIn = nullptr;
    }
    if (m_fftOut) {
        fftwf_free(m_fftOut);
        m_fftOut = nullptr;
    }
}

void RtlSdrDdc::applyCapture(double rateHz, double centerHz, double sliceHz,
                              RtlCaptureTransaction::Mode mode, int lowHz, int highHz)
{
    if (m_sampleRateHz.load() != rateHz || m_centerHz.load() != centerHz
        || m_sliceHz.load() != sliceHz || m_mode.load() != mode) {
        resetReceiveAudio();
        resetSpectrum();
    }
    setSampleRate(rateHz);
    setCenterFrequency(centerHz);
    setSliceFrequency(sliceHz);
    m_mode.store(mode, std::memory_order_relaxed);
    setSliceFilter(lowHz, highHz);
}

void RtlSdrDdc::resetSpectrum() noexcept
{
    m_displayWrite = 0;
    m_displayFilled = 0;
    m_displayUntilFrame = kSpectrumBinCount;
    m_displaySamplesSinceFrame = 0;
    m_displayAverage.reset();
    m_detectorCounter = 0;
    m_firstDetectorEmitted = false;
    m_squelchSpectrumFresh = false;
}

void RtlSdrDdc::resetReceiveAudio() noexcept
{
    m_ncoPhase = 0;
    m_ncoPhasor = {1, 0};
    m_ncoNormalizeCounter = 0;
    m_decimAcc = {};
    m_decimCount = 0;
    m_prevDecimIq = {};
    m_deemphState = 0;
    m_audioDecimAcc = 0;
    m_audioDecimCounter = 0;
    m_audioResamplePhase = 0;
    // The accumulator is private after append. Truncation retains capacity
    // and does not allocate in the USB callback.
    m_audioBuffer.truncate(0);
    m_tapBuffer.truncate(0);
    m_firstAudioEmitted = false;
}

void RtlSdrDdc::setSampleRate(double sampleRateHz)
{
    if (std::isfinite(sampleRateHz) && sampleRateHz >= 1 && sampleRateHz <= 3'000'000) {
        m_sampleRateHz.store(sampleRateHz, std::memory_order_relaxed);
        const int spectrumFps = m_spectrumFps.load(std::memory_order_relaxed);
        m_spectrumSampleStride.store(
            std::max<size_t>(1, static_cast<size_t>(sampleRateHz / spectrumFps)),
            std::memory_order_relaxed);
    }
}

void RtlSdrDdc::setCenterFrequency(double centerHz)
{
    m_centerHz.store(centerHz, std::memory_order_relaxed);
}

void RtlSdrDdc::setSliceFrequency(double sliceHz)
{
    m_sliceHz.store(sliceHz, std::memory_order_relaxed);
}

void RtlSdrDdc::setSliceMode(const QString& mode)
{
    const QString canonical = mode.trimmed().toUpper();
    DemodMode demodMode = DemodMode::Usb;
    if (canonical == QLatin1String("AM")) {
        demodMode = DemodMode::Am;
    } else if (canonical == QLatin1String("SAM")) {
        demodMode = DemodMode::Sam;
    } else if (canonical == QLatin1String("FM")) {
        demodMode = DemodMode::Fm;
    } else if (canonical == QLatin1String("FMN")) {
        demodMode = DemodMode::Fmn;
    } else if (canonical == QLatin1String("WFM")) {
        demodMode = DemodMode::Wfm;
    } else if (canonical == QLatin1String("LSB")) {
        demodMode = DemodMode::Lsb;
    } else if (canonical == QLatin1String("CW")) {
        demodMode = DemodMode::Cw;
    } else if (canonical == QLatin1String("CWR")) {
        demodMode = DemodMode::Cwr;
    }
    m_mode.store(demodMode, std::memory_order_relaxed);
}

void RtlSdrDdc::setSliceFilter(int lowHz, int highHz)
{
    m_filterLowHz.store(lowHz, std::memory_order_relaxed);
    m_filterHighHz.store(highHz, std::memory_order_relaxed);
}

void RtlSdrDdc::setSpectrumRateFps(int fps)
{
    const int clampedFps = std::clamp(fps, 1, 60);
    m_spectrumFps.store(clampedFps, std::memory_order_relaxed);
    m_spectrumSampleStride.store(
        std::max<size_t>(1, static_cast<size_t>(
            m_sampleRateHz.load(std::memory_order_relaxed) / clampedFps)),
        std::memory_order_relaxed);
}

void RtlSdrDdc::setAudioMute(bool mute)
{
    m_audioMuted.store(mute, std::memory_order_relaxed);
}

void RtlSdrDdc::setAudioGain(int gainPercent)
{
    m_audioGain.store(std::clamp(gainPercent, 0, 100) / 100.0f,
                      std::memory_order_relaxed);
}

void RtlSdrDdc::setAudioPan(int panPercent)
{
    m_audioPanPercent.store(std::clamp(panPercent, 0, 100),
                            std::memory_order_relaxed);
}

void RtlSdrDdc::applyMonitor(int gain, int pan, bool mute)
{
    if (m_audioGain.load() != std::clamp(gain, 0, 100) / 100.0f
        || m_audioPanPercent.load() != std::clamp(pan, 0, 100) || m_audioMuted.load() != mute) {
        // Buffered speaker samples were rendered with the old monitor state.
        // Keep demodulator history, but never publish that batch under the new
        // acknowledged controls. Both taps retain the same chunk boundary.
        m_audioBuffer.truncate(0); m_tapBuffer.truncate(0); m_firstAudioEmitted = false;
    }
    setAudioGain(gain); setAudioPan(pan); setAudioMute(mute);
}

void RtlSdrDdc::processIqData(const QVector<std::complex<float>>& samples, bool audio, bool measureSquelch)
{
    if (samples.isEmpty()) {
        return;
    }

    processDisplaySpectrum(std::span(samples.constData(), samples.size()));
    if (measureSquelch) { processSquelchSpectrum(samples); }
    if (audio) { processAudio(samples); }
}

void RtlSdrDdc::processSquelchSpectrum(const QVector<std::complex<float>>& samples)
{
    if (!m_fftPlan || !m_fftIn || !m_fftOut) {
        return;
    }

    m_detectorCounter += samples.size();
    const bool detectorDue = !m_firstDetectorEmitted
        || m_detectorCounter >= m_sampleRateHz.load(std::memory_order_relaxed) / 30.0;
    if (!detectorDue) { return; }

    const size_t numToCopy = std::min(static_cast<size_t>(samples.size()), kFftSize);
    for (size_t i = 0; i < numToCopy; ++i) {
        const float w = m_fftWindow[i];
        m_fftIn[i][0] = samples[i].real() * w;
        m_fftIn[i][1] = samples[i].imag() * w;
    }
    for (size_t i = numToCopy; i < kFftSize; ++i) {
        m_fftIn[i][0] = 0.0f;
        m_fftIn[i][1] = 0.0f;
    }

    // Run FFTW 1D forward transform
    fftwf_execute(m_fftPlan);

    for (size_t k = 0; k < kFftSize; ++k) {
        const float re = m_fftOut[k][0];
        const float im = m_fftOut[k][1];
        const float mag = std::sqrt(re * re + im * im) / static_cast<float>(kFftSize);
        const float db = 20.0f * std::log10(std::max(mag, 1e-6f));
        // Shift zero-frequency component to center
        size_t outIdx = (k + kFftSize / 2) % kFftSize;
        m_spectrumBins[outIdx] = db;
    }

    if (detectorDue) {
        m_detectorCounter = 0; m_firstDetectorEmitted = true;
        m_squelchSpectrumFresh = true;
    }
}

void RtlSdrDdc::processDisplaySpectrum(std::span<const std::complex<float>> samples)
{
    if (!m_displayPlan || !m_displayIn || !m_displayOut) { return; }
    const double rate = m_sampleRateHz.load(std::memory_order_relaxed);
    if (rate != m_displayRateHz) {
        resetSpectrum();
        m_displayRateHz = rate;
    }
    const size_t stride = m_spectrumSampleStride.load(std::memory_order_relaxed);
    if (stride != m_displayStride) {
        if (m_displayFilled == kSpectrumBinCount) {
            m_displayUntilFrame = std::min(m_displayUntilFrame, stride);
        }
        m_displayStride = stride;
    }
    while (!samples.empty()) {
        // Stop at the exact sample deadline, even when USB partitions straddle
        // it. The circular history keeps the last complete contiguous window.
        const size_t count = std::min({samples.size(), m_displayUntilFrame,
                                      size_t(kSpectrumBinCount) - m_displayWrite});
        std::copy_n(samples.data(), count, m_displayHistory.data() + m_displayWrite);
        samples = samples.subspan(count);
        m_displayWrite = (m_displayWrite + count) % kSpectrumBinCount;
        m_displayFilled = std::min(size_t(kSpectrumBinCount), m_displayFilled + count);
        m_displayUntilFrame -= count;
        m_displaySamplesSinceFrame += count;
        if (m_displayUntilFrame != 0) { continue; }
        // The first deadline is a whole observation; later deadlines may
        // overlap it. Short blocks are accumulated, never zero-padded.
        for (size_t i = 0; i < kSpectrumBinCount; ++i) {
            const auto sample = m_displayHistory[(m_displayWrite + i) % kSpectrumBinCount];
            m_displayIn[i][0] = sample.real() * m_displayWindow[i];
            m_displayIn[i][1] = sample.imag() * m_displayWindow[i];
        }
        fftwf_execute(m_displayPlan);
        const int average = m_spectrumAverage.load(std::memory_order_relaxed);
        const bool weighted = m_spectrumWeightedAverage.load(std::memory_order_relaxed);
        m_displayAverage.beginFrame(average, weighted, m_displaySamplesSinceFrame / rate);
        for (size_t i = 0; i < kSpectrumBinCount; ++i) {
            if (average > 0 && !weighted) {
                // Accumulate before the log, with no power -> dB -> power trip.
                const float re = m_displayOut[i][0] / kSpectrumBinCount;
                const float im = m_displayOut[i][1] / kSpectrumBinCount;
                m_displayBins[(i + kSpectrumBinCount / 2) % kSpectrumBinCount] =
                    m_displayAverage.bin(i, re * re + im * im, 0);
                continue;
            }
            const float magnitude = std::hypot(m_displayOut[i][0], m_displayOut[i][1])
                / kSpectrumBinCount;
            const float db = 20.0f * std::log10(std::max(magnitude, 1e-6f));
            m_displayBins[(i + kSpectrumBinCount / 2) % kSpectrumBinCount] =
                m_displayAverage.bin(i, magnitude * magnitude, db);
        }
        m_displaySamplesSinceFrame = 0;
        m_displayUntilFrame = m_displayStride;
        const QByteArray frame(reinterpret_cast<const char*>(m_displayBins.data()),
                               kSpectrumBinCount * int(sizeof(float)));
        emit spectrumFrameReady(0, frame);
        emit waterfallRowReady(0, frame);
    }
}

void RtlSdrDdc::processAudio(const QVector<std::complex<float>>& samples)
{
    double sampleRateHz = 2'400'000.0;
    double centerHz = 95'200'000.0;
    double sliceHz = 95'200'000.0;
    DemodMode mode = DemodMode::Wfm;
    bool audioMuted = false;
    float audioGain = 1.0f;
    int audioPanPercent = 50;

    sampleRateHz = m_sampleRateHz.load(std::memory_order_relaxed);
    centerHz = m_centerHz.load(std::memory_order_relaxed);
    sliceHz = m_sliceHz.load(std::memory_order_relaxed);
    mode = m_mode.load(std::memory_order_relaxed);
    audioMuted = m_audioMuted.load(std::memory_order_relaxed);
    audioGain = m_audioGain.load(std::memory_order_relaxed);
    audioPanPercent = m_audioPanPercent.load(std::memory_order_relaxed);

    const double ncoStep = 2.0 * std::numbers::pi * (sliceHz - centerHz) / sampleRateHz;
    const std::complex<float> ncoStepPhasor(
        static_cast<float>(std::cos(-ncoStep)),
        static_cast<float>(std::sin(-ncoStep))
    );

    QByteArray pcm;
    QByteArray tap;
    tap.reserve(static_cast<int>((samples.size() / 100 + 1) * sizeof(float) * 2));
    pcm.reserve(static_cast<int>((samples.size() / 100 + 1) * sizeof(float) * 2));

    // Target ~240 kSPS intermediate IQ sample rate for Stage 1 decimation
    const int stage1Decim = std::max(1, static_cast<int>(std::round(sampleRateHz / 240'000.0)));
    const double stage1Fs = sampleRateHz / static_cast<double>(stage1Decim);
    const float deemphAlpha = 1.0f - std::exp(-1.0f / (75e-6f * static_cast<float>(stage1Fs)));

    for (const auto& sample : samples) {
        // NCO phasor shift (shifts target slice frequency to DC)
        m_ncoPhasor *= ncoStepPhasor;
        const std::complex<float> shifted = sample * m_ncoPhasor;

        if (++m_ncoNormalizeCounter >= 1000) {
            m_ncoNormalizeCounter = 0;
            const float mag = std::abs(m_ncoPhasor);
            if (mag > 0.0f) {
                m_ncoPhasor /= mag;
            }
        }

        // Anti-aliasing boxcar accumulator
        m_decimAcc += shifted;
        ++m_decimCount;

        if (m_decimCount >= stage1Decim) {
            const std::complex<float> decimIq = m_decimAcc / static_cast<float>(m_decimCount);
            m_decimAcc = {0.0f, 0.0f};
            m_decimCount = 0;

            float audioLeft = 0.0f;

            if (mode == DemodMode::Fm || mode == DemodMode::Fmn
                || mode == DemodMode::Wfm) {
                // Demodulate FM: phase difference angle(Z_n * conj(Z_n-1))
                const std::complex<float> prod = decimIq * std::conj(m_prevDecimIq);
                m_prevDecimIq = decimIq;

                float dphi = std::atan2(prod.imag(), prod.real());
                if (mode == DemodMode::Wfm) {
                    // WFM (±75 kHz deviation)
                    audioLeft = dphi / std::numbers::pi_v<float>;
                    // 75µs de-emphasis lowpass filter
                    m_deemphState += deemphAlpha * (audioLeft - m_deemphState);
                    audioLeft = m_deemphState;
                } else {
                    // NFM (±5 kHz deviation)
                    audioLeft = (dphi / std::numbers::pi_v<float>) * 4.0f;
                }
            } else if (mode == DemodMode::Am || mode == DemodMode::Sam) {
                audioLeft = (std::abs(decimIq) - 0.3f) * 1.5f;
                m_prevDecimIq = decimIq;
            } else if (mode == DemodMode::Lsb) {
                audioLeft = (decimIq.real() - decimIq.imag()) * 1.5f;
                m_prevDecimIq = decimIq;
            } else if (mode == DemodMode::Cw || mode == DemodMode::Cwr) {
                audioLeft = decimIq.real() * 1.5f;
                m_prevDecimIq = decimIq;
            } else {
                // USB
                audioLeft = (decimIq.real() + decimIq.imag()) * 1.5f;
                m_prevDecimIq = decimIq;
            }

            // Stage 2: fractional boxcar resampling to exactly 24 kSPS on
            // average. A rounded integer divisor drifts for rates such as
            // 225001 and 1.8432 MSPS and eventually starves/overruns audio.
            ++m_audioDecimCounter;
            m_audioDecimAcc += audioLeft;
            m_audioResamplePhase += 24'000.0;
            if (m_audioResamplePhase >= stage1Fs) {
                m_audioResamplePhase -= stage1Fs;
                float finalAudio = m_audioDecimAcc / static_cast<float>(m_audioDecimCounter);
                m_audioDecimAcc = 0.0f;
                m_audioDecimCounter = 0;

                // Preserve the existing demodulator and its unmodified tap.
                // Monitor mute must keep clocking its history and consumers.
                tap.append(reinterpret_cast<const char*>(&finalAudio), sizeof(float));
                tap.append(reinterpret_cast<const char*>(&finalAudio), sizeof(float));
                finalAudio = audioMuted ? 0.0f : std::clamp(finalAudio * audioGain, -1.0f, 1.0f);
                const float pan = audioPanPercent / 100.0f;
                float audioLeftOut = finalAudio * std::min(1.0f, 2.0f * (1.0f - pan));
                float audioRight = finalAudio * std::min(1.0f, 2.0f * pan);

                pcm.append(reinterpret_cast<const char*>(&audioLeftOut), sizeof(float));
                pcm.append(reinterpret_cast<const char*>(&audioRight), sizeof(float));
            }
        }
    }

    if (!pcm.isEmpty()) {
        m_audioBuffer.append(pcm);
        m_tapBuffer.append(tap);
        // Batch audio dispatches to ~50 ms chunks (9600 bytes = 1200 stereo float samples @ 24kHz).
        if (!m_firstAudioEmitted || m_audioBuffer.size() >= 9600) {
            m_firstAudioEmitted = true;
            emit audioFrameReady(m_audioBuffer, m_tapBuffer);
            m_audioBuffer.clear();
            m_tapBuffer.clear();
        }
    }
}

}  // namespace AetherSDR::rtl
