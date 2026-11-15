#include "core/WfmDsp.h"
#include "core/Resampler.h"

#include <QByteArray>
#include <algorithm>
#include <cmath>
#include <numbers>   // std::numbers::pi — M_PI needs _USE_MATH_DEFINES on MSVC

namespace AetherSDR {

// FIR low-pass (buildFirLP): fs = 48 kHz, fc = 20 kHz (18 kHz pass / 22 kHz
// stop), order M = 94 → 95 taps, Hamming (≥ 53 dB stopband), Type-I linear
// phase (h[n] = h[N-1-n]), DC gain normalised to 1.
//   t    = n − M/2
//   sinc = sin(π·2·fc·t) / (π·2·fc·t),   sinc(0) = 1
//   win  = 0.54 − 0.46·cos(2π·n / M)
//   h[n] = sinc · win, then normalise so Σ h[n] = 1
static std::array<float, WfmDsp::kFirTaps> buildFirLP()
{
    constexpr int   M  = WfmDsp::kFirOrder;   // 94
    constexpr float fc = static_cast<float>(WfmDsp::kLpCutoffHz)
                       / static_cast<float>(WfmDsp::kAudioRate); // 20000/48000
    const float pi = static_cast<float>(std::numbers::pi);

    std::array<float, WfmDsp::kFirTaps> h{};
    float sum = 0.0f;

    for (int n = 0; n <= M; ++n) {
        const float t = static_cast<float>(n) - static_cast<float>(M) * 0.5f;

        // Ideal low-pass impulse response (sinc)
        float sinc;
        if (std::abs(t) < 1e-7f) {
            sinc = 1.0f;
        } else {
            const float x = 2.0f * fc * pi * t;   // π · 2fc · t
            sinc = std::sin(x) / x;
        }

        // Hamming window: w[n] = 0.54 − 0.46·cos(2π·n/M)
        const float win = 0.54f
                        - 0.46f * std::cos(2.0f * pi * static_cast<float>(n)
                                                      / static_cast<float>(M));

        h[n] = sinc * win;
        sum += h[n];
    }

    // Normalise to unity DC gain: Σ h[n] = 1  →  no DC notch
    for (float& v : h) v /= sum;

    return h;
}

// Computed once at program start; const so it lives in .rodata
static const std::array<float, WfmDsp::kFirTaps> kFirLP = buildFirLP();

// ---------------------------------------------------------------------------
// WfmDsp
// ---------------------------------------------------------------------------

WfmDsp::WfmDsp(int iqRateHz)
    : m_iqRate(iqRateHz > 0 ? iqRateHz : kAudioRate)
{
    if (m_iqRate != kAudioRate) {
        // reqTransBand = 5 (% of Nyquist) → passband flat to 0.95·Nyquist,
        // the same useful-bandwidth ratio SkyRoof uses for its decimators
        // (USEFUL_BANDWIDTH = 0.95 · 24 kHz). r8brain is linear-phase, so
        // two instances with identical parameters stay phase-matched.
        constexpr int    kMaxBlock  = 8192;
        constexpr double kTransBand = 5.0;
        m_resampleI = std::make_unique<Resampler>(m_iqRate, kAudioRate,
                                                  kMaxBlock, kTransBand);
        m_resampleQ = std::make_unique<Resampler>(m_iqRate, kAudioRate,
                                                  kMaxBlock, kTransBand);
    }
}

WfmDsp::~WfmDsp() = default;

float WfmDsp::maxFreqOffsetHz() const
{
    const float usableHz = 0.95f * 0.5f
                         * static_cast<float>(std::min(m_iqRate, kAudioRate));
    constexpr float kSignalGuardHz = 8000.0f;   // G3RUH Carson half-bandwidth
    return std::max(0.0f, usableHz - kSignalGuardHz);
}

void WfmDsp::setFreqOffsetHz(float offsetHz)
{
    // Mix DOWN: a negative rotation moves +offsetHz to 0 Hz. Only the step
    // changes; m_ncoPhase keeps accumulating → the retune is phase-continuous.
    m_ncoStep = -2.0 * std::numbers::pi * static_cast<double>(offsetHz) / m_iqRate;
}

void WfmDsp::process(const float* iqInterleaved, int frames,
                     std::vector<float>& audioOut)
{
    audioOut.clear();
    if (!iqInterleaved || frames <= 0) return;

    // [1] NCO mix-down + deinterleave: (I+jQ)·e^{jθ}, θ += step per sample
    m_workI.resize(static_cast<size_t>(frames));
    m_workQ.resize(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const float I = iqInterleaved[2 * i];
        const float Q = iqInterleaved[2 * i + 1];
        const float c = static_cast<float>(std::cos(m_ncoPhase));
        const float s = static_cast<float>(std::sin(m_ncoPhase));
        m_workI[static_cast<size_t>(i)] = I * c - Q * s;
        m_workQ[static_cast<size_t>(i)] = I * s + Q * c;
        m_ncoPhase += m_ncoStep;
        if (m_ncoPhase > std::numbers::pi)       m_ncoPhase -= 2.0 * std::numbers::pi;
        else if (m_ncoPhase < -std::numbers::pi) m_ncoPhase += 2.0 * std::numbers::pi;
    }

    // [2] resample I and Q to exactly 48 kHz (identical twins ⇒ identical
    // output lengths; min() is belt-and-braces only)
    const float* pI = m_workI.data();
    const float* pQ = m_workQ.data();
    int n = frames;
    QByteArray bufI, bufQ;
    if (m_resampleI) {
        bufI = m_resampleI->process(pI, frames);
        bufQ = m_resampleQ->process(pQ, frames);
        n  = static_cast<int>(std::min(bufI.size(), bufQ.size())
                              / static_cast<qsizetype>(sizeof(float)));
        if (n <= 0) return;
        pI = reinterpret_cast<const float*>(bufI.constData());
        pQ = reinterpret_cast<const float*>(bufQ.constData());
    }

    audioOut.resize(static_cast<size_t>(n));

    float prevI = m_prevI;
    float prevQ = m_prevQ;

    const float pi   = static_cast<float>(std::numbers::pi);
    const float norm = kGain / pi;   // atan2 ∈ [−π,π] → disc ∈ [−kGain, kGain]

    for (int i = 0; i < n; ++i) {
        const float I = pI[i];
        const float Q = pQ[i];

        // [3] FM discriminator — atan2, amplitude-invariant
        //   Standard convention: disc = arg(curr·conj(prev)) = Δφ, so a
        //   positive frequency deviation gives a positive output.
        //   cross = Im(conj(prev)·curr) = Q·Iprev − I·Qprev
        //   dot   = Re(conj(prev)·curr) = I·Iprev + Q·Qprev
        const float cross = Q * prevI - I * prevQ;
        const float dot   = I * prevI + Q * prevQ;
        const float disc  = std::atan2(cross, dot) * norm;

        prevI = I;
        prevQ = Q;

        // [4] FIR low-pass — 95 taps, Hamming, fc = 20 kHz, h[n]=h[N−1−n], Σh=1
        //   Circular delay line; newest sample at m_firIdx, then convolve.
        m_firBuf[static_cast<size_t>(m_firIdx)] = disc;
        float lp = 0.0f;
        for (int k = 0; k < kFirTaps; ++k)
            lp += kFirLP[static_cast<size_t>(k)]
                * m_firBuf[static_cast<size_t>((m_firIdx - k + kFirTaps) % kFirTaps)];
        m_firIdx = (m_firIdx + 1) % kFirTaps;

        audioOut[static_cast<size_t>(i)] = lp;
    }

    m_prevI = prevI;
    m_prevQ = prevQ;
}

} // namespace AetherSDR
