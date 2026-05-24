#include "ClientDeEss.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

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

// RBJ bandpass (constant 0 dB peak gain).  Centre f, quality q.
void computeBandpass(float f, float q, double sr,
                     ClientDeEss* /*unused*/,
                     float& b0, float& b1, float& b2,
                     float& a1, float& a2)
{
    const float omega = kTwoPi * f / static_cast<float>(sr);
    const float sinw  = std::sin(omega);
    const float cosw  = std::cos(omega);
    const float alpha = sinw / (2.0f * std::max(0.1f, q));

    const float a0 = 1.0f + alpha;
    b0 =  alpha         / a0;
    b1 =  0.0f;
    b2 = -alpha         / a0;
    a1 = (-2.0f * cosw) / a0;
    a2 = (1.0f - alpha) / a0;
}

inline float processBiquad(float x,
                           const ClientDeEss::BiquadCoef& c,
                           ClientDeEss::BiquadState& s) noexcept
{
    // Direct Form II Transposed — low noise at single precision.
    const float y = c.b0 * x + s.z1;
    s.z1 = c.b1 * x - c.a1 * y + s.z2;
    s.z2 = c.b2 * x - c.a2 * y;
    return y;
}

} // namespace

ClientDeEss::ClientDeEss() = default;

void ClientDeEss::prepare(double sampleRate)
{
    m_sampleRate = sampleRate;
    reset();
    m_atomics.version.fetch_add(1, std::memory_order_release);
    recacheIfDirty();
}

void ClientDeEss::setEnabled(bool on) noexcept
{
    m_atomics.enabled.store(on, std::memory_order_release);
}
bool ClientDeEss::isEnabled() const noexcept
{
    return m_atomics.enabled.load(std::memory_order_acquire);
}

void ClientDeEss::setFrequencyHz(float hz) noexcept
{
    m_atomics.frequencyHz.store(std::clamp(hz, 1000.0f, 12000.0f),
                                std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientDeEss::frequencyHz() const noexcept
{ return m_atomics.frequencyHz.load(std::memory_order_relaxed); }

void ClientDeEss::setQ(float q) noexcept
{
    m_atomics.q.store(std::clamp(q, 0.5f, 5.0f),
                      std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientDeEss::q() const noexcept
{ return m_atomics.q.load(std::memory_order_relaxed); }

void ClientDeEss::setThresholdDb(float db) noexcept
{
    m_atomics.thresholdDb.store(std::clamp(db, -60.0f, 0.0f),
                                std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientDeEss::thresholdDb() const noexcept
{ return m_atomics.thresholdDb.load(std::memory_order_relaxed); }

void ClientDeEss::setAmountDb(float db) noexcept
{
    m_atomics.amountDb.store(std::clamp(db, -24.0f, 0.0f),
                             std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientDeEss::amountDb() const noexcept
{ return m_atomics.amountDb.load(std::memory_order_relaxed); }

void ClientDeEss::setAttackMs(float ms) noexcept
{
    m_atomics.attackMs.store(std::clamp(ms, 0.1f, 30.0f),
                             std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientDeEss::attackMs() const noexcept
{ return m_atomics.attackMs.load(std::memory_order_relaxed); }

void ClientDeEss::setReleaseMs(float ms) noexcept
{
    m_atomics.releaseMs.store(std::clamp(ms, 10.0f, 500.0f),
                              std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientDeEss::releaseMs() const noexcept
{ return m_atomics.releaseMs.load(std::memory_order_relaxed); }

void ClientDeEss::setSlopeStages(int stages) noexcept
{
    m_atomics.slopeStages.store(std::clamp(stages, 1, kMaxSlopeStages),
                                std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
int ClientDeEss::slopeStages() const noexcept
{ return m_atomics.slopeStages.load(std::memory_order_relaxed); }

void ClientDeEss::reset() noexcept
{
    m_envLin = 0.0f;
    for (int i = 0; i < kMaxSlopeStages; ++i) {
        m_bpL[i] = {};
        m_bpR[i] = {};
    }
}

float ClientDeEss::inputPeakDb() const noexcept
{ return m_meters.inputPeakDb.load(std::memory_order_relaxed); }
float ClientDeEss::sidechainPeakDb() const noexcept
{ return m_meters.sidechainPeakDb.load(std::memory_order_relaxed); }
float ClientDeEss::gainReductionDb() const noexcept
{ return m_meters.gainReductionDb.load(std::memory_order_relaxed); }

void ClientDeEss::recacheIfDirty() noexcept
{
    const uint64_t v = m_atomics.version.load(std::memory_order_acquire);
    if (v == m_lastVersion) return;
    m_lastVersion = v;

    computeBandpass(
        m_atomics.frequencyHz.load(std::memory_order_relaxed),
        m_atomics.q.load(std::memory_order_relaxed),
        m_sampleRate, this,
        m_cached.bp.b0, m_cached.bp.b1, m_cached.bp.b2,
        m_cached.bp.a1, m_cached.bp.a2);

    m_cached.thresholdDb = m_atomics.thresholdDb.load(std::memory_order_relaxed);
    m_cached.amountDb    = m_atomics.amountDb.load(std::memory_order_relaxed);
    m_cached.attackCoeff = coeffFromMs(
        m_atomics.attackMs.load(std::memory_order_relaxed), m_sampleRate);
    m_cached.releaseCoeff = coeffFromMs(
        m_atomics.releaseMs.load(std::memory_order_relaxed), m_sampleRate);
    m_cached.slopeStages = std::clamp(
        m_atomics.slopeStages.load(std::memory_order_relaxed),
        1, kMaxSlopeStages);
}

float ClientDeEss::staticCurveGainDb(float envDb) const noexcept
{
    // Soft compressor on the sidechain band, clamped to amountDb so
    // total broadband attenuation never exceeds the user's max.
    // Hard-knee is fine here — sibilants are transient and the knee
    // doesn't contribute much to perceived smoothness.
    const float over = envDb - m_cached.thresholdDb;
    if (over <= 0.0f) return 0.0f;
    // 4:1 reduction slope above threshold, clamped to amountDb.
    constexpr float kSlope = 0.75f;   // (1 - 1/4) — feels natural for dessing
    const float gainDb = -over * kSlope;
    return std::max(gainDb, m_cached.amountDb);
}

void ClientDeEss::process(float* interleaved, int frames, int channels) noexcept
{
    if (frames <= 0) return;
    if (channels != 1 && channels != 2) return;

    recacheIfDirty();
    const bool enabled = m_atomics.enabled.load(std::memory_order_acquire);

    float inPeakLin = 0.0f;
    float scPeakLin = 0.0f;
    float worstGrDb = 0.0f;

    const float attackCoeff  = m_cached.attackCoeff;
    const float releaseCoeff = m_cached.releaseCoeff;
    const BiquadCoef& bp     = m_cached.bp;

    for (int f = 0; f < frames; ++f) {
        float l = interleaved[f * channels];
        float r = (channels == 2) ? interleaved[f * channels + 1] : l;

        const float inAbs = std::max(std::fabs(l), std::fabs(r));
        if (inAbs > inPeakLin) inPeakLin = inAbs;

        // Sidechain: cascade N bandpass biquads per channel where N
        // is the user-selectable slope-stage count (1..kMaxSlopeStages).
        // Each stage adds 12 dB/oct of rolloff, so the de-esser
        // notch can be dialled from broad (1 stage, 12 dB/oct) to
        // surgical (4 stages, 48 dB/oct). Filters stay live even
        // when disabled so their state is warm if the user toggles
        // enable.
        float scL = l;
        float scR = (channels == 2) ? r : l;
        const int stages = m_cached.slopeStages;
        for (int s = 0; s < stages; ++s) {
            scL = processBiquad(scL, bp, m_bpL[s]);
            if (channels == 2)
                scR = processBiquad(scR, bp, m_bpR[s]);
        }
        if (channels != 2) scR = scL;
        const float scAbs = std::max(std::fabs(scL), std::fabs(scR));
        if (scAbs > scPeakLin) scPeakLin = scAbs;

        float gainLin = 1.0f;
        if (enabled) {
            // Peak envelope on the sidechain band.
            const float alpha = (scAbs > m_envLin) ? attackCoeff : releaseCoeff;
            m_envLin += alpha * (scAbs - m_envLin);
            const float envDb = linToDb(std::max(m_envLin, 1e-6f));

            const float gainDb = staticCurveGainDb(envDb);
            if (gainDb < worstGrDb) worstGrDb = gainDb;
            gainLin = dbToLin(gainDb);
        }

        // Split-band de-essing: only attenuate the sibilant band
        // (the bandpass output), leave lows + mids untouched. The
        // bandpass is a constant-0-dB-peak filter, so subtracting
        // bp*(1-gain) from the full signal reduces only the 4-8 kHz
        // slice by the gain amount. The original broadband-attenuate
        // implementation pulled the whole signal down on sibilance,
        // crashing RMS during S-heavy phrases.
        //   output = full + bp * (gain - 1)
        // gain = 1  → output = full       (no change, perfect pass-through)
        // gain = 0.5→ output = full - 0.5*bp (HF band reduced by 6 dB)
        if (enabled && gainLin < 0.9999f) {
            const float scGainDelta = gainLin - 1.0f;
            l += scL * scGainDelta;
            r += scR * scGainDelta;
        }
        interleaved[f * channels] = l;
        if (channels == 2) interleaved[f * channels + 1] = r;
    }

    m_meters.inputPeakDb.store(linToDb(std::max(inPeakLin, 1e-6f)),
                               std::memory_order_relaxed);
    m_meters.sidechainPeakDb.store(linToDb(std::max(scPeakLin, 1e-6f)),
                                   std::memory_order_relaxed);
    m_meters.gainReductionDb.store(worstGrDb, std::memory_order_relaxed);
}

} // namespace AetherSDR
