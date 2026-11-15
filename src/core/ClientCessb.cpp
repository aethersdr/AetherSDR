#include "core/ClientCessb.h"

#include "aether_wdsp_cessb.h"
#include "core/dsp/FftwPlannerLock.h"

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

ClientCessb::ClientCessb() = default;

ClientCessb::~ClientCessb()
{
    if (m_block) {
        auto lock = fftwPlannerLock();
        aether_cessb_destroy(m_block);
    }
}

void ClientCessb::prepare(double sampleRate)
{
    auto lock = fftwPlannerLock();
    aether_cessb_destroy(m_block);
    m_sampleRate = sampleRate;
    m_block = aether_cessb_create(kBlockSize, static_cast<int>(std::lround(sampleRate)), kTaps,
                                  m_lowHz.load(), m_highHz.load());
    m_latencyFrames.store(m_block ? kBlockSize + aether_cessb_delay(m_block) : 0,
                          std::memory_order_relaxed);
    m_appliedVersion = 0;
    clearFifo();
    m_flushRequested.store(false, std::memory_order_relaxed);
}

void ClientCessb::setEnabled(bool on) noexcept
{
    const bool was = m_enabled.exchange(on, std::memory_order_acq_rel);
    if (on && !was) {
        m_flushRequested.store(true, std::memory_order_release);
    }
}

bool ClientCessb::isEnabled() const noexcept
{
    return m_enabled.load(std::memory_order_acquire);
}

void ClientCessb::setClipDb(float db) noexcept
{
    m_clipDb.store(std::clamp(finiteOr(db, 3.0f), 0.0f, kMaxClipDb), std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

float ClientCessb::clipDb() const noexcept
{
    return m_clipDb.load(std::memory_order_relaxed);
}

void ClientCessb::setOvershootControl(bool on) noexcept
{
    m_overshoot.store(on, std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

bool ClientCessb::overshootControl() const noexcept
{
    return m_overshoot.load(std::memory_order_relaxed);
}

void ClientCessb::setOutputDb(float db) noexcept
{
    m_outputDb.store(std::clamp(finiteOr(db, -1.0f), kMinOutputDb, kMaxOutputDb),
                     std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

float ClientCessb::outputDb() const noexcept
{
    return m_outputDb.load(std::memory_order_relaxed);
}

void ClientCessb::setPassband(float lowHz, float highHz) noexcept
{
    const float lo = std::clamp(finiteOr(lowHz, kDefaultLowHz), 0.0f, 1000.0f);
    const float hi = std::clamp(finiteOr(highHz, kDefaultHighHz), lo + 500.0f, 4000.0f);
    m_lowHz.store(lo, std::memory_order_relaxed);
    m_highHz.store(hi, std::memory_order_relaxed);
}

float ClientCessb::lowHz() const noexcept
{
    return m_lowHz.load(std::memory_order_relaxed);
}

float ClientCessb::highHz() const noexcept
{
    return m_highHz.load(std::memory_order_relaxed);
}

void ClientCessb::reset() noexcept
{
    m_flushRequested.store(true, std::memory_order_release);
}

int ClientCessb::latencyFrames() const noexcept
{
    return m_latencyFrames.load(std::memory_order_relaxed);
}

void ClientCessb::clearFifo() noexcept
{
    std::fill(std::begin(m_inBlock), std::end(m_inBlock), 0.0f);
    std::fill(std::begin(m_outBlock), std::end(m_outBlock), 0.0f);
    m_fifoPos = 0;
}

void ClientCessb::process(float* interleaved, int frames, int channels) noexcept
{
    if (!interleaved || frames <= 0 || channels < 1 || channels > 2 || !m_block) {
        return;
    }
    const uint64_t version = m_version.load(std::memory_order_acquire);
    if (version != m_appliedVersion) {
        m_appliedVersion = version;
        aether_cessb_set_clip_gain_db(m_block, m_clipDb.load(std::memory_order_relaxed));
        aether_cessb_set_overshoot_control(m_block, m_overshoot.load(std::memory_order_relaxed));
        m_outputGain = std::pow(10.0f, m_outputDb.load(std::memory_order_relaxed) / 20.0f);
    }
    if (m_flushRequested.exchange(false, std::memory_order_acq_rel)) {
        aether_cessb_flush(m_block);
        clearFifo();
    }
    double* buffer = aether_cessb_buffer(m_block);
    for (int f = 0; f < frames; ++f) {
        m_inBlock[m_fifoPos] = interleaved[f * channels];
        const float y = m_outBlock[m_fifoPos];
        if (++m_fifoPos == kBlockSize) {
            for (int i = 0; i < kBlockSize; ++i) {
                buffer[2 * i] = m_inBlock[i];
                buffer[2 * i + 1] = 0.0;
            }
            aether_cessb_process(m_block);
            for (int i = 0; i < kBlockSize; ++i) {
                m_outBlock[i] = static_cast<float>(buffer[2 * i]) * m_outputGain;
            }
            m_fifoPos = 0;
        }
        interleaved[f * channels] = y;
        if (channels == 2) {
            interleaved[f * channels + 1] = y;
        }
    }
}

}  // namespace AetherSDR
