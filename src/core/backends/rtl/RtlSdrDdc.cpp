#include "core/backends/rtl/RtlSdrDdc.h"

#include <cmath>
#include <numbers>
#include <algorithm>
#include <QDebug>

namespace AetherSDR::rtl {

RtlSdrDdc::RtlSdrDdc(QObject* parent)
    : QObject(parent)
{
    m_fftAccumulator.reserve(kFftSize);

    // Initialize Blackman-Harris window for FFT spectrum
    m_fftWindow.resize(kFftSize);
    for (size_t i = 0; i < kFftSize; ++i) {
        const double n = static_cast<double>(i);
        const double N = static_cast<double>(kFftSize);
        m_fftWindow[i] = static_cast<float>(0.35875 - 0.48829 * std::cos(2.0 * std::numbers::pi * n / (N - 1.0))
                                                    + 0.14128 * std::cos(4.0 * std::numbers::pi * n / (N - 1.0))
                                                    - 0.01168 * std::cos(6.0 * std::numbers::pi * n / (N - 1.0)));
    }

    // Allocate FFTW buffers and plan
    m_fftIn  = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * kFftSize));
    m_fftOut = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * kFftSize));
    if (m_fftIn && m_fftOut) {
        m_fftPlan = fftwf_plan_dft_1d(static_cast<int>(kFftSize), m_fftIn, m_fftOut, FFTW_FORWARD, FFTW_ESTIMATE);
    }
}

RtlSdrDdc::~RtlSdrDdc()
{
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

void RtlSdrDdc::setSampleRate(double sampleRateHz)
{
    if (sampleRateHz > 0) {
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

void RtlSdrDdc::processIqData(const QVector<std::complex<float>>& samples)
{
    if (samples.isEmpty()) {
        return;
    }

    processSpectrum(samples);
    processAudio(samples);
}

void RtlSdrDdc::processSpectrum(const QVector<std::complex<float>>& samples)
{
    if (!m_fftPlan || !m_fftIn || !m_fftOut) {
        return;
    }

    m_spectrumCounter += samples.size();
    if (m_spectrumCounter < m_spectrumSampleStride.load(std::memory_order_relaxed)
        && m_firstSpectrumEmitted) {
        return;
    }

    m_spectrumCounter = 0;
    m_firstSpectrumEmitted = true;

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

    QByteArray frame;
    frame.resize(static_cast<int>(kFftSize * sizeof(float)));
    float* magOut = reinterpret_cast<float*>(frame.data());

    for (size_t k = 0; k < kFftSize; ++k) {
        const float re = m_fftOut[k][0];
        const float im = m_fftOut[k][1];
        const float mag = std::sqrt(re * re + im * im) / static_cast<float>(kFftSize);
        const float db = 20.0f * std::log10(std::max(mag, 1e-6f));
        // Shift zero-frequency component to center
        size_t outIdx = (k + kFftSize / 2) % kFftSize;
        magOut[outIdx] = db;
    }

    emit spectrumFrameReady(0, frame);
    emit waterfallRowReady(0, frame);
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

    if (audioMuted) {
        return;
    }

    const double ncoStep = 2.0 * std::numbers::pi * (sliceHz - centerHz) / sampleRateHz;
    const std::complex<float> ncoStepPhasor(
        static_cast<float>(std::cos(-ncoStep)),
        static_cast<float>(std::sin(-ncoStep))
    );

    QByteArray pcm;
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

                finalAudio = std::clamp(finalAudio * audioGain, -1.0f, 1.0f);
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
        // Batch audio dispatches to ~50 ms chunks (9600 bytes = 1200 stereo float samples @ 24kHz).
        if (!m_firstAudioEmitted || m_audioBuffer.size() >= 9600) {
            m_firstAudioEmitted = true;
            emit audioFrameReady(m_audioBuffer);
            m_audioBuffer.clear();
        }
    }
}

}  // namespace AetherSDR::rtl
