#include "ClientPhaseRotator.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {
constexpr float kMinusInfDb = -120.0f;

float linToDb(float lin) noexcept
{
    if (lin <= 1e-7f) return kMinusInfDb;
    return 20.0f * std::log10(lin);
}
} // namespace

ClientPhaseRotator::ClientPhaseRotator()
{
    designSections();
}

void ClientPhaseRotator::prepare(double sampleRate)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 24000.0;
    designSections();
    reset();
}

void ClientPhaseRotator::setEnabled(bool on) noexcept
{
    m_atomics.enabled.store(on, std::memory_order_release);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

bool ClientPhaseRotator::isEnabled() const noexcept
{
    return m_atomics.enabled.load(std::memory_order_acquire);
}

void ClientPhaseRotator::setStages(int stages) noexcept
{
    const int clamped = std::clamp(stages, 0, kMaxStages);
    m_atomics.stages.store(clamped, std::memory_order_release);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

int ClientPhaseRotator::stages() const noexcept
{
    return m_atomics.stages.load(std::memory_order_acquire);
}

float ClientPhaseRotator::inputPosPeakDb()  const noexcept { return m_meters.inPosDb.load(std::memory_order_relaxed); }
float ClientPhaseRotator::inputNegPeakDb()  const noexcept { return m_meters.inNegDb.load(std::memory_order_relaxed); }
float ClientPhaseRotator::outputPosPeakDb() const noexcept { return m_meters.outPosDb.load(std::memory_order_relaxed); }
float ClientPhaseRotator::outputNegPeakDb() const noexcept { return m_meters.outNegDb.load(std::memory_order_relaxed); }

void ClientPhaseRotator::reset() noexcept
{
    for (auto& s : m_sections) {
        s.x1[0] = s.x1[1] = 0.0f;
        s.x2[0] = s.x2[1] = 0.0f;
        s.y1[0] = s.y1[1] = 0.0f;
        s.y2[0] = s.y2[1] = 0.0f;
    }
}

void ClientPhaseRotator::designSections() noexcept
{
    // Second-order all-pass coefficients per RBJ cookbook, but the
    // canonical AP form whose magnitude is exactly 1 at every frequency.
    // For a centre frequency f0 and Q, the pole/zero pair sits on a
    // common radius with mirrored arguments — the standard analog-to-
    // digital bilinear-transform derivation gives:
    //   w0 = 2π f0 / fs
    //   alpha = sin(w0) / (2 Q)
    //   a0 = 1 + alpha,  a1 = -2 cos(w0),  a2 = 1 - alpha
    //   b0 = a2,         b1 = a1,          b2 = a0
    // After normalising by a0, the all-pass numerator coefficients equal
    // the reversed denominator coefficients — which is the standard form
    // captured in the AllPass struct comment.
    const double fs = m_sampleRate > 0.0 ? m_sampleRate : 24000.0;
    for (int i = 0; i < kMaxStages; ++i) {
        const double f0 = kCentreHz[i];
        constexpr double kPi = 3.14159265358979323846;
        const double w0 = 2.0 * kPi * f0 / fs;
        const double alpha = std::sin(w0) / (2.0 * kQ);
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * std::cos(w0);
        const double a2 = 1.0 - alpha;
        m_sections[i].a1 = static_cast<float>(a1 / a0);
        m_sections[i].a2 = static_cast<float>(a2 / a0);
    }
}

void ClientPhaseRotator::recacheIfDirty() noexcept
{
    const uint64_t v = m_atomics.version.load(std::memory_order_acquire);
    if (v == m_lastVersion) return;
    m_cachedStages = std::clamp(m_atomics.stages.load(std::memory_order_acquire),
                                0, kMaxStages);
    m_lastVersion = v;
}

void ClientPhaseRotator::process(float* interleaved, int frames, int channels) noexcept
{
    if (!isEnabled() || frames <= 0 || channels < 1 || channels > 2) return;
    recacheIfDirty();
    if (m_cachedStages <= 0) return;

    // Block-level peak tracking, signed — record max positive sample and
    // most negative sample at both input and output. Asymmetry meter
    // visualises peakPos vs |peakNeg|; phase rotation should make those
    // values converge.
    float inPos = 0.0f, inNeg = 0.0f, outPos = 0.0f, outNeg = 0.0f;

    for (int i = 0; i < frames; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            const float x = interleaved[i * channels + ch];
            if (x > inPos)  inPos = x;
            if (x < inNeg)  inNeg = x;

            float v = x;
            for (int s = 0; s < m_cachedStages; ++s) {
                auto& f = m_sections[s];
                // Direct Form I all-pass:
                //   y[n] = a2*x[n] + a1*x[n-1] + x[n-2] - a1*y[n-1] - a2*y[n-2]
                const float y = f.a2 * v
                              + f.a1 * f.x1[ch]
                              +        f.x2[ch]
                              - f.a1 * f.y1[ch]
                              - f.a2 * f.y2[ch];
                f.x2[ch] = f.x1[ch];
                f.x1[ch] = v;
                f.y2[ch] = f.y1[ch];
                f.y1[ch] = y;
                v = y;
            }
            interleaved[i * channels + ch] = v;

            if (v > outPos) outPos = v;
            if (v < outNeg) outNeg = v;
        }
    }

    m_meters.inPosDb.store(linToDb(inPos),         std::memory_order_relaxed);
    m_meters.inNegDb.store(linToDb(-inNeg),        std::memory_order_relaxed);
    m_meters.outPosDb.store(linToDb(outPos),       std::memory_order_relaxed);
    m_meters.outNegDb.store(linToDb(-outNeg),      std::memory_order_relaxed);
}

} // namespace AetherSDR
