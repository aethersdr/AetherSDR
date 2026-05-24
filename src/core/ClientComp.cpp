#include "ClientComp.h"

#include "ClientPhaseRotator.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

float dbToLin(float db) noexcept
{
    return std::pow(10.0f, db * 0.05f);
}

float linToDb(float lin) noexcept
{
    return (lin > 1e-9f) ? 20.0f * std::log10(lin) : -180.0f;
}

// Classic time-constant coefficient: y += α·(target - y) with
// α = 1 - exp(-1 / (fs · τ)).  Hits ≈63 % of the step in τ seconds.
float coeffFromMs(float ms, double sampleRate) noexcept
{
    if (ms <= 0.0f) return 1.0f;
    const float tau = ms * 0.001f;
    return 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * tau));
}

} // namespace

ClientComp::ClientComp()
    : m_phaseRotator(std::make_unique<ClientPhaseRotator>())
{
    // Phase rotator stays internally "always enabled" — its stages
    // count gates whether it does anything (0 = pass-through). Drive
    // is owned by ClientComp; the rotator runs unconditionally and is
    // followed by the drive gain before the comp curve sees the signal.
    m_phaseRotator->setEnabled(true);
    m_phaseRotator->setStages(0);
}

ClientComp::~ClientComp() = default;

void ClientComp::prepare(double sampleRate)
{
    m_sampleRate = sampleRate;
    m_envLin     = 0.0f;
    m_limEnvLin  = 0.0f;

    if (m_phaseRotator) m_phaseRotator->prepare(sampleRate);

    // Bump version so recacheIfDirty rebuilds coefficients on first block.
    m_atomics.version.fetch_add(1, std::memory_order_release);
    recacheIfDirty();
}

void ClientComp::setEnabled(bool on) noexcept
{
    m_atomics.enabled.store(on, std::memory_order_release);
}

bool ClientComp::isEnabled() const noexcept
{
    return m_atomics.enabled.load(std::memory_order_acquire);
}

void ClientComp::setThresholdDb(float db) noexcept
{
    m_atomics.thresholdDb.store(std::clamp(db, -60.0f, 0.0f),
                                std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientComp::thresholdDb() const noexcept
{ return m_atomics.thresholdDb.load(std::memory_order_relaxed); }

void ClientComp::setRatio(float ratio) noexcept
{
    m_atomics.ratio.store(std::clamp(ratio, 1.0f, 20.0f),
                          std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientComp::ratio() const noexcept
{ return m_atomics.ratio.load(std::memory_order_relaxed); }

void ClientComp::setAttackMs(float ms) noexcept
{
    m_atomics.attackMs.store(std::clamp(ms, 0.1f, 300.0f),
                             std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientComp::attackMs() const noexcept
{ return m_atomics.attackMs.load(std::memory_order_relaxed); }

void ClientComp::setReleaseMs(float ms) noexcept
{
    m_atomics.releaseMs.store(std::clamp(ms, 5.0f, 2000.0f),
                              std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientComp::releaseMs() const noexcept
{ return m_atomics.releaseMs.load(std::memory_order_relaxed); }

void ClientComp::setKneeDb(float db) noexcept
{
    m_atomics.kneeDb.store(std::clamp(db, 0.0f, 24.0f),
                           std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientComp::kneeDb() const noexcept
{ return m_atomics.kneeDb.load(std::memory_order_relaxed); }

void ClientComp::setMakeupDb(float db) noexcept
{
    m_atomics.makeupDb.store(std::clamp(db, -12.0f, 24.0f),
                             std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientComp::makeupDb() const noexcept
{ return m_atomics.makeupDb.load(std::memory_order_relaxed); }

void ClientComp::setLimiterEnabled(bool on) noexcept
{
    m_atomics.limEnabled.store(on, std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
bool ClientComp::limiterEnabled() const noexcept
{ return m_atomics.limEnabled.load(std::memory_order_relaxed); }

void ClientComp::setLimiterCeilingDb(float db) noexcept
{
    m_atomics.limCeilingDb.store(std::clamp(db, -24.0f, 0.0f),
                                 std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientComp::limiterCeilingDb() const noexcept
{ return m_atomics.limCeilingDb.load(std::memory_order_relaxed); }

void ClientComp::setDriveDb(float db) noexcept
{
    m_atomics.driveDb.store(std::clamp(db, 0.0f, 18.0f),
                            std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientComp::driveDb() const noexcept
{ return m_atomics.driveDb.load(std::memory_order_relaxed); }

void ClientComp::setPhaseRotatorStages(int stages) noexcept
{
    m_atomics.phaseRotatorStages.store(std::clamp(stages, 0, 6),
                                       std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
int ClientComp::phaseRotatorStages() const noexcept
{ return m_atomics.phaseRotatorStages.load(std::memory_order_relaxed); }

void ClientComp::reset() noexcept
{
    m_envLin = 0.0f;
    m_limEnvLin = 0.0f;
}

float ClientComp::inputPeakDb() const noexcept
{ return m_meters.inputPeakDb.load(std::memory_order_relaxed); }
float ClientComp::outputPeakDb() const noexcept
{ return m_meters.outputPeakDb.load(std::memory_order_relaxed); }
float ClientComp::gainReductionDb() const noexcept
{ return m_meters.gainReductionDb.load(std::memory_order_relaxed); }
float ClientComp::limiterGrDb() const noexcept
{ return m_meters.limiterGrDb.load(std::memory_order_relaxed); }
bool  ClientComp::limiterActive() const noexcept
{ return m_meters.limiterActive.load(std::memory_order_relaxed); }

void ClientComp::recacheIfDirty() noexcept
{
    const uint64_t v = m_atomics.version.load(std::memory_order_acquire);
    if (v == m_lastVersion) return;
    m_lastVersion = v;

    m_cached.thresholdDb = m_atomics.thresholdDb.load(std::memory_order_relaxed);
    const float r        = std::max(1.0f,
                                    m_atomics.ratio.load(std::memory_order_relaxed));
    m_cached.ratioInv    = 1.0f / r;
    m_cached.attackCoeff = coeffFromMs(
        m_atomics.attackMs.load(std::memory_order_relaxed), m_sampleRate);
    m_cached.releaseCoeff = coeffFromMs(
        m_atomics.releaseMs.load(std::memory_order_relaxed), m_sampleRate);
    m_cached.kneeDb      = m_atomics.kneeDb.load(std::memory_order_relaxed);
    m_cached.makeupLin   = dbToLin(
        m_atomics.makeupDb.load(std::memory_order_relaxed));
    m_cached.limEnabled  = m_atomics.limEnabled.load(std::memory_order_relaxed);
    m_cached.limCeilingLin = dbToLin(
        m_atomics.limCeilingDb.load(std::memory_order_relaxed));
    // Limiter ballistics: very fast attack, moderately fast release.
    m_cached.limAttackCoeff  = coeffFromMs(0.1f, m_sampleRate);
    m_cached.limReleaseCoeff = coeffFromMs(50.0f, m_sampleRate);

    m_cached.driveLin = dbToLin(
        m_atomics.driveDb.load(std::memory_order_relaxed));
    m_cached.phaseRotatorStages =
        m_atomics.phaseRotatorStages.load(std::memory_order_relaxed);
    if (m_phaseRotator)
        m_phaseRotator->setStages(m_cached.phaseRotatorStages);
}

float ClientComp::staticCurveGainDb(float envDb) const noexcept
{
    // Soft-knee downward compression in dB domain.  Returns the gain
    // adjustment (≤ 0 dB) to apply to the signal at this envelope level.
    const float T    = m_cached.thresholdDb;
    const float W    = m_cached.kneeDb;
    const float overshoot = envDb - T;
    const float slope = 1.0f - m_cached.ratioInv;  // 0 = bypass, 1 = limit

    if (W <= 0.0f) {
        // Hard knee
        return (overshoot > 0.0f) ? -overshoot * slope : 0.0f;
    }

    if (overshoot <= -0.5f * W) {
        return 0.0f;  // below knee
    }
    if (overshoot >= 0.5f * W) {
        return -overshoot * slope;  // above knee
    }
    // Soft-knee quadratic interpolation: smoothly blends from 0 to full
    // reduction across [-W/2, +W/2].
    const float x = overshoot + 0.5f * W;  // 0 .. W
    return -slope * (x * x) / (2.0f * W);
}

void ClientComp::process(float* interleaved, int frames, int channels) noexcept
{
    if (frames <= 0) return;
    if (channels != 1 && channels != 2) return;

    recacheIfDirty();
    const bool enabled = m_atomics.enabled.load(std::memory_order_acquire);

    // Pre-comp PAPR shaping (#2887). The rotator runs first to
    // symmetrize asymmetric voice peaks; the drive gain then pushes
    // more material across the threshold so the existing comp curve
    // engages harder and the brickwall limiter below contains the
    // resulting hot peaks. These run independent of the comp's enabled
    // flag — they're a useful pair even when the comp curve itself is
    // bypassed (Drive + Limiter alone is the simplest broadcast PAPR
    // setup). Their work happens *before* the input peak meter so the
    // GR readout reflects the comp's workload at the boosted level.
    if (m_cached.phaseRotatorStages > 0 && m_phaseRotator) {
        m_phaseRotator->process(interleaved, frames, channels);
    }
    if (m_cached.driveLin != 1.0f) {
        const float gain = m_cached.driveLin;
        const int n = frames * channels;
        for (int i = 0; i < n; ++i) interleaved[i] *= gain;
    }

    float inPeakLin  = 0.0f;
    float outPeakLin = 0.0f;
    float worstGrDb  = 0.0f;  // most negative (largest reduction)
    float worstLimGrDb = 0.0f;  // limiter-only GR, most negative
    bool  limFired   = false;

    const float attackCoeff  = m_cached.attackCoeff;
    const float releaseCoeff = m_cached.releaseCoeff;
    const float makeup       = m_cached.makeupLin;
    const float limCeiling   = m_cached.limCeilingLin;
    const float limAttack    = m_cached.limAttackCoeff;
    const float limRelease   = m_cached.limReleaseCoeff;

    for (int f = 0; f < frames; ++f) {
        float l = interleaved[f * channels];
        float r = (channels == 2) ? interleaved[f * channels + 1] : l;

        const float inAbs = std::max(std::fabs(l), std::fabs(r));
        if (inAbs > inPeakLin) inPeakLin = inAbs;

        // Compressor gain (only applied if enabled).  Linear-domain peak
        // envelope: smoothed |x| with asymmetric attack/release, converted
        // to dB once to feed the static curve.  This tracks the actual
        // peak amplitude of the signal (unlike a dB-domain filter that
        // would average log|x| and read ~4 dB below peak on a sine).
        float gainLin = 1.0f;
        if (enabled) {
            const float alpha = (inAbs > m_envLin) ? attackCoeff : releaseCoeff;
            m_envLin += alpha * (inAbs - m_envLin);
            const float envDb = linToDb(std::max(m_envLin, 1e-6f));
            const float gainDb = staticCurveGainDb(envDb);
            if (gainDb < worstGrDb) worstGrDb = gainDb;
            // Auto-makeup tracks Drive (#2887). Without this, dialing
            // Drive up makes the comp do more GR than the fixed Makeup
            // can compensate, and net RMS DROPS. Linking the post-curve
            // gain to Drive matches the broadcast-Optimod model: Drive
            // pushes more material into the curve AND adds equal gain
            // back at the output, so the user's fixed Makeup setting
            // stays a clean "post-everything trim" knob.
            gainLin = dbToLin(gainDb) * makeup * m_cached.driveLin;
        }
        l *= gainLin;
        r *= gainLin;

        // Brickwall peak limiter — feed-forward, fast attack/release,
        // so the envelope sits just above the instantaneous peak when
        // signal exceeds the ceiling.
        if (m_cached.limEnabled) {
            const float maxAbs = std::max(std::fabs(l), std::fabs(r));
            const float over   = maxAbs / std::max(limCeiling, 1e-6f);
            const float target = std::max(1.0f, over);
            const float lc = (target > m_limEnvLin) ? limAttack : limRelease;
            m_limEnvLin += lc * (target - m_limEnvLin);
            // Threshold slightly above 1.0 — the envelope decays
            // asymptotically toward 1.0 in float and never quite
            // reaches it, so a strict `> 1.0f` check would latch the
            // active indicator forever after any trigger.  Anything
            // under 0.005 dB of reduction is inaudible anyway.
            if (m_limEnvLin > 1.0006f) {
                const float reduce = 1.0f / m_limEnvLin;
                l *= reduce;
                r *= reduce;
                limFired = true;
                // Track the worst (most negative) limiter GR this block
                // so the UI can draw a distinct lim-GR indicator.
                const float limGrDb = linToDb(reduce);
                if (limGrDb < worstLimGrDb) worstLimGrDb = limGrDb;
            }
        }

        interleaved[f * channels] = l;
        if (channels == 2) interleaved[f * channels + 1] = r;

        const float outAbs = std::max(std::fabs(l), std::fabs(r));
        if (outAbs > outPeakLin) outPeakLin = outAbs;
    }

    // Publish meter values — audio-thread side of the atomic handoff.
    m_meters.inputPeakDb.store(linToDb(std::max(inPeakLin, 1e-6f)),
                               std::memory_order_relaxed);
    m_meters.outputPeakDb.store(linToDb(std::max(outPeakLin, 1e-6f)),
                                std::memory_order_relaxed);
    m_meters.gainReductionDb.store(worstGrDb, std::memory_order_relaxed);
    m_meters.limiterGrDb.store(worstLimGrDb, std::memory_order_relaxed);
    m_meters.limiterActive.store(limFired, std::memory_order_relaxed);
}

} // namespace AetherSDR
