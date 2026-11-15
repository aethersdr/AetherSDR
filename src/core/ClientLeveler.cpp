#include "core/ClientLeveler.h"

#include "aether_wdsp_leveler.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace AetherSDR {

namespace {

float finiteOr(float v, float fallback) noexcept
{
    return std::isfinite(v) ? v : fallback;
}

}  // namespace

ClientLeveler::ClientLeveler() = default;

ClientLeveler::~ClientLeveler()
{
    aether_leveler_destroy(m_block);
}

void ClientLeveler::prepare(double sampleRate)
{
    aether_leveler_destroy(m_block);
    m_sampleRate = sampleRate;
    m_block = aether_leveler_create(kBlockSize, static_cast<int>(std::lround(sampleRate)));
    m_appliedVersion = 0;
    applyParameters();
    clearFifo();
    m_flushRequested.store(false, std::memory_order_relaxed);
}

void ClientLeveler::setEnabled(bool on) noexcept
{
    const bool was = m_enabled.exchange(on, std::memory_order_acq_rel);
    if (on && !was) {
        // The FIFO and the look-ahead ring still hold audio from the last run.
        m_flushRequested.store(true, std::memory_order_release);
    }
}

bool ClientLeveler::isEnabled() const noexcept
{
    return m_enabled.load(std::memory_order_acquire);
}

void ClientLeveler::setMaxGainDb(float db) noexcept
{
    m_maxGainDb.store(std::clamp(finiteOr(db, kDefaultMaxGainDb), 0.0f, kMaxGainDb),
                      std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

float ClientLeveler::maxGainDb() const noexcept
{
    return m_maxGainDb.load(std::memory_order_relaxed);
}

void ClientLeveler::setAttackMs(float ms) noexcept
{
    m_attackMs.store(std::clamp(finiteOr(ms, kDefaultAttackMs), kMinAttackMs, kMaxAttackMs),
                     std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

float ClientLeveler::attackMs() const noexcept
{
    return m_attackMs.load(std::memory_order_relaxed);
}

void ClientLeveler::setDecayMs(float ms) noexcept
{
    m_decayMs.store(std::clamp(finiteOr(ms, kDefaultDecayMs), kMinDecayMs, kMaxDecayMs),
                    std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

float ClientLeveler::decayMs() const noexcept
{
    return m_decayMs.load(std::memory_order_relaxed);
}

void ClientLeveler::setHangMs(float ms) noexcept
{
    const float v = finiteOr(ms, 0.0f);
    // Below the shortest hang the knob reads Off.
    m_hangMs.store(v < kMinHangMs ? 0.0f : std::min(v, kMaxHangMs), std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

float ClientLeveler::hangMs() const noexcept
{
    return m_hangMs.load(std::memory_order_relaxed);
}

void ClientLeveler::reset() noexcept
{
    m_flushRequested.store(true, std::memory_order_release);
}

int ClientLeveler::latencyFrames() const noexcept
{
    return m_latencyFrames.load(std::memory_order_relaxed);
}

float ClientLeveler::gainDb() const noexcept
{
    return m_gainDb.load(std::memory_order_relaxed);
}

void ClientLeveler::applyParameters() noexcept
{
    if (!m_block) {
        return;
    }
    aether_leveler_set_max_gain_db(m_block, m_maxGainDb.load(std::memory_order_relaxed));
    aether_leveler_set_attack_ms(m_block, m_attackMs.load(std::memory_order_relaxed));
    aether_leveler_set_decay_ms(m_block, m_decayMs.load(std::memory_order_relaxed));
    aether_leveler_set_hang_ms(m_block, m_hangMs.load(std::memory_order_relaxed));
    m_latencyFrames.store(kBlockSize + aether_leveler_lookahead(m_block),
                          std::memory_order_relaxed);
}

void ClientLeveler::clearFifo() noexcept
{
    std::fill(std::begin(m_inBlock), std::end(m_inBlock), 0.0f);
    std::fill(std::begin(m_outBlock), std::end(m_outBlock), 0.0f);
    m_fifoPos = 0;
}

void ClientLeveler::process(float* interleaved, int frames, int channels) noexcept
{
    if (!interleaved || frames <= 0 || channels < 1 || channels > 2 || !m_block) {
        return;
    }
    const uint64_t version = m_version.load(std::memory_order_acquire);
    if (version != m_appliedVersion) {
        m_appliedVersion = version;
        applyParameters();
    }
    if (m_flushRequested.exchange(false, std::memory_order_acq_rel)) {
        aether_leveler_flush(m_block);
        clearFifo();
    }

    double* buffer = aether_leveler_buffer(m_block);
    for (int f = 0; f < frames; ++f) {
        m_inBlock[m_fifoPos] = interleaved[f * channels];
        const float y = m_outBlock[m_fifoPos];
        if (++m_fifoPos == kBlockSize) {
            for (int i = 0; i < kBlockSize; ++i) {
                buffer[2 * i] = m_inBlock[i];
                buffer[2 * i + 1] = 0.0;
            }
            aether_leveler_process(m_block);
            for (int i = 0; i < kBlockSize; ++i) {
                m_outBlock[i] = static_cast<float>(buffer[2 * i]);
            }
            m_fifoPos = 0;
        }
        interleaved[f * channels] = y;
        if (channels == 2) {
            interleaved[f * channels + 1] = y;
        }
    }
    m_gainDb.store(static_cast<float>(aether_leveler_gain_db(m_block)), std::memory_order_relaxed);
}

}  // namespace AetherSDR
