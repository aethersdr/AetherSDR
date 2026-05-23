#include "ClientPudu.h"

#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

constexpr float kTwoPi = 6.283185307179586476f;

float dbToLin(float db) noexcept
{
    return std::pow(10.0f, db * 0.05f);
}

float linToDb(float lin) noexcept
{
    return (lin > 1e-9f) ? 20.0f * std::log10(lin) : -180.0f;
}

float coeffFromMs(float ms, double sampleRate) noexcept
{
    if (ms <= 0.0f) return 1.0f;
    const float tau = ms * 0.001f;
    return 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * tau));
}

// RBJ highpass biquad (Q = 0.707 — 2-pole Butterworth).
void computeHighpass(float fc, double sr,
                     ClientPudu::BiquadCoef& c) noexcept
{
    const float Q     = 0.70710678f;
    const float omega = kTwoPi * fc / static_cast<float>(sr);
    const float sinw  = std::sin(omega);
    const float cosw  = std::cos(omega);
    const float alpha = sinw / (2.0f * Q);

    const float a0    = 1.0f + alpha;
    c.b0 =  (1.0f + cosw) / 2.0f / a0;
    c.b1 = -(1.0f + cosw)        / a0;
    c.b2 =  (1.0f + cosw) / 2.0f / a0;
    c.a1 = -2.0f * cosw          / a0;
    c.a2 =  (1.0f - alpha)       / a0;
}

// RBJ lowpass biquad (Q = 0.707).
void computeLowpass(float fc, double sr,
                    ClientPudu::BiquadCoef& c) noexcept
{
    const float Q     = 0.70710678f;
    const float omega = kTwoPi * fc / static_cast<float>(sr);
    const float sinw  = std::sin(omega);
    const float cosw  = std::cos(omega);
    const float alpha = sinw / (2.0f * Q);

    const float a0    = 1.0f + alpha;
    c.b0 =  (1.0f - cosw) / 2.0f / a0;
    c.b1 =  (1.0f - cosw)        / a0;
    c.b2 =  (1.0f - cosw) / 2.0f / a0;
    c.a1 = -2.0f * cosw          / a0;
    c.a2 =  (1.0f - alpha)       / a0;
}

// 2nd-order all-pass biquad centred at fc — preserves magnitude but
// rotates phase by up to 360° across the corner.  Used in Behringer
// mode LF path for phase-aligned re-injection.
void computeAllpass(float fc, double sr,
                    ClientPudu::BiquadCoef& c) noexcept
{
    const float Q     = 0.70710678f;
    const float omega = kTwoPi * fc / static_cast<float>(sr);
    const float sinw  = std::sin(omega);
    const float cosw  = std::cos(omega);
    const float alpha = sinw / (2.0f * Q);

    const float a0 =  1.0f + alpha;
    c.b0 = (1.0f - alpha) / a0;
    c.b1 = -2.0f * cosw   / a0;
    c.b2 = (1.0f + alpha) / a0;
    c.a1 = -2.0f * cosw   / a0;
    c.a2 = (1.0f - alpha) / a0;
}

inline float processBiquad(float x,
                           const ClientPudu::BiquadCoef& c,
                           ClientPudu::BiquadState& s) noexcept
{
    const float y = c.b0 * x + s.z1;
    s.z1 = c.b1 * x - c.a1 * y + s.z2;
    s.z2 = c.b2 * x - c.a2 * y;
    return y;
}

} // namespace

ClientPudu::ClientPudu() = default;

void ClientPudu::prepare(double sampleRate)
{
    m_sampleRate = sampleRate;
    reset();
    m_atomics.version.fetch_add(1, std::memory_order_release);
    recacheIfDirty();
}

void ClientPudu::setEnabled(bool on) noexcept
{ m_atomics.enabled.store(on, std::memory_order_release); }
bool ClientPudu::isEnabled() const noexcept
{ return m_atomics.enabled.load(std::memory_order_acquire); }

void ClientPudu::setMode(Mode m) noexcept
{
    m_atomics.mode.store(static_cast<uint8_t>(m), std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
ClientPudu::Mode ClientPudu::mode() const noexcept
{ return static_cast<Mode>(m_atomics.mode.load(std::memory_order_relaxed)); }

void ClientPudu::setPooDriveDb(float db) noexcept
{
    m_atomics.pooDriveDb.store(std::clamp(db, 0.0f, 24.0f),
                               std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientPudu::pooDriveDb() const noexcept
{ return m_atomics.pooDriveDb.load(std::memory_order_relaxed); }

void ClientPudu::setPooTuneHz(float hz) noexcept
{
    m_atomics.pooTuneHz.store(std::clamp(hz, 50.0f, 160.0f),
                              std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientPudu::pooTuneHz() const noexcept
{ return m_atomics.pooTuneHz.load(std::memory_order_relaxed); }

void ClientPudu::setPooMix(float v) noexcept
{
    m_atomics.pooMix.store(std::clamp(v, 0.0f, 1.0f),
                           std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientPudu::pooMix() const noexcept
{ return m_atomics.pooMix.load(std::memory_order_relaxed); }

void ClientPudu::setDooTuneHz(float hz) noexcept
{
    m_atomics.dooTuneHz.store(std::clamp(hz, 1000.0f, 10000.0f),
                              std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientPudu::dooTuneHz() const noexcept
{ return m_atomics.dooTuneHz.load(std::memory_order_relaxed); }

void ClientPudu::setDooHarmonicsDb(float db) noexcept
{
    m_atomics.dooHarmonicsDb.store(std::clamp(db, 0.0f, 24.0f),
                                   std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientPudu::dooHarmonicsDb() const noexcept
{ return m_atomics.dooHarmonicsDb.load(std::memory_order_relaxed); }

void ClientPudu::setDooMix(float v) noexcept
{
    m_atomics.dooMix.store(std::clamp(v, 0.0f, 1.0f),
                           std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientPudu::dooMix() const noexcept
{ return m_atomics.dooMix.load(std::memory_order_relaxed); }

void ClientPudu::reset() noexcept
{
    m_chL = {};
    m_chR = {};
    m_lfEnvLin = 0.0f;
}

float ClientPudu::inputPeakDb() const noexcept
{ return m_meters.inputPeakDb.load(std::memory_order_relaxed); }
float ClientPudu::outputPeakDb() const noexcept
{ return m_meters.outputPeakDb.load(std::memory_order_relaxed); }
float ClientPudu::wetRmsDb() const noexcept
{ return m_meters.wetRmsDb.load(std::memory_order_relaxed); }

void ClientPudu::recacheIfDirty() noexcept
{
    const uint64_t v = m_atomics.version.load(std::memory_order_acquire);
    if (v == m_lastVersion) return;
    m_lastVersion = v;

    m_cached.mode = m_atomics.mode.load(std::memory_order_relaxed);

    // HF path.
    const float dooFc = m_atomics.dooTuneHz.load(std::memory_order_relaxed);
    computeHighpass(dooFc, m_sampleRate, m_cached.hpf);
    m_cached.dooDriveLin = dbToLin(
        m_atomics.dooHarmonicsDb.load(std::memory_order_relaxed));
    m_cached.dooMix = m_atomics.dooMix.load(std::memory_order_relaxed);

    // LF path.
    const float pooFc = m_atomics.pooTuneHz.load(std::memory_order_relaxed);
    computeLowpass(pooFc, m_sampleRate, m_cached.lpf);
    computeAllpass(pooFc, m_sampleRate, m_cached.allpass);
    m_cached.pooDriveLin = dbToLin(
        m_atomics.pooDriveDb.load(std::memory_order_relaxed));
    m_cached.pooMix = m_atomics.pooMix.load(std::memory_order_relaxed);

    // LF envelope ballistics.  Aphex dynamic EQ wants slightly
    // slower envelope (~15 ms attack, ~150 ms release) for musical
    // bass enhancement; Behringer compressor wants tighter (~5 / 80).
    // Using a middle ground that works well for both modes.
    m_cached.envAttackCoeff  = coeffFromMs( 8.0f, m_sampleRate);
    m_cached.envReleaseCoeff = coeffFromMs(120.0f, m_sampleRate);
}

// ── HF nonlinearity variants ────────────────────────────────────
//
// Aphex: one-sided tanh approximates a diode across op-amp
// feedback.  The negative half passes unshaped, the positive half
// saturates softly.  Asymmetry generates both odd and even
// harmonics; a DC block removes the resulting offset.
static inline float aphexHfShape(float x) noexcept
{
    return (x >= 0.0f) ? std::tanh(x) : x;
}

// Behringer: symmetric tanh.  Pure odd-harmonic content, tighter
// character.
static inline float behringerHfShape(float x) noexcept
{
    return std::tanh(x);
}

// Single-pole DC block, a = 0.995.  Used on the Aphex HF path to
// remove the offset introduced by one-sided clipping.
static inline float dcBlock(float x, float& x1, float& y1) noexcept
{
    const float y = x - x1 + 0.995f * y1;
    x1 = x;
    y1 = y;
    return y;
}

void ClientPudu::process(float* interleaved, int frames, int channels) noexcept
{
    if (frames <= 0) return;
    if (channels != 1 && channels != 2) return;

    recacheIfDirty();
    const bool enabled = m_atomics.enabled.load(std::memory_order_acquire);

    float inPeakLin  = 0.0f;
    float outPeakLin = 0.0f;
    double wetSumSq  = 0.0;
    int    wetCount  = 0;

    const bool isAphex = (m_cached.mode == static_cast<uint8_t>(Mode::Aphex));
    const float dooDrive  = m_cached.dooDriveLin;
    const float dooMix    = m_cached.dooMix;
    const float pooDrive  = m_cached.pooDriveLin;
    const float pooMix    = m_cached.pooMix;
    const float envAttack  = m_cached.envAttackCoeff;
    const float envRelease = m_cached.envReleaseCoeff;

    for (int f = 0; f < frames; ++f) {
        const float dryL = interleaved[f * channels];
        const float dryR = (channels == 2)
            ? interleaved[f * channels + 1] : dryL;

        const float inAbs = std::max(std::fabs(dryL), std::fabs(dryR));
        if (inAbs > inPeakLin) inPeakLin = inAbs;

        // LF envelope follower (used by both modes on the LPF
        // output).  Envelope feeds the dynamic gain that both
        // algorithms use to shape the LF contribution.
        const float lpfSideL = processBiquad(dryL, m_cached.lpf, m_chL.lpf);
        const float lpfSideR = (channels == 2)
            ? processBiquad(dryR, m_cached.lpf, m_chR.lpf)
            : lpfSideL;
        const float lpAbs = std::max(std::fabs(lpfSideL), std::fabs(lpfSideR));
        const float alpha = (lpAbs > m_lfEnvLin) ? envAttack : envRelease;
        m_lfEnvLin += alpha * (lpAbs - m_lfEnvLin);

        // ── LF wet per mode ─────────────────────────────────────
        // Aphex Big Bottom: LPF → soft saturation scaled by drive,
        // with envelope-tracked dynamic EQ boost so quiet lows get
        // more harmonics than loud ones (keeps it from muddying
        // loud passages).  The envelope inverts: low envelope →
        // more boost.
        //
        // Behringer SX3040: LPF → feed-forward compressor using
        // the envelope as sidechain → all-pass phase rotator →
        // mix.  No harmonic content; transient emphasis only.
        float lfWetL = 0.0f;
        float lfWetR = 0.0f;
        if (isAphex) {
            // Dynamic EQ: boost factor = 1 + drive * (1 - env)
            // so quiet content gets full drive, loud gets less.
            const float dynBoost = 1.0f + pooDrive * (1.0f - std::min(m_lfEnvLin, 1.0f));
            lfWetL = std::tanh(lpfSideL * dynBoost) * 0.5f;
            lfWetR = std::tanh(lpfSideR * dynBoost) * 0.5f;
        } else {
            // Compressor: slope curve reducing gain when envelope
            // crosses threshold.  Threshold chosen relative to
            // drive so the user's Drive knob maps to "how much
            // compression" — more drive, lower threshold.
            const float thr = 0.5f - 0.3f * std::log10(pooDrive + 0.1f);
            float comp = 1.0f;
            if (m_lfEnvLin > thr) {
                // 4:1 ratio, feed-forward.
                const float over = m_lfEnvLin / std::max(thr, 1e-6f);
                comp = std::pow(over, -0.75f);   // 1 - 1/ratio = 0.75
            }
            // All-pass phase rotator on the compressed LF.
            const float apL = processBiquad(lpfSideL * comp,
                                            m_cached.allpass, m_chL.allpass);
            const float apR = (channels == 2)
                ? processBiquad(lpfSideR * comp,
                                m_cached.allpass, m_chR.allpass)
                : apL;
            lfWetL = apL;
            lfWetR = apR;
        }

        // ── HF wet per mode ─────────────────────────────────────
        const float hpfSideL = processBiquad(dryL, m_cached.hpf, m_chL.hpf);
        const float hpfSideR = (channels == 2)
            ? processBiquad(dryR, m_cached.hpf, m_chR.hpf)
            : hpfSideL;
        float hfWetL, hfWetR;
        if (isAphex) {
            hfWetL = aphexHfShape(hpfSideL * dooDrive);
            hfWetR = aphexHfShape(hpfSideR * dooDrive);
            // Remove the DC offset from one-sided clipping.
            hfWetL = dcBlock(hfWetL, m_chL.dcX1, m_chL.dcY1);
            hfWetR = dcBlock(hfWetR, m_chR.dcX1, m_chR.dcY1);
        } else {
            hfWetL = behringerHfShape(hpfSideL * dooDrive);
            hfWetR = behringerHfShape(hpfSideR * dooDrive);
        }

        // Sum: dry + pooMix * lfWet + dooMix * hfWet, with bypass
        // short-circuit.
        float outL = dryL;
        float outR = dryR;
        if (enabled) {
            outL = dryL + pooMix * lfWetL + dooMix * hfWetL;
            outR = dryR + pooMix * lfWetR + dooMix * hfWetR;

            // Track wet content separately so the logo pulse
            // reflects only the exciter contribution (not the dry).
            const float wetL = pooMix * lfWetL + dooMix * hfWetL;
            const float wetR = pooMix * lfWetR + dooMix * hfWetR;
            wetSumSq += static_cast<double>(wetL) * wetL
                      + static_cast<double>(wetR) * wetR;
            wetCount += 2;
        }

        interleaved[f * channels] = outL;
        if (channels == 2) interleaved[f * channels + 1] = outR;

        const float outAbs = std::max(std::fabs(outL), std::fabs(outR));
        if (outAbs > outPeakLin) outPeakLin = outAbs;
    }

    m_meters.inputPeakDb.store(linToDb(std::max(inPeakLin, 1e-6f)),
                               std::memory_order_relaxed);
    m_meters.outputPeakDb.store(linToDb(std::max(outPeakLin, 1e-6f)),
                                std::memory_order_relaxed);
    const float wetRms = (wetCount > 0)
        ? static_cast<float>(std::sqrt(wetSumSq / wetCount))
        : 0.0f;
    m_meters.wetRmsDb.store(linToDb(std::max(wetRms, 1e-6f)),
                            std::memory_order_relaxed);
}

} // namespace MasterSDR
