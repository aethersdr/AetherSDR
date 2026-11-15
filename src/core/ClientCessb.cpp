#include "core/ClientCessb.h"

#include "aether_wdsp_cessb.h"
#include "core/dsp/FftwPlannerLock.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

std::atomic<int> g_liveBlocks{0};

float finiteOr(float v, float fallback) noexcept
{
    return std::isfinite(v) ? v : fallback;
}

}  // namespace

ClientCessb::ClientCessb() = default;

ClientCessb::~ClientCessb()
{
    destroyBlock(m_active);
    destroyBlock(m_next);
    destroyBlock(m_pending.exchange(nullptr));
    collectRetiredBlocks();
}

AetherCessb* ClientCessb::buildBlock(float lowHz, float highHz) const
{
    if (m_sampleRate <= 0.0) {
        return nullptr;
    }
    auto lock = fftwPlannerLock();
    AetherCessb* block = aether_cessb_create(kBlockSize, static_cast<int>(std::lround(m_sampleRate)),
                                             kTaps, lowHz, highHz);
    if (block) {
        g_liveBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    return block;
}

void ClientCessb::destroyBlock(AetherCessb* block)
{
    if (!block) {
        return;
    }
    auto lock = fftwPlannerLock();
    aether_cessb_destroy(block);
    g_liveBlocks.fetch_sub(1, std::memory_order_relaxed);
}

int ClientCessb::liveBlockCount() noexcept
{
    return g_liveBlocks.load(std::memory_order_relaxed);
}

void ClientCessb::collectRetiredBlocks()
{
    for (auto& slot : m_retired) {
        destroyBlock(slot.exchange(nullptr, std::memory_order_acq_rel));
    }
}

bool ClientCessb::retire(AetherCessb* block) noexcept
{
    for (auto& slot : m_retired) {
        AetherCessb* expected = nullptr;
        if (slot.compare_exchange_strong(expected, block, std::memory_order_acq_rel)) {
            return true;
        }
    }
    return false;
}

bool ClientCessb::hasRetireSlot() const noexcept
{
    for (const auto& slot : m_retired) {
        if (slot.load(std::memory_order_acquire) == nullptr) {
            return true;
        }
    }
    return false;
}

void ClientCessb::prepare(double sampleRate)
{
    destroyBlock(m_active);
    destroyBlock(m_next);
    destroyBlock(m_pending.exchange(nullptr));
    collectRetiredBlocks();
    m_next = nullptr;
    m_sampleRate = sampleRate;
    m_active = buildBlock(m_lowHz.load(), m_highHz.load());
    m_latencyFrames.store(m_active ? kBlockSize + aether_cessb_delay(m_active) : 0,
                          std::memory_order_relaxed);
    m_appliedVersion = 0;
    clearFifo();
    m_flushRequested.store(false, std::memory_order_relaxed);
}

void ClientCessb::setEnabled(bool on) noexcept
{
    const bool was = m_enabled.exchange(on, std::memory_order_acq_rel);
    if (on && !was) {
        // The FIFO and the filters still hold audio from the last run.
        m_flushRequested.store(true, std::memory_order_release);
    }
}

bool ClientCessb::isEnabled() const noexcept
{
    return m_enabled.load(std::memory_order_acquire);
}

void ClientCessb::setClipDb(float db) noexcept
{
    m_clipDb.store(std::clamp(finiteOr(db, kDefaultClipDb), 0.0f, kMaxClipDb),
                   std::memory_order_relaxed);
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
    m_outputDb.store(std::clamp(finiteOr(db, kDefaultOutputDb), kMinOutputDb, kMaxOutputDb),
                     std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

float ClientCessb::outputDb() const noexcept
{
    return m_outputDb.load(std::memory_order_relaxed);
}

void ClientCessb::setPassband(float lowHz, float highHz)
{
    collectRetiredBlocks();
    float lo = std::clamp(finiteOr(lowHz, kDefaultLowHz), kMinPassbandHz,
                          kMaxPassbandHz - kMinWidthHz);
    float hi = std::clamp(finiteOr(highHz, kDefaultHighHz), lo + kMinWidthHz, kMaxPassbandHz);
    lo = std::round(lo);
    hi = std::round(hi);
    if (lo == m_lowHz.load() && hi == m_highHz.load()) {
        return;
    }
    m_lowHz.store(lo, std::memory_order_relaxed);
    m_highHz.store(hi, std::memory_order_relaxed);
    AetherCessb* block = buildBlock(lo, hi);
    if (!block) {
        return;  // not prepared yet: prepare() builds at this passband
    }
    destroyBlock(m_pending.exchange(block, std::memory_order_acq_rel));
}

void ClientCessb::followRadioFilter(int radioLowHz, int radioHighHz)
{
    if (radioHighHz <= radioLowHz) {
        return;
    }
    setPassband(static_cast<float>(radioLowHz) + kRadioFilterMarginHz,
                static_cast<float>(radioHighHz) - kRadioFilterMarginHz);
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

float ClientCessb::takeEnvelopePeakDb() noexcept
{
    const float peak = m_envelopePeak.exchange(0.0f, std::memory_order_relaxed);
    return peak > 1e-6f ? 20.0f * std::log10(peak) : -120.0f;
}

void ClientCessb::applyParameters(AetherCessb* block) noexcept
{
    if (block) {
        aether_cessb_set_clip_gain_db(block, m_clipDb.load(std::memory_order_relaxed));
        aether_cessb_set_overshoot_control(block, m_overshoot.load(std::memory_order_relaxed));
    }
}

void ClientCessb::clearFifo() noexcept
{
    m_inBlock.fill(0.0f);
    m_outBlock.fill(0.0f);
    m_fifoPos = 0;
}

void ClientCessb::runBlock() noexcept
{
    double* a = aether_cessb_buffer(m_active);
    for (int i = 0; i < kBlockSize; ++i) {
        a[2 * i] = m_inBlock[static_cast<std::size_t>(i)];
        a[2 * i + 1] = 0.0;
    }
    aether_cessb_process(m_active);

    double* b = nullptr;
    int nextDelay = 0;
    if (m_next) {
        b = aether_cessb_buffer(m_next);
        for (int i = 0; i < kBlockSize; ++i) {
            b[2 * i] = m_inBlock[static_cast<std::size_t>(i)];
            b[2 * i + 1] = 0.0;
        }
        aether_cessb_process(m_next);
        nextDelay = aether_cessb_delay(m_next);
    }

    float peak = m_envelopePeak.load(std::memory_order_relaxed);
    for (int i = 0; i < kBlockSize; ++i) {
        double re = a[2 * i];
        double im = a[2 * i + 1];
        if (b && m_nextSamples >= 2 * nextDelay) {
            // The new block has filled its filters; crossfade to it.
            const double w = std::min(
                1.0, static_cast<double>(m_nextSamples - 2 * nextDelay) / kCrossfadeSamples);
            re = (1.0 - w) * re + w * b[2 * i];
            im = (1.0 - w) * im + w * b[2 * i + 1];
        }
        if (b) {
            ++m_nextSamples;
        }
        peak = std::max(peak, static_cast<float>(std::hypot(re, im)) * m_outputGain);
        m_outBlock[static_cast<std::size_t>(i)] = static_cast<float>(re) * m_outputGain;
    }
    m_envelopePeak.store(peak, std::memory_order_relaxed);

    if (m_next && m_nextSamples >= 2 * nextDelay + kCrossfadeSamples && retire(m_active)) {
        m_active = m_next;
        m_next = nullptr;
    }
}

void ClientCessb::process(float* interleaved, int frames, int channels) noexcept
{
    if (!interleaved || frames <= 0 || channels < 1 || channels > 2 || !m_active) {
        return;
    }
    const uint64_t version = m_version.load(std::memory_order_acquire);
    if (version != m_appliedVersion) {
        m_appliedVersion = version;
        applyParameters(m_active);
        applyParameters(m_next);
        m_outputGain = std::pow(10.0f, m_outputDb.load(std::memory_order_relaxed) / 20.0f);
    }
    // A new passband. Take it only while a slot is free to hand back the block
    // it replaces: nothing is freed here.
    if (m_pending.load(std::memory_order_acquire) != nullptr && hasRetireSlot()) {
        if (AetherCessb* block = m_pending.exchange(nullptr, std::memory_order_acq_rel)) {
            if (m_next && !retire(m_next)) {
                m_pending.store(block, std::memory_order_release);
            } else {
                m_next = block;
                m_nextSamples = 0;
                applyParameters(m_next);
            }
        }
    }
    if (m_flushRequested.exchange(false, std::memory_order_acq_rel)) {
        if (m_next && retire(m_active)) {
            m_active = m_next;
            m_next = nullptr;
        }
        aether_cessb_flush(m_active);
        if (m_next) {
            aether_cessb_flush(m_next);
            m_nextSamples = 0;
        }
        clearFifo();
    }

    for (int f = 0; f < frames; ++f) {
        m_inBlock[static_cast<std::size_t>(m_fifoPos)] = interleaved[f * channels];
        const float y = m_outBlock[static_cast<std::size_t>(m_fifoPos)];
        if (++m_fifoPos == kBlockSize) {
            runBlock();
            m_fifoPos = 0;
        }
        interleaved[f * channels] = y;
        if (channels == 2) {
            interleaved[f * channels + 1] = y;
        }
    }
}

}  // namespace AetherSDR
