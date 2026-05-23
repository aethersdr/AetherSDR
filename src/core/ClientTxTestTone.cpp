#include "ClientTxTestTone.h"

#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {
constexpr float kTwoPi = 6.28318530717958647692f;
}

ClientTxTestTone::ClientTxTestTone() = default;

void ClientTxTestTone::prepare(double sampleRate)
{
    m_sampleRate = sampleRate > 0 ? sampleRate : 24000.0;
    recacheIfDirty();
    reset();
}

void ClientTxTestTone::setEnabled(bool on) noexcept
{
    m_atomics.enabled.store(on, std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

bool ClientTxTestTone::isEnabled() const noexcept
{
    return m_atomics.enabled.load(std::memory_order_relaxed);
}

void ClientTxTestTone::setFrequencyHz(float hz) noexcept
{
    m_atomics.freqHz.store(std::clamp(hz, 50.0f, 5000.0f),
                           std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

float ClientTxTestTone::frequencyHz() const noexcept
{
    return m_atomics.freqHz.load(std::memory_order_relaxed);
}

void ClientTxTestTone::setLevelDb(float db) noexcept
{
    m_atomics.levelDb.store(std::clamp(db, -60.0f, 0.0f),
                            std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

float ClientTxTestTone::levelDb() const noexcept
{
    return m_atomics.levelDb.load(std::memory_order_relaxed);
}

void ClientTxTestTone::reset() noexcept
{
    m_phase = 0.0f;
}

void ClientTxTestTone::recacheIfDirty() noexcept
{
    const uint64_t v = m_atomics.version.load(std::memory_order_acquire);
    if (v == m_cachedVersion) return;
    m_cached.enabled = m_atomics.enabled.load(std::memory_order_relaxed);
    const float hz   = m_atomics.freqHz.load(std::memory_order_relaxed);
    const float db   = m_atomics.levelDb.load(std::memory_order_relaxed);
    m_cached.phaseInc = kTwoPi * hz / static_cast<float>(m_sampleRate);
    m_cached.ampLin   = std::pow(10.0f, db / 20.0f);
    m_cachedVersion = v;
}

void ClientTxTestTone::process(int16_t* interleaved, int frames, int channels) noexcept
{
    if (!interleaved || frames <= 0 || channels < 1 || channels > 2) return;
    recacheIfDirty();
    if (!m_cached.enabled) return;

    const float inc = m_cached.phaseInc;
    const float amp = m_cached.ampLin;
    for (int f = 0; f < frames; ++f) {
        const float s = std::sin(m_phase) * amp;
        const int16_t v = static_cast<int16_t>(
            std::clamp(s * 32767.0f, -32768.0f, 32767.0f));
        interleaved[f * channels] = v;
        if (channels == 2) interleaved[f * channels + 1] = v;
        m_phase += inc;
        if (m_phase > kTwoPi) m_phase -= kTwoPi;
    }
}

} // namespace MasterSDR
