#ifdef __APPLE__

#include "MacNRFilter.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace AetherSDR {

// ── Constructor / destructor ────────────────────────────────────────────────

MacNRFilter::MacNRFilter(int sampleRate)
    : m_sampleRate(sampleRate)
    , m_log2n(sampleRate == 48000 ? 10 : 9)
    , m_fftSize(1 << m_log2n)
    , m_hopSize(m_fftSize / 2)
    , m_bins(m_hopSize + 1)
    , m_powerHistory(HIST * m_bins, 0.0f)
{
    if (sampleRate != 24000 && sampleRate != 48000) {
        return;
    }
    m_fftSetup = vDSP_create_fftsetup(m_log2n, kFFTRadix2);
    if (!m_fftSetup)
        return;

    // vDSP split-complex scratch buffers (size m_hopSize = m_fftSize/2)
    m_splitRe.assign(m_hopSize, 0.0f);
    m_splitIm.assign(m_hopSize, 0.0f);

    // sqrt-Hann window (analysis × synthesis = Hann; perfect reconstruction
    // with 50 % overlap when frames are hop-sized)
    m_window.resize(m_fftSize);
    for (int i = 0; i < m_fftSize; ++i)
        m_window[i] = std::sqrt(0.5f * (1.0f - std::cos(2.0f * M_PI * i / m_fftSize)));

    // Pre-fill input accumulator with one full frame of zeros so that the
    // first call to process() sees a centred first frame immediately and
    // the existing one-frame delay (≈21.3 ms) stays constant at both rates.
    m_inAccum.assign(m_fftSize, 0.0f);
    m_inAccumL.assign(m_fftSize, 0.0f);
    m_inAccumR.assign(m_fftSize, 0.0f);

    // OLA and scratch buffers
    m_olaBufferL.assign(m_fftSize, 0.0f);
    m_olaBufferR.assign(m_fftSize, 0.0f);
    m_frameBuf .assign(m_fftSize, 0.0f);
    m_synthBuf .assign(m_fftSize, 0.0f);
    m_outAccumL.clear();
    m_outAccumR.clear();

    // Noise estimator
    m_noiseEst      .assign(m_bins, 0.0f);
    m_smoothedPower .assign(m_bins, 0.0f);
    m_prevPostSnr   .assign(m_bins, 1.0f);
    m_filterGain    .assign(m_bins, 1.0f);
    m_powerBuf      .assign(m_bins, 0.0f);
    m_gainBuf       .assign(m_bins, 1.0f);
}

MacNRFilter::~MacNRFilter()
{
    if (m_fftSetup)
        vDSP_destroy_fftsetup(m_fftSetup);
}

// ── reset ───────────────────────────────────────────────────────────────────

void MacNRFilter::reset()
{
    // Clear time-domain accumulators
    std::fill(m_inAccum .begin(), m_inAccum .end(), 0.0f);
    std::fill(m_inAccumL.begin(), m_inAccumL.end(), 0.0f);
    std::fill(m_inAccumR.begin(), m_inAccumR.end(), 0.0f);
    std::fill(m_olaBufferL.begin(), m_olaBufferL.end(), 0.0f);
    std::fill(m_olaBufferR.begin(), m_olaBufferR.end(), 0.0f);
    m_outAccumL.clear();
    m_outAccumR.clear();

    // Re-prime the input accumulator (same delay as the constructor).
    m_inAccum.assign(m_fftSize, 0.0f);
    m_inAccumL.assign(m_fftSize, 0.0f);
    m_inAccumR.assign(m_fftSize, 0.0f);

    // Reset noise estimator
    std::fill(m_noiseEst.begin(), m_noiseEst.end(), 0.0f);
    std::fill(m_smoothedPower.begin(), m_smoothedPower.end(), 0.0f);
    std::fill(m_prevPostSnr.begin(), m_prevPostSnr.end(), 1.0f);
    std::fill(m_filterGain.begin(), m_filterGain.end(), 1.0f);
    std::fill(m_gainBuf.begin(), m_gainBuf.end(), 1.0f);
    std::fill(m_powerHistory.begin(), m_powerHistory.end(), 0.0f);

    m_histIdx = 0;
    m_noiseInitialized = false;
}

// ── updateGainFromFrame ─────────────────────────────────────────────────────
//
// inBuf: m_fftSize mono analysis samples. Updates m_filterGain, which is then reused for
// independent left/right synthesis so balance survives the MNR stage.

void MacNRFilter::updateGainFromFrame(const float* inBuf)
{
    // ── 1. Apply analysis window and copy into frame buffer ─────────────────
    vDSP_vmul(inBuf, 1, m_window.data(), 1, m_frameBuf.data(), 1, m_fftSize);

    // ── 2. Forward real FFT ─────────────────────────────────────────────────
    // Pack m_fftSize real samples into m_fftSize/2 complex pairs for vDSP
    DSPSplitComplex sc{ m_splitRe.data(), m_splitIm.data() };
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(m_frameBuf.data()), 2, &sc, 1, m_hopSize);
    vDSP_fft_zrip(m_fftSetup, &sc, 1, m_log2n, kFFTDirection_Forward);

    // Extract power spectrum  (m_bins = m_fftSize/2+1 unique bins)
    // Bins 1 … m_hopSize-1 : re² + im²
    // Bin 0        : DC only  (im part holds Nyquist in vDSP packed format)
    // Bin m_hopSize        : Nyquist only
    m_powerBuf[0] = m_splitRe[0] * m_splitRe[0];
    m_powerBuf[m_hopSize] = m_splitIm[0] * m_splitIm[0];
    for (int k = 1; k < m_hopSize; ++k)
        m_powerBuf[k] = m_splitRe[k] * m_splitRe[k] + m_splitIm[k] * m_splitIm[k];

    // ── 3. Smoothed-periodogram minimum-statistics noise update ─────────────
    // A raw periodogram is exponentially distributed, so a 25-frame raw
    // minimum estimates roughly 1/25 of stationary Gaussian noise power.
    // Smooth first, then calibrate the history minimum. Ignore frames below a
    // usable -80 dBFS RMS floor: latency prefill, mute/squelch, and TX silence
    // must not initialize or collapse the learned noise estimate.
    double framePowerSum = 0.0;
    for (int i = 0; i < m_fftSize; ++i) {
        framePowerSum += static_cast<double>(inBuf[i]) * inBuf[i];
    }
    const float meanFramePower = static_cast<float>(framePowerSum / m_fftSize);
    if (meanFramePower < MIN_FRAME_POWER) {
        return;
    }

    if (!m_noiseInitialized) {
        for (int k = 0; k < m_bins; ++k) {
            m_smoothedPower[k] = m_powerBuf[k];
            // Enabling during speech must not initially classify the entire
            // wanted signal as noise. Start conservatively; NOISE_RISE then
            // walks the estimate upward without an over-suppressive transient.
            m_noiseEst[k] = std::max(INITIAL_NOISE_FRACTION * m_powerBuf[k],
                                     1e-10f);
            for (int h = 0; h < HIST; ++h) {
                m_powerHistory[h * m_bins + k] = m_smoothedPower[k];
            }
        }
        m_noiseInitialized = true;
    } else {
        for (int k = 0; k < m_bins; ++k) {
            m_smoothedPower[k] = POWER_SMOOTH * m_smoothedPower[k]
                              + (1.0f - POWER_SMOOTH) * m_powerBuf[k];
            m_powerHistory[m_histIdx * m_bins + k] = m_smoothedPower[k];
        }

        for (int k = 0; k < m_bins; ++k) {
            float minPower = m_powerHistory[0 * m_bins + k];
            for (int h = 1; h < HIST; ++h) {
                minPower = std::min(minPower, m_powerHistory[h * m_bins + k]);
            }
            const float candidate = std::max(MINSTAT_BIAS * minPower, 1e-10f);
            m_noiseEst[k] = candidate < m_noiseEst[k]
                ? candidate
                : NOISE_RISE * m_noiseEst[k] + (1.0f - NOISE_RISE) * candidate;
        }
    }
    m_histIdx = (m_histIdx + 1) % HIST;

    // ── 4. Decision-directed MMSE-Wiener gain ────────────────────────────────
    for (int k = 0; k < m_bins; ++k) {
        // A-posteriori SNR
        const float postSnr = m_powerBuf[k] / std::max(m_noiseEst[k], 1e-10f);

        // A-priori SNR (decision-directed: blend previous clean estimate
        // with new a-posteriori observation)
        const float priorSnr = ALPHA * (m_filterGain[k] * m_filterGain[k]) * m_prevPostSnr[k]
                             + (1.0f - ALPHA) * std::max(postSnr - 1.0f, 0.0f);

        // Raw Wiener gain, clamped to [FLOOR, 1]
        const float g = priorSnr / (priorSnr + OVER);
        m_gainBuf[k] = std::clamp(g, FLOOR, 1.0f);

        // ── Temporal gain smoothing ──────────────────────────────────────
        // Suppresses "musical noise" (rapid frame-to-frame gain swings)
        m_filterGain[k] = GSMOOTH * m_filterGain[k]
                        + (1.0f - GSMOOTH) * m_gainBuf[k];
        m_prevPostSnr[k] = postSnr;
    }
}

// ── synthesizeFrameWithCurrentGain ──────────────────────────────────────────
//
// inBuf  : m_fftSize channel samples
// outBuf : m_fftSize synthesis samples (added into OLA buffer by caller)

void MacNRFilter::synthesizeFrameWithCurrentGain(const float* inBuf, float* outBuf,
                                                  float synthesisStrength)
{
    vDSP_vmul(inBuf, 1, m_window.data(), 1, m_frameBuf.data(), 1, m_fftSize);

    DSPSplitComplex sc{ m_splitRe.data(), m_splitIm.data() };
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(m_frameBuf.data()), 2, &sc, 1, m_hopSize);
    vDSP_fft_zrip(m_fftSetup, &sc, 1, m_log2n, kFFTDirection_Forward);

    // ── Apply shared gain to spectrum ───────────────────────────────────────
    const auto synthesisGain = [synthesisStrength](float filterGain) {
        return 1.0f - synthesisStrength * (1.0f - filterGain);
    };
    m_splitRe[0] *= synthesisGain(m_filterGain[0]);   // DC
    m_splitIm[0] *= synthesisGain(m_filterGain[m_hopSize]);   // Nyquist (stored in im[0] by vDSP)
    for (int k = 1; k < m_hopSize; ++k) {
        const float gain = synthesisGain(m_filterGain[k]);
        m_splitRe[k] *= gain;
        m_splitIm[k] *= gain;
    }

    // ── 6. Inverse real FFT ──────────────────────────────────────────────────
    vDSP_fft_zrip(m_fftSetup, &sc, 1, m_log2n, kFFTDirection_Inverse);
    vDSP_ztoc(&sc, 1, reinterpret_cast<DSPComplex*>(m_synthBuf.data()), 2, m_hopSize);

    // Normalise (vDSP's FFT is un-normalised; factor = 1/(2N))
    const float scale = 1.0f / (2.0f * m_fftSize);
    vDSP_vsmul(m_synthBuf.data(), 1, &scale, m_synthBuf.data(), 1, m_fftSize);

    // ── 7. Apply synthesis window ────────────────────────────────────────────
    vDSP_vmul(m_synthBuf.data(), 1, m_window.data(), 1, outBuf, 1, m_fftSize);
}

// ── process ─────────────────────────────────────────────────────────────────
//
// Input:  24 kHz stereo float32 PCM (0.8.x format)
// Output: same format, same byte count — guaranteed, no silence padding.

QByteArray MacNRFilter::process(const QByteArray& pcmStereo)
{
    if (!m_fftSetup || pcmStereo.isEmpty())
        return pcmStereo;

    const int nBytes   = pcmStereo.size();
    const int nFrames  = nBytes / (2 * sizeof(float));  // 2 ch × 4 bytes each
    const auto* src    = reinterpret_cast<const float*>(pcmStereo.constData());

    // ── Stereo float32 → shared mono analysis plus dry L/R synthesis inputs ──
    for (int i = 0; i < nFrames; ++i) {
        const float L = src[2 * i    ];
        const float R = src[2 * i + 1];
        m_inAccum.push_back(0.5f * (L + R));
        m_inAccumL.push_back(L);
        m_inAccumR.push_back(R);
    }

    // ── OLA processing — emit one hop per iteration ──────────────────────────
    while (static_cast<int>(m_inAccum.size()) >= m_fftSize) {
        updateGainFromFrame(m_inAccum.data());
        // Snapshot once per frame so both channels receive the same shared
        // mask even if the UI changes strength while this block is processed.
        const float synthesisStrength = m_strength.load();

        std::vector<float> outFrameL(m_fftSize, 0.0f);
        std::vector<float> outFrameR(m_fftSize, 0.0f);
        synthesizeFrameWithCurrentGain(m_inAccumL.data(), outFrameL.data(), synthesisStrength);
        synthesizeFrameWithCurrentGain(m_inAccumR.data(), outFrameR.data(), synthesisStrength);

        // Add into OLA buffer
        for (int i = 0; i < m_fftSize; ++i) {
            m_olaBufferL[i] += outFrameL[i];
            m_olaBufferR[i] += outFrameR[i];
        }

        // Flush the first m_hopSize samples to output
        for (int i = 0; i < m_hopSize; ++i) {
            m_outAccumL.push_back(m_olaBufferL[i]);
            m_outAccumR.push_back(m_olaBufferR[i]);
        }

        // Shift OLA buffer left by m_hopSize
        std::copy(m_olaBufferL.begin() + m_hopSize, m_olaBufferL.end(), m_olaBufferL.begin());
        std::copy(m_olaBufferR.begin() + m_hopSize, m_olaBufferR.end(), m_olaBufferR.begin());
        std::fill(m_olaBufferL.begin() + m_hopSize, m_olaBufferL.end(), 0.0f);
        std::fill(m_olaBufferR.begin() + m_hopSize, m_olaBufferR.end(), 0.0f);

        // Consume m_hopSize input samples
        m_inAccum.erase(m_inAccum.begin(), m_inAccum.begin() + m_hopSize);
        m_inAccumL.erase(m_inAccumL.begin(), m_inAccumL.begin() + m_hopSize);
        m_inAccumR.erase(m_inAccumR.begin(), m_inAccumR.begin() + m_hopSize);
    }

    // ── Drain exactly nFrames processed L/R samples → stereo float32 ────────
    // If the accumulator has fewer samples than needed (first few calls during
    // startup), pad with zeros rather than silence the whole buffer.
    const int available = std::min(static_cast<int>(m_outAccumL.size()),
                                   static_cast<int>(m_outAccumR.size()));
    const int take      = std::min(available, nFrames);

    QByteArray out;
    out.resize(nBytes);
    auto* dst = reinterpret_cast<float*>(out.data());

    for (int i = 0; i < take; ++i) {
        dst[2 * i    ] = m_outAccumL[i];
        dst[2 * i + 1] = m_outAccumR[i];
    }

    // Zero-fill any samples not yet produced (startup transient only)
    for (int i = take; i < nFrames; ++i) {
        dst[2 * i    ] = 0.0f;
        dst[2 * i + 1] = 0.0f;
    }

    // Remove consumed samples
    if (take > 0) {
        m_outAccumL.erase(m_outAccumL.begin(), m_outAccumL.begin() + take);
        m_outAccumR.erase(m_outAccumR.begin(), m_outAccumR.begin() + take);
    }

    return out;
}

} // namespace AetherSDR

#endif // __APPLE__
