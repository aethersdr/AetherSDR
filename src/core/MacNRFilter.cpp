#ifdef __APPLE__

#include "MacNRFilter.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace AetherSDR {

// ── Constructor / destructor ────────────────────────────────────────────────

MacNRFilter::MacNRFilter()
{
    m_fftSetup = vDSP_create_fftsetup(LOG2N, kFFTRadix2);
    if (!m_fftSetup)
        return;

    // vDSP split-complex scratch buffers (size H = N/2)
    m_splitRe.assign(H, 0.0f);
    m_splitIm.assign(H, 0.0f);

    // sqrt-Hann window (analysis × synthesis = Hann; perfect reconstruction
    // with 50 % overlap when frames are hop-sized)
    m_window.resize(N);
    for (int i = 0; i < N; ++i)
        m_window[i] = std::sqrt(0.5f * (1.0f - std::cos(2.0f * M_PI * i / N)));

    // Pre-fill input accumulator with one full frame of zeros so that the
    // first call to process() sees a centred first frame immediately and
    // latency is exactly one hop (H samples ≈ 10.7 ms at 24 kHz).
    m_inAccum.assign(N, 0.0f);
    m_inAccumL.assign(N, 0.0f);
    m_inAccumR.assign(N, 0.0f);

    // OLA and scratch buffers
    m_olaBufferL.assign(N, 0.0f);
    m_olaBufferR.assign(N, 0.0f);
    m_frameBuf .assign(N, 0.0f);
    m_synthBuf .assign(N, 0.0f);
    m_outAccumL.clear();
    m_outAccumR.clear();

    // Noise estimator
    m_noiseEst      .assign(NBINS, 0.0f);
    m_smoothedPower .assign(NBINS, 0.0f);
    m_prevPostSnr   .assign(NBINS, 1.0f);
    m_filterGain    .assign(NBINS, 1.0f);
    m_powerBuf      .assign(NBINS, 0.0f);
    m_gainBuf       .assign(NBINS, 1.0f);
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

    // Re-prime input accumulator for one-hop latency (same as constructor)
    m_inAccum.assign(N, 0.0f);
    m_inAccumL.assign(N, 0.0f);
    m_inAccumR.assign(N, 0.0f);

    // Reset noise estimator
    std::fill(m_noiseEst.begin(), m_noiseEst.end(), 0.0f);
    std::fill(m_smoothedPower.begin(), m_smoothedPower.end(), 0.0f);
    std::fill(m_prevPostSnr.begin(), m_prevPostSnr.end(), 1.0f);
    std::fill(m_filterGain.begin(), m_filterGain.end(), 1.0f);
    std::fill(m_gainBuf.begin(), m_gainBuf.end(), 1.0f);
    std::memset(m_powerHistory, 0, sizeof(m_powerHistory));

    m_histIdx = 0;
    m_noiseInitialized = false;
}

// ── updateGainFromFrame ─────────────────────────────────────────────────────
//
// inBuf: N mono analysis samples. Updates m_filterGain, which is then reused for
// independent left/right synthesis so balance survives the MNR stage.

void MacNRFilter::updateGainFromFrame(const float* inBuf)
{
    // ── 1. Apply analysis window and copy into frame buffer ─────────────────
    vDSP_vmul(inBuf, 1, m_window.data(), 1, m_frameBuf.data(), 1, N);

    // ── 2. Forward real FFT ─────────────────────────────────────────────────
    // Pack N real samples into N/2 complex pairs for vDSP
    DSPSplitComplex sc{ m_splitRe.data(), m_splitIm.data() };
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(m_frameBuf.data()), 2, &sc, 1, H);
    vDSP_fft_zrip(m_fftSetup, &sc, 1, LOG2N, kFFTDirection_Forward);

    // Extract power spectrum  (NBINS = N/2+1 unique bins)
    // Bins 1 … H-1 : re² + im²
    // Bin 0        : DC only  (im part holds Nyquist in vDSP packed format)
    // Bin H        : Nyquist only
    m_powerBuf[0] = m_splitRe[0] * m_splitRe[0];
    m_powerBuf[H] = m_splitIm[0] * m_splitIm[0];
    for (int k = 1; k < H; ++k)
        m_powerBuf[k] = m_splitRe[k] * m_splitRe[k] + m_splitIm[k] * m_splitIm[k];

    // ── 3. Smoothed-periodogram minimum-statistics noise update ─────────────
    // A raw periodogram is exponentially distributed, so a 25-frame raw
    // minimum estimates roughly 1/25 of stationary Gaussian noise power.
    // Smooth first, then calibrate the history minimum. Ignore frames below a
    // usable -80 dBFS RMS floor: latency prefill, mute/squelch, and TX silence
    // must not initialize or collapse the learned noise estimate.
    double framePowerSum = 0.0;
    for (int i = 0; i < N; ++i) {
        framePowerSum += static_cast<double>(inBuf[i]) * inBuf[i];
    }
    const float meanFramePower = static_cast<float>(framePowerSum / N);
    if (meanFramePower < MIN_FRAME_POWER) {
        return;
    }

    if (!m_noiseInitialized) {
        for (int k = 0; k < NBINS; ++k) {
            m_smoothedPower[k] = m_powerBuf[k];
            // Enabling during speech must not initially classify the entire
            // wanted signal as noise. Start conservatively; NOISE_RISE then
            // walks the estimate upward without an over-suppressive transient.
            m_noiseEst[k] = std::max(INITIAL_NOISE_FRACTION * m_powerBuf[k],
                                     1e-10f);
            for (int h = 0; h < HIST; ++h) {
                m_powerHistory[h][k] = m_smoothedPower[k];
            }
        }
        m_noiseInitialized = true;
    } else {
        for (int k = 0; k < NBINS; ++k) {
            m_smoothedPower[k] = POWER_SMOOTH * m_smoothedPower[k]
                              + (1.0f - POWER_SMOOTH) * m_powerBuf[k];
            m_powerHistory[m_histIdx][k] = m_smoothedPower[k];
        }

        for (int k = 0; k < NBINS; ++k) {
            float minPower = m_powerHistory[0][k];
            for (int h = 1; h < HIST; ++h) {
                minPower = std::min(minPower, m_powerHistory[h][k]);
            }
            const float candidate = std::max(MINSTAT_BIAS * minPower, 1e-10f);
            m_noiseEst[k] = candidate < m_noiseEst[k]
                ? candidate
                : NOISE_RISE * m_noiseEst[k] + (1.0f - NOISE_RISE) * candidate;
        }
    }
    m_histIdx = (m_histIdx + 1) % HIST;

    // ── 4. Decision-directed MMSE-Wiener gain ────────────────────────────────
    for (int k = 0; k < NBINS; ++k) {
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
// inBuf  : N channel samples
// outBuf : N synthesis samples (added into OLA buffer by caller)

void MacNRFilter::synthesizeFrameWithCurrentGain(const float* inBuf, float* outBuf,
                                                  float synthesisStrength)
{
    vDSP_vmul(inBuf, 1, m_window.data(), 1, m_frameBuf.data(), 1, N);

    DSPSplitComplex sc{ m_splitRe.data(), m_splitIm.data() };
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(m_frameBuf.data()), 2, &sc, 1, H);
    vDSP_fft_zrip(m_fftSetup, &sc, 1, LOG2N, kFFTDirection_Forward);

    // ── Apply shared gain to spectrum ───────────────────────────────────────
    const auto synthesisGain = [synthesisStrength](float filterGain) {
        return 1.0f - synthesisStrength * (1.0f - filterGain);
    };
    m_splitRe[0] *= synthesisGain(m_filterGain[0]);   // DC
    m_splitIm[0] *= synthesisGain(m_filterGain[H]);   // Nyquist (stored in im[0] by vDSP)
    for (int k = 1; k < H; ++k) {
        const float gain = synthesisGain(m_filterGain[k]);
        m_splitRe[k] *= gain;
        m_splitIm[k] *= gain;
    }

    // ── 6. Inverse real FFT ──────────────────────────────────────────────────
    vDSP_fft_zrip(m_fftSetup, &sc, 1, LOG2N, kFFTDirection_Inverse);
    vDSP_ztoc(&sc, 1, reinterpret_cast<DSPComplex*>(m_synthBuf.data()), 2, H);

    // Normalise (vDSP's FFT is un-normalised; factor = 1/(2N))
    const float scale = 1.0f / (2.0f * N);
    vDSP_vsmul(m_synthBuf.data(), 1, &scale, m_synthBuf.data(), 1, N);

    // ── 7. Apply synthesis window ────────────────────────────────────────────
    vDSP_vmul(m_synthBuf.data(), 1, m_window.data(), 1, outBuf, 1, N);
}

// ── process ─────────────────────────────────────────────────────────────────
//
// Input:  24 kHz stereo float32 PCM (0.8.x format)
// Output: same format, same byte count — guaranteed, no silence padding.

QByteArray MacNRFilter::process(const QByteArray& pcm24kStereo)
{
    if (!m_fftSetup || pcm24kStereo.isEmpty())
        return pcm24kStereo;

    const int nBytes   = pcm24kStereo.size();
    const int nFrames  = nBytes / (2 * sizeof(float));  // 2 ch × 4 bytes each
    const auto* src    = reinterpret_cast<const float*>(pcm24kStereo.constData());

    // ── Stereo float32 → shared mono analysis plus dry L/R synthesis inputs ──
    for (int i = 0; i < nFrames; ++i) {
        const float L = src[2 * i    ];
        const float R = src[2 * i + 1];
        m_inAccum.push_back(0.5f * (L + R));
        m_inAccumL.push_back(L);
        m_inAccumR.push_back(R);
    }

    // ── OLA processing — emit one hop per iteration ──────────────────────────
    while (static_cast<int>(m_inAccum.size()) >= N) {
        updateGainFromFrame(m_inAccum.data());
        // Snapshot once per frame so both channels receive the same shared
        // mask even if the UI changes strength while this block is processed.
        const float synthesisStrength = m_strength.load();

        std::vector<float> outFrameL(N, 0.0f);
        std::vector<float> outFrameR(N, 0.0f);
        synthesizeFrameWithCurrentGain(m_inAccumL.data(), outFrameL.data(), synthesisStrength);
        synthesizeFrameWithCurrentGain(m_inAccumR.data(), outFrameR.data(), synthesisStrength);

        // Add into OLA buffer
        for (int i = 0; i < N; ++i) {
            m_olaBufferL[i] += outFrameL[i];
            m_olaBufferR[i] += outFrameR[i];
        }

        // Flush the first H samples to output
        for (int i = 0; i < H; ++i) {
            m_outAccumL.push_back(m_olaBufferL[i]);
            m_outAccumR.push_back(m_olaBufferR[i]);
        }

        // Shift OLA buffer left by H
        std::copy(m_olaBufferL.begin() + H, m_olaBufferL.end(), m_olaBufferL.begin());
        std::copy(m_olaBufferR.begin() + H, m_olaBufferR.end(), m_olaBufferR.begin());
        std::fill(m_olaBufferL.begin() + H, m_olaBufferL.end(), 0.0f);
        std::fill(m_olaBufferR.begin() + H, m_olaBufferR.end(), 0.0f);

        // Consume H input samples
        m_inAccum.erase(m_inAccum.begin(), m_inAccum.begin() + H);
        m_inAccumL.erase(m_inAccumL.begin(), m_inAccumL.begin() + H);
        m_inAccumR.erase(m_inAccumR.begin(), m_inAccumR.begin() + H);
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
