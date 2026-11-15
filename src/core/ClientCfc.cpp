#include "core/ClientCfc.h"

#include "aether_wdsp_cfc.h"
#include "core/dsp/FftwPlannerLock.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

std::atomic<int> g_liveBlocks{0};

float linToDb(float v) noexcept
{
    return v > 1e-6f ? 20.0f * std::log10(v) : -120.0f;
}

float finiteOr(float v, float fallback) noexcept
{
    return std::isfinite(v) ? v : fallback;
}

}  // namespace

std::vector<ClientCfc::Band> ClientCfc::defaultBands()
{
    return {{200.0f, 0.0f, 7.0f},
            {1000.0f, 5.0f, 7.0f},
            {2000.0f, 10.0f, 7.0f},
            {3000.0f, 10.0f, 7.0f},
            {4000.0f, 5.0f, 7.0f}};
}

int ClientCfc::snapFftSize(int size) noexcept
{
    int best = kMinFftSize;
    for (int candidate = kMinFftSize; candidate <= kMaxFftSize; candidate *= 2) {
        // Nearest in the log domain, so 1536 sits halfway between 1024 and 2048.
        if (std::abs(std::log2(static_cast<double>(std::max(size, 1)))
                     - std::log2(static_cast<double>(candidate)))
            < std::abs(std::log2(static_cast<double>(std::max(size, 1)))
                       - std::log2(static_cast<double>(best)))) {
            best = candidate;
        }
    }
    return best;
}

ClientCfc::ClientCfc()
{
    setBands(defaultBands());
}

ClientCfc::~ClientCfc()
{
    destroyBlock(m_active);
    destroyBlock(m_next);
    destroyBlock(m_pending.exchange(nullptr));
    collectRetiredBlocks();
}

AetherCfc* ClientCfc::buildBlock(int fftSize) const
{
    const double rate = m_sampleRate.load(std::memory_order_acquire);
    if (rate <= 0.0) {
        return nullptr;
    }
    auto lock = fftwPlannerLock();
    AetherCfc* block = aether_cfc_create(fftSize, kBlockSize, static_cast<int>(std::lround(rate)));
    if (block) {
        g_liveBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    return block;
}

void ClientCfc::destroyBlock(AetherCfc* block)
{
    if (!block) {
        return;
    }
    auto lock = fftwPlannerLock();
    aether_cfc_destroy(block);
    g_liveBlocks.fetch_sub(1, std::memory_order_relaxed);
}

int ClientCfc::liveBlockCount() noexcept
{
    return g_liveBlocks.load(std::memory_order_relaxed);
}

void ClientCfc::collectRetiredBlocks()
{
    for (auto& slot : m_retired) {
        destroyBlock(slot.exchange(nullptr, std::memory_order_acq_rel));
    }
}

bool ClientCfc::retire(AetherCfc* block) noexcept
{
    for (auto& slot : m_retired) {
        AetherCfc* expected = nullptr;
        if (slot.compare_exchange_strong(expected, block, std::memory_order_acq_rel)) {
            return true;
        }
    }
    return false;
}

bool ClientCfc::hasRetireSlot() const noexcept
{
    for (const auto& slot : m_retired) {
        if (slot.load(std::memory_order_acquire) == nullptr) {
            return true;
        }
    }
    return false;
}

void ClientCfc::prepare(double sampleRate)
{
    destroyBlock(m_active);
    destroyBlock(m_next);
    destroyBlock(m_pending.exchange(nullptr));
    collectRetiredBlocks();
    m_next = nullptr;
    m_sampleRate.store(sampleRate, std::memory_order_release);
    m_active = buildBlock(m_fftSize.load(std::memory_order_relaxed));
    m_activeFftSize.store(m_active ? aether_cfc_fft_size(m_active) : 0,
                          std::memory_order_relaxed);
    m_appliedVersion = 0;
    clearFifo();
    m_flushRequested.store(false, std::memory_order_relaxed);
}

void ClientCfc::setEnabled(bool on) noexcept
{
    const bool was = m_enabled.exchange(on, std::memory_order_acq_rel);
    if (on && !was) {
        // The FIFO and the block still hold audio from the last time it ran.
        m_flushRequested.store(true, std::memory_order_release);
    }
}

bool ClientCfc::isEnabled() const noexcept
{
    return m_enabled.load(std::memory_order_acquire);
}

void ClientCfc::setFftSize(int size)
{
    collectRetiredBlocks();
    const int snapped = snapFftSize(size);
    if (snapped == m_fftSize.exchange(snapped, std::memory_order_acq_rel)) {
        return;
    }
    AetherCfc* block = buildBlock(snapped);
    if (!block) {
        return;  // not prepared yet: prepare() builds at m_fftSize
    }
    destroyBlock(m_pending.exchange(block, std::memory_order_acq_rel));
}

int ClientCfc::fftSize() const noexcept
{
    return m_fftSize.load(std::memory_order_relaxed);
}

void ClientCfc::bumpVersion() noexcept
{
    m_version.fetch_add(1, std::memory_order_release);
}

void ClientCfc::setPrecompDb(float db) noexcept
{
    m_precompDb.store(std::clamp(finiteOr(db, 0.0f), 0.0f, kMaxPrecompDb),
                      std::memory_order_relaxed);
    bumpVersion();
}

float ClientCfc::precompDb() const noexcept
{
    return m_precompDb.load(std::memory_order_relaxed);
}

void ClientCfc::setPostEqEnabled(bool on) noexcept
{
    m_postEq.store(on, std::memory_order_relaxed);
    bumpVersion();
}

bool ClientCfc::postEqEnabled() const noexcept
{
    return m_postEq.load(std::memory_order_relaxed);
}

void ClientCfc::setPostGainDb(float db) noexcept
{
    m_postGainDb.store(std::clamp(finiteOr(db, 0.0f), -kMaxPostGainDb, kMaxPostGainDb),
                       std::memory_order_relaxed);
    bumpVersion();
}

float ClientCfc::postGainDb() const noexcept
{
    return m_postGainDb.load(std::memory_order_relaxed);
}

void ClientCfc::setBands(std::vector<Band> bands) noexcept
{
    for (Band& b : bands) {
        b.hz = std::clamp(finiteOr(b.hz, 1000.0f), kMinHz, kMaxHz);
        b.compDb = std::clamp(finiteOr(b.compDb, 0.0f), 0.0f, kMaxCompDb);
        b.eqDb = std::clamp(finiteOr(b.eqDb, 0.0f), -kMaxEqDb, kMaxEqDb);
    }
    if (static_cast<int>(bands.size()) < kMinBands) {
        bands = defaultBands();
    }
    if (static_cast<int>(bands.size()) > kMaxBands) {
        bands.resize(kMaxBands);
    }
    std::stable_sort(bands.begin(), bands.end(),
                     [](const Band& a, const Band& b) { return a.hz < b.hz; });
    for (std::size_t i = 0; i < bands.size(); ++i) {
        m_bandValues[i * 3 + 0].store(bands[i].hz, std::memory_order_relaxed);
        m_bandValues[i * 3 + 1].store(bands[i].compDb, std::memory_order_relaxed);
        m_bandValues[i * 3 + 2].store(bands[i].eqDb, std::memory_order_relaxed);
    }
    m_bandCount.store(static_cast<int>(bands.size()), std::memory_order_relaxed);
    bumpVersion();
}

std::vector<ClientCfc::Band> ClientCfc::bands() const
{
    const int count = m_bandCount.load(std::memory_order_relaxed);
    std::vector<Band> out(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        out[static_cast<std::size_t>(i)] = {
            m_bandValues[static_cast<std::size_t>(i) * 3 + 0].load(std::memory_order_relaxed),
            m_bandValues[static_cast<std::size_t>(i) * 3 + 1].load(std::memory_order_relaxed),
            m_bandValues[static_cast<std::size_t>(i) * 3 + 2].load(std::memory_order_relaxed)};
    }
    return out;
}

void ClientCfc::reset() noexcept
{
    m_flushRequested.store(true, std::memory_order_release);
}

int ClientCfc::latencyFrames() const noexcept
{
    return m_activeFftSize.load(std::memory_order_relaxed);
}

float ClientCfc::inputPeakDb() const noexcept
{
    return m_inputPeakDb.load(std::memory_order_relaxed);
}

float ClientCfc::outputPeakDb() const noexcept
{
    return m_outputPeakDb.load(std::memory_order_relaxed);
}

void ClientCfc::setCompressionDisplayEnabled(bool on) noexcept
{
    m_displayEnabled.store(on, std::memory_order_relaxed);
}

bool ClientCfc::compressionDisplay(std::array<float, kDisplayPoints>& out) const noexcept
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t before = m_displaySeq.load(std::memory_order_acquire);
        if (before == 0) {
            return false;  // nothing published yet
        }
        if (before & 1u) {
            continue;
        }
        for (int i = 0; i < kDisplayPoints; ++i) {
            out[static_cast<std::size_t>(i)] =
                m_display[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m_displaySeq.load(std::memory_order_relaxed) == before) {
            return true;
        }
    }
    return false;
}

void ClientCfc::applyParameters(AetherCfc* block) noexcept
{
    if (!block) {
        return;
    }
    const int count = m_bandCount.load(std::memory_order_relaxed);
    std::array<double, kMaxBands> hz{};
    std::array<double, kMaxBands> comp{};
    std::array<double, kMaxBands> eq{};
    for (int i = 0; i < count; ++i) {
        const auto base = static_cast<std::size_t>(i) * 3;
        hz[static_cast<std::size_t>(i)] = m_bandValues[base + 0].load(std::memory_order_relaxed);
        comp[static_cast<std::size_t>(i)] = m_bandValues[base + 1].load(std::memory_order_relaxed);
        eq[static_cast<std::size_t>(i)] = m_bandValues[base + 2].load(std::memory_order_relaxed);
    }
    aether_cfc_set_profile(block, count, hz.data(), comp.data(), eq.data());
    aether_cfc_set_precomp_db(block, m_precompDb.load(std::memory_order_relaxed));
    aether_cfc_set_post_eq(block, m_postEq.load(std::memory_order_relaxed) ? 1 : 0);
    aether_cfc_set_post_gain_db(block, m_postGainDb.load(std::memory_order_relaxed));
}

void ClientCfc::clearFifo() noexcept
{
    m_inBlock.fill(0.0f);
    m_outBlock.fill(0.0f);
    m_fifoPos = 0;
}

void ClientCfc::recacheIfDirty() noexcept
{
    const uint64_t version = m_version.load(std::memory_order_acquire);
    if (version != m_appliedVersion) {
        m_appliedVersion = version;
        applyParameters(m_active);
        applyParameters(m_next);
    }

    // A new FFT size. Take it only while a slot is free to hand the block it
    // replaces back to the owner thread: nothing is freed here.
    if (m_pending.load(std::memory_order_acquire) != nullptr && hasRetireSlot()) {
        AetherCfc* block = m_pending.exchange(nullptr, std::memory_order_acq_rel);
        if (block) {
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
        // Starting from silence anyway, so a waiting block takes over at once.
        if (m_next && retire(m_active)) {
            m_active = m_next;
            m_next = nullptr;
        }
        aether_cfc_flush(m_active);
        if (m_next) {
            aether_cfc_flush(m_next);
            m_nextSamples = 0;
        }
        clearFifo();
    }
    m_activeFftSize.store(aether_cfc_fft_size(m_active), std::memory_order_relaxed);
}

void ClientCfc::runBlock() noexcept
{
    double* a = aether_cfc_buffer(m_active);
    for (int i = 0; i < kBlockSize; ++i) {
        a[2 * i] = m_inBlock[static_cast<std::size_t>(i)];
        a[2 * i + 1] = 0.0;
    }
    aether_cfc_process(m_active);

    double* b = nullptr;
    int nextFft = 0;
    if (m_next) {
        b = aether_cfc_buffer(m_next);
        for (int i = 0; i < kBlockSize; ++i) {
            b[2 * i] = m_inBlock[static_cast<std::size_t>(i)];
            b[2 * i + 1] = 0.0;
        }
        aether_cfc_process(m_next);
        nextFft = aether_cfc_fft_size(m_next);
    }

    for (int i = 0; i < kBlockSize; ++i) {
        double y = a[2 * i];
        if (b) {
            // The new block is silent for one FFT length and ramps up over
            // the next while its overlap-add fills; crossfade after both.
            if (m_nextSamples >= 2 * nextFft) {
                const double w = std::min(
                    1.0, static_cast<double>(m_nextSamples - 2 * nextFft) / kCrossfadeSamples);
                y = (1.0 - w) * y + w * b[2 * i];
            }
            ++m_nextSamples;
        }
        m_outBlock[static_cast<std::size_t>(i)] = static_cast<float>(y);
    }

    if (m_next && m_nextSamples >= 2 * nextFft + kCrossfadeSamples && retire(m_active)) {
        m_active = m_next;
        m_next = nullptr;
    }

    if (m_displayEnabled.load(std::memory_order_relaxed)
        && aether_cfc_compression_db(m_active, m_displayScratch.data(), kDisplayPoints)) {
        const uint32_t seq = m_displaySeq.load(std::memory_order_relaxed);
        m_displaySeq.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < kDisplayPoints; ++i) {
            m_display[static_cast<std::size_t>(i)].store(
                m_displayScratch[static_cast<std::size_t>(i)], std::memory_order_relaxed);
        }
        m_displaySeq.store(seq + 2, std::memory_order_release);
    }
}

void ClientCfc::process(float* interleaved, int frames, int channels) noexcept
{
    if (!interleaved || frames <= 0 || channels < 1 || channels > 2 || !m_active) {
        return;
    }
    recacheIfDirty();

    float inPeak = 0.0f;
    float outPeak = 0.0f;
    for (int f = 0; f < frames; ++f) {
        const float x = interleaved[f * channels];
        inPeak = std::max(inPeak, std::fabs(x));
        m_inBlock[static_cast<std::size_t>(m_fifoPos)] = x;
        const float y = m_outBlock[static_cast<std::size_t>(m_fifoPos)];
        if (++m_fifoPos == kBlockSize) {
            runBlock();
            m_fifoPos = 0;
        }
        outPeak = std::max(outPeak, std::fabs(y));
        interleaved[f * channels] = y;
        if (channels == 2) {
            interleaved[f * channels + 1] = y;
        }
    }
    m_inputPeakDb.store(linToDb(inPeak), std::memory_order_relaxed);
    m_outputPeakDb.store(linToDb(outPeak), std::memory_order_relaxed);
}

}  // namespace AetherSDR
