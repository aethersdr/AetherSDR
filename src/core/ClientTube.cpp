#include "ClientTube.h"

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

// Tilt filter coefficient — simple one-pole LP built from sample rate
// and a corner near 2 kHz.  The tilt is applied as a weighted blend of
// the LP-filtered signal and the original: positive tone emphasises
// the HP component (input - lp), negative emphasises the LP.
constexpr float kToneCornerHz = 2000.0f;
float toneCoeff(double sampleRate) noexcept
{
    const float omega = kTwoPi * kToneCornerHz / static_cast<float>(sampleRate);
    return omega / (omega + 1.0f);
}

} // namespace

ClientTube::ClientTube() = default;

void ClientTube::prepare(double sampleRate)
{
    m_sampleRate = sampleRate;
    reset();
    m_atomics.version.fetch_add(1, std::memory_order_release);
    recacheIfDirty();
}

void ClientTube::setEnabled(bool on) noexcept
{
    m_atomics.enabled.store(on, std::memory_order_release);
}
bool ClientTube::isEnabled() const noexcept
{
    return m_atomics.enabled.load(std::memory_order_acquire);
}

void ClientTube::setModel(Model m) noexcept
{
    m_atomics.model.store(static_cast<uint8_t>(m), std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
ClientTube::Model ClientTube::model() const noexcept
{ return static_cast<Model>(m_atomics.model.load(std::memory_order_relaxed)); }

void ClientTube::setDriveDb(float db) noexcept
{
    m_atomics.driveDb.store(std::clamp(db, 0.0f, 24.0f),
                            std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientTube::driveDb() const noexcept
{ return m_atomics.driveDb.load(std::memory_order_relaxed); }

void ClientTube::setBiasAmount(float v) noexcept
{
    m_atomics.biasAmount.store(std::clamp(v, 0.0f, 1.0f),
                               std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientTube::biasAmount() const noexcept
{ return m_atomics.biasAmount.load(std::memory_order_relaxed); }

void ClientTube::setTone(float v) noexcept
{
    m_atomics.tone.store(std::clamp(v, -1.0f, 1.0f),
                         std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientTube::tone() const noexcept
{ return m_atomics.tone.load(std::memory_order_relaxed); }

void ClientTube::setOutputGainDb(float db) noexcept
{
    m_atomics.outputGainDb.store(std::clamp(db, -24.0f, 12.0f),
                                 std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientTube::outputGainDb() const noexcept
{ return m_atomics.outputGainDb.load(std::memory_order_relaxed); }

void ClientTube::setDryWet(float v) noexcept
{
    m_atomics.dryWet.store(std::clamp(v, 0.0f, 1.0f),
                           std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientTube::dryWet() const noexcept
{ return m_atomics.dryWet.load(std::memory_order_relaxed); }

void ClientTube::setEnvelopeAmount(float v) noexcept
{
    m_atomics.envelopeAmount.store(std::clamp(v, -1.0f, 1.0f),
                                   std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientTube::envelopeAmount() const noexcept
{ return m_atomics.envelopeAmount.load(std::memory_order_relaxed); }

void ClientTube::setAttackMs(float ms) noexcept
{
    m_atomics.attackMs.store(std::clamp(ms, 0.1f, 30.0f),
                             std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientTube::attackMs() const noexcept
{ return m_atomics.attackMs.load(std::memory_order_relaxed); }

void ClientTube::setReleaseMs(float ms) noexcept
{
    m_atomics.releaseMs.store(std::clamp(ms, 10.0f, 500.0f),
                              std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientTube::releaseMs() const noexcept
{ return m_atomics.releaseMs.load(std::memory_order_relaxed); }

void ClientTube::reset() noexcept
{
    m_envLin = 0.0f;
    m_toneStateL = 0.0f;
    m_toneStateR = 0.0f;
}

float ClientTube::inputPeakDb() const noexcept
{ return m_meters.inputPeakDb.load(std::memory_order_relaxed); }
float ClientTube::outputPeakDb() const noexcept
{ return m_meters.outputPeakDb.load(std::memory_order_relaxed); }
float ClientTube::driveAppliedDb() const noexcept
{ return m_meters.driveAppliedDb.load(std::memory_order_relaxed); }

void ClientTube::recacheIfDirty() noexcept
{
    const uint64_t v = m_atomics.version.load(std::memory_order_acquire);
    if (v == m_lastVersion) return;
    m_lastVersion = v;

    m_cached.model       = m_atomics.model.load(std::memory_order_relaxed);
    m_cached.baseDriveLin = dbToLin(
        m_atomics.driveDb.load(std::memory_order_relaxed));
    m_cached.bias        = m_atomics.biasAmount.load(std::memory_order_relaxed);
    m_cached.tone        = m_atomics.tone.load(std::memory_order_relaxed);
    m_cached.outputLin   = dbToLin(
        m_atomics.outputGainDb.load(std::memory_order_relaxed));
    m_cached.dryWet      = m_atomics.dryWet.load(std::memory_order_relaxed);
    m_cached.envAmount   = m_atomics.envelopeAmount.load(std::memory_order_relaxed);
    m_cached.attackCoeff = coeffFromMs(
        m_atomics.attackMs.load(std::memory_order_relaxed), m_sampleRate);
    m_cached.releaseCoeff = coeffFromMs(
        m_atomics.releaseMs.load(std::memory_order_relaxed), m_sampleRate);
    m_cached.toneCoeff = toneCoeff(m_sampleRate);
}

float ClientTube::shape(float x) const noexcept
{
    const float bias = m_cached.bias;
    // Asymmetric term — always non-negative, produces DC offset +
    // even harmonics.  tanh-bounded so it never blows up at high
    // drive; peak contribution capped at ±bias.
    const float asym = bias * std::tanh(x * x);
    switch (m_cached.model) {
        case static_cast<uint8_t>(Model::A):   // soft tanh
            return std::tanh(x) + asym;
        case static_cast<uint8_t>(Model::B): { // hard clip + tanh hybrid
            // Hard clip at ±1 followed by tanh softening — gives a
            // stronger odd-harmonic character than pure tanh.
            const float h = std::clamp(x, -1.0f, 1.0f);
            return std::tanh(h * 1.3f) * 0.85f + asym;
        }
        case static_cast<uint8_t>(Model::C): { // asymmetric-dominant
            // tanh-bounded asymmetric shaper — stronger even-harmonic
            // content than A/B without the unbounded growth of raw
            // x² terms.
            const float t  = std::tanh(x);
            const float a2 = std::tanh(x * x);
            return 0.75f * t + (0.35f + 0.65f * bias) * a2;
        }
    }
    return std::tanh(x);
}

void ClientTube::process(float* interleaved, int frames, int channels) noexcept
{
    if (frames <= 0) return;
    if (channels != 1 && channels != 2) return;

    recacheIfDirty();
    const bool enabled = m_atomics.enabled.load(std::memory_order_acquire);

    float inPeakLin  = 0.0f;
    float outPeakLin = 0.0f;
    float maxDriveLin = 0.0f;

    const float baseDrive    = m_cached.baseDriveLin;
    const float outputLin    = m_cached.outputLin;
    const float dryWet       = m_cached.dryWet;
    const float envAmount    = m_cached.envAmount;
    const float attackCoeff  = m_cached.attackCoeff;
    const float releaseCoeff = m_cached.releaseCoeff;
    const float toneCoef     = m_cached.toneCoeff;
    const float tone         = m_cached.tone;

    for (int f = 0; f < frames; ++f) {
        const float dryL = interleaved[f * channels];
        const float dryR = (channels == 2)
            ? interleaved[f * channels + 1] : dryL;

        const float inAbs = std::max(std::fabs(dryL), std::fabs(dryR));
        if (inAbs > inPeakLin) inPeakLin = inAbs;

        // Envelope follower for dynamic drive.
        const float alpha = (inAbs > m_envLin) ? attackCoeff : releaseCoeff;
        m_envLin += alpha * (inAbs - m_envLin);

        // Effective drive: baseDrive * (1 + envAmount * envLin).
        //   envAmount = 0        → static drive (no modulation)
        //   envAmount = +1       → at full-scale input, drive × 2
        //   envAmount = -1       → at full-scale input, drive × 0
        // Clamped to [0.1, 10] so an aggressive negative envelope at
        // a loud level doesn't zero the drive entirely.
        const float envMod  = 1.0f + envAmount * m_envLin;
        const float drive   = std::clamp(baseDrive * envMod, 0.1f, 10.0f);
        if (drive > maxDriveLin) maxDriveLin = drive;

        // Per-channel tone pre-filter (simple one-pole LP with HP
        // blend).  m_toneState holds the LP output; the shaper sees
        // dry blended toward HP (tone > 0) or LP (tone < 0).
        auto shapeChan = [&](float x, float& state) {
            state += toneCoef * (x - state);
            const float lp = state;
            const float hp = x - state;
            const float shaped = (tone >= 0.0f)
                ? x + tone * (hp - x * 0.5f)      // brighten
                : x + (-tone) * (lp - x * 0.5f);  // darken
            return shape(shaped * drive);
        };

        const float wetL = shapeChan(dryL, m_toneStateL);
        const float wetR = (channels == 2)
            ? shapeChan(dryR, m_toneStateR)
            : wetL;

        // Parallel mix + output trim.  When disabled, pass dry
        // straight through unmodified (not just bypass shape — also
        // skip tone filtering so the signal is bit-identical).
        float outL = dryL, outR = dryR;
        if (enabled) {
            outL = (dryL * (1.0f - dryWet) + wetL * dryWet) * outputLin;
            outR = (dryR * (1.0f - dryWet) + wetR * dryWet) * outputLin;
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
    m_meters.driveAppliedDb.store(linToDb(std::max(maxDriveLin, 1e-6f)),
                                  std::memory_order_relaxed);
}

} // namespace MasterSDR
