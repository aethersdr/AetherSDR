#include "ClientReverb.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace MasterSDR {

namespace {

// Jezar's Freeverb tunings at 44.1 kHz.  L-channel lengths; R-channel
// adds kStereoSpread to each.  Public domain (Jezar 2000).
constexpr int kCombTuningsL44k[8] =
    {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr int kAllpassTuningsL44k[4] =
    {556, 441, 341, 225};

// Input attenuation — prevents feedback chain from blowing up when all
// eight combs sum.
constexpr float kFixedGain = 0.015f;

float linToDb(float lin) noexcept
{
    return (lin > 1e-9f) ? 20.0f * std::log10(lin) : -180.0f;
}

int scaleLenForRate(int refLen, double sampleRate)
{
    return std::max(1, static_cast<int>(
        std::lround(refLen * (sampleRate / 44100.0))));
}

} // namespace

ClientReverb::ClientReverb() = default;

void ClientReverb::prepare(double sampleRate)
{
    m_sampleRate = sampleRate;

    // Allocate comb + allpass delay buffers at max (Size=1) lengths,
    // per channel.  Stereo-spread adds kStereoSpread samples on the
    // right channel.
    // Allocate both channels at the SAME max length — L's stereo-spread
    // variant is the upper bound for both so a single cached active-
    // length works without overrunning either buffer.  Trades ~23
    // unused samples per comb per channel for a simpler index loop.
    for (int i = 0; i < kNumCombs; ++i) {
        const int baseLen = scaleLenForRate(kCombTuningsL44k[i], sampleRate);
        const int maxLen  = baseLen + kStereoSpread;
        m_maxCombLen[i] = maxLen;
        m_chL.combs[i].buffer.assign(maxLen, 0.0f);
        m_chL.combs[i].index    = 0;
        m_chL.combs[i].lpfState = 0.0f;
        m_chR.combs[i].buffer.assign(maxLen, 0.0f);
        m_chR.combs[i].index    = 0;
        m_chR.combs[i].lpfState = 0.0f;
    }
    for (int i = 0; i < kNumAllpasses; ++i) {
        const int baseLen = scaleLenForRate(kAllpassTuningsL44k[i], sampleRate);
        const int maxLen  = baseLen + kStereoSpread;
        m_maxAllpassLen[i] = maxLen;
        m_chL.allpasses[i].buffer.assign(maxLen, 0.0f);
        m_chL.allpasses[i].index = 0;
        m_chR.allpasses[i].buffer.assign(maxLen, 0.0f);
        m_chR.allpasses[i].index = 0;
    }

    // Pre-delay: stereo-interleaved buffer sized for kMaxPreDelayMs.
    m_preDelayCap = static_cast<int>(
        std::lround(kMaxPreDelayMs * 0.001 * sampleRate)) + 1;
    m_preDelay.assign(m_preDelayCap * 2, 0.0f);
    m_preDelayWrite = 0;

    m_atomics.version.fetch_add(1, std::memory_order_release);
    recacheIfDirty();
}

void ClientReverb::setEnabled(bool on) noexcept
{
    m_atomics.enabled.store(on, std::memory_order_release);
}
bool ClientReverb::isEnabled() const noexcept
{
    return m_atomics.enabled.load(std::memory_order_acquire);
}

void ClientReverb::setSize(float s) noexcept
{
    m_atomics.size.store(std::clamp(s, 0.0f, 1.0f), std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientReverb::size() const noexcept
{ return m_atomics.size.load(std::memory_order_relaxed); }

void ClientReverb::setDecayS(float s) noexcept
{
    m_atomics.decayS.store(std::clamp(s, 0.3f, 5.0f),
                           std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientReverb::decayS() const noexcept
{ return m_atomics.decayS.load(std::memory_order_relaxed); }

void ClientReverb::setDamping(float d) noexcept
{
    m_atomics.damping.store(std::clamp(d, 0.0f, 1.0f),
                            std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientReverb::damping() const noexcept
{ return m_atomics.damping.load(std::memory_order_relaxed); }

void ClientReverb::setPreDelayMs(float ms) noexcept
{
    m_atomics.preDelayMs.store(
        std::clamp(ms, 0.0f, static_cast<float>(kMaxPreDelayMs)),
        std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientReverb::preDelayMs() const noexcept
{ return m_atomics.preDelayMs.load(std::memory_order_relaxed); }

void ClientReverb::setMix(float m) noexcept
{
    m_atomics.mix.store(std::clamp(m, 0.0f, 1.0f),
                        std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientReverb::mix() const noexcept
{ return m_atomics.mix.load(std::memory_order_relaxed); }

void ClientReverb::reset() noexcept
{
    for (int i = 0; i < kNumCombs; ++i) {
        std::fill(m_chL.combs[i].buffer.begin(),
                  m_chL.combs[i].buffer.end(), 0.0f);
        std::fill(m_chR.combs[i].buffer.begin(),
                  m_chR.combs[i].buffer.end(), 0.0f);
        m_chL.combs[i].index    = 0;
        m_chR.combs[i].index    = 0;
        m_chL.combs[i].lpfState = 0.0f;
        m_chR.combs[i].lpfState = 0.0f;
    }
    for (int i = 0; i < kNumAllpasses; ++i) {
        std::fill(m_chL.allpasses[i].buffer.begin(),
                  m_chL.allpasses[i].buffer.end(), 0.0f);
        std::fill(m_chR.allpasses[i].buffer.begin(),
                  m_chR.allpasses[i].buffer.end(), 0.0f);
        m_chL.allpasses[i].index = 0;
        m_chR.allpasses[i].index = 0;
    }
    std::fill(m_preDelay.begin(), m_preDelay.end(), 0.0f);
    m_preDelayWrite = 0;
}

float ClientReverb::inputPeakDb() const noexcept
{ return m_inputPeakDb.load(std::memory_order_relaxed); }
float ClientReverb::outputPeakDb() const noexcept
{ return m_outputPeakDb.load(std::memory_order_relaxed); }
float ClientReverb::wetRmsDb() const noexcept
{ return m_wetRmsDb.load(std::memory_order_relaxed); }

void ClientReverb::recacheIfDirty() noexcept
{
    const uint64_t v = m_atomics.version.load(std::memory_order_acquire);
    if (v == m_lastVersion) return;
    m_lastVersion = v;

    const float size    = m_atomics.size.load(std::memory_order_relaxed);
    const float decayS  = m_atomics.decayS.load(std::memory_order_relaxed);
    const float damping = m_atomics.damping.load(std::memory_order_relaxed);
    const float preMs   = m_atomics.preDelayMs.load(std::memory_order_relaxed);
    const float mix     = m_atomics.mix.load(std::memory_order_relaxed);

    // Size scales comb lengths between 50 % and 100 % of max.  Shorter
    // combs → tighter room; longer → bigger space.
    const float sizeScale = 0.5f + 0.5f * size;

    // Per-comb: scale by size, convert delay time to seconds, then
    // derive T60-style feedback coefficient.  6.91 ≈ ln(1000) so a
    // sample at the far end of decayS seconds is attenuated by -60 dB.
    for (int i = 0; i < kNumCombs; ++i) {
        const int activeLen = std::max(1,
            static_cast<int>(std::lround(m_maxCombLen[i] * sizeScale)));
        m_cached.combLen[i] = std::min(activeLen, m_maxCombLen[i]);
        const float delaySec =
            static_cast<float>(m_cached.combLen[i]) /
            static_cast<float>(m_sampleRate);
        m_cached.combFeedback[i] =
            std::exp(-6.91f * delaySec / std::max(0.05f, decayS));
        // Cap well below 1 for numerical safety.
        m_cached.combFeedback[i] = std::min(m_cached.combFeedback[i], 0.985f);
    }
    for (int i = 0; i < kNumAllpasses; ++i) {
        // Allpass lengths don't depend on size (they're for diffusion,
        // not tail length) — but they're still clamped to the
        // pre-allocated max.
        m_cached.allpassLen[i] = m_maxAllpassLen[i];
    }

    m_cached.damp1 = damping * 0.4f;      // freeverb's kScaleDamp
    m_cached.damp2 = 1.0f - m_cached.damp1;
    m_cached.mix   = mix;
    m_cached.dryGain = 1.0f - 0.5f * mix;  // gentle equal-ish-power blend
    m_cached.wetGain = mix;
    m_cached.preDelaySamples = std::clamp(
        static_cast<int>(std::lround(preMs * 0.001 * m_sampleRate)),
        0, m_preDelayCap - 1);
}

namespace {

// Process one sample through a single lowpass-feedback comb.  The LPF
// is a one-pole smoother in the feedback path: damp1 + damp2 = 1.
// Templated on the struct type so we don't need friend access.
template <typename CombT>
inline float processCombImpl(CombT& c, float in,
                             float feedback, float damp1, float damp2,
                             int activeLen)
{
    const float out = c.buffer[c.index];
    c.lpfState = out * damp2 + c.lpfState * damp1;
    c.buffer[c.index] = in + c.lpfState * feedback;
    c.index = (c.index + 1) % activeLen;
    return out;
}

// Process one sample through a Schroeder allpass.  Feedback fixed at 0.5.
template <typename ApT>
inline float processAllpassImpl(ApT& a, float in, int activeLen)
{
    const float bufOut = a.buffer[a.index];
    const float out    = -in + bufOut;
    a.buffer[a.index]  = in + bufOut * 0.5f;
    a.index = (a.index + 1) % activeLen;
    return out;
}

} // namespace

void ClientReverb::process(float* interleaved, int frames, int channels) noexcept
{
    if (frames <= 0) return;
    if (channels != 1 && channels != 2) return;

    recacheIfDirty();
    const bool enabled = m_atomics.enabled.load(std::memory_order_acquire);

    float inPeakLin  = 0.0f;
    float outPeakLin = 0.0f;
    float wetSumSq   = 0.0f;

    const float dryGain   = m_cached.dryGain;
    const float wetGain   = m_cached.wetGain;
    const float damp1     = m_cached.damp1;
    const float damp2     = m_cached.damp2;
    const int   preDelayN = m_cached.preDelaySamples;

    for (int f = 0; f < frames; ++f) {
        const float inL = interleaved[f * channels];
        const float inR = (channels == 2) ? interleaved[f * channels + 1] : inL;

        const float inAbs = std::max(std::fabs(inL), std::fabs(inR));
        if (inAbs > inPeakLin) inPeakLin = inAbs;

        if (!enabled) {
            // Bypass path — leave sample untouched.
            const float oAbs = std::max(std::fabs(inL), std::fabs(inR));
            if (oAbs > outPeakLin) outPeakLin = oAbs;
            continue;
        }

        // Pre-delay stage.  Write raw input to ring buffer; read the
        // sample that entered preDelayN frames ago.
        m_preDelay[m_preDelayWrite * 2]     = inL;
        m_preDelay[m_preDelayWrite * 2 + 1] = inR;
        int readIdx = m_preDelayWrite - preDelayN;
        if (readIdx < 0) readIdx += m_preDelayCap;
        const float delL = m_preDelay[readIdx * 2];
        const float delR = m_preDelay[readIdx * 2 + 1];
        m_preDelayWrite = (m_preDelayWrite + 1) % m_preDelayCap;

        // Feed reverb with attenuated delayed signal.
        const float inputL = delL * kFixedGain;
        const float inputR = delR * kFixedGain;

        // 8 parallel combs, summed per channel.
        float wetL = 0.0f;
        float wetR = 0.0f;
        for (int i = 0; i < kNumCombs; ++i) {
            wetL += processCombImpl(m_chL.combs[i], inputL,
                                m_cached.combFeedback[i], damp1, damp2,
                                m_cached.combLen[i]);
            wetR += processCombImpl(m_chR.combs[i], inputR,
                                m_cached.combFeedback[i], damp1, damp2,
                                m_cached.combLen[i]);
        }

        // 4 series allpasses diffuse the comb sum.
        for (int i = 0; i < kNumAllpasses; ++i) {
            wetL = processAllpassImpl(m_chL.allpasses[i], wetL,
                                  m_cached.allpassLen[i]);
            wetR = processAllpassImpl(m_chR.allpasses[i], wetR,
                                  m_cached.allpassLen[i]);
        }

        wetSumSq += wetL * wetL + wetR * wetR;

        // Dry + wet blend.
        const float outL = inL * dryGain + wetL * wetGain;
        const float outR = inR * dryGain + wetR * wetGain;

        interleaved[f * channels] = outL;
        if (channels == 2) interleaved[f * channels + 1] = outR;

        const float oAbs = std::max(std::fabs(outL), std::fabs(outR));
        if (oAbs > outPeakLin) outPeakLin = oAbs;
    }

    m_inputPeakDb.store(linToDb(std::max(inPeakLin, 1e-6f)),
                        std::memory_order_relaxed);
    m_outputPeakDb.store(linToDb(std::max(outPeakLin, 1e-6f)),
                         std::memory_order_relaxed);
    const float wetRms = (enabled && frames > 0)
        ? std::sqrt(wetSumSq / (2.0f * frames)) : 0.0f;
    m_wetRmsDb.store(linToDb(std::max(wetRms, 1e-6f)),
                     std::memory_order_relaxed);
}

} // namespace MasterSDR
