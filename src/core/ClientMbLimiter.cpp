#include "core/ClientMbLimiter.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kButterworthQ = 0.70710678118654752;

float finiteOr(float v, float fallback) noexcept
{
    return std::isfinite(v) ? v : fallback;
}

bool validBand(int band) noexcept
{
    return band >= 0 && band < ClientMbLimiter::kBands;
}

}  // namespace

ClientMbLimiter::ClientMbLimiter()
{
    for (int i = 0; i < kCrossovers; ++i) {
        m_crossoverHz[static_cast<std::size_t>(i)].store(kDefaultCrossoverHz[static_cast<std::size_t>(i)]);
    }
    for (int b = 0; b < kBands; ++b) {
        m_thresholdDb[static_cast<std::size_t>(b)].store(kDefaultThresholdDb);
        m_releaseMs[static_cast<std::size_t>(b)].store(kDefaultReleaseMs);
        m_grPeak[static_cast<std::size_t>(b)].store(1.0f);
    }
}

void ClientMbLimiter::prepare(double sampleRate)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : kRate;
    m_appliedVersion = 0;
    applyParameters();
    m_crossoverLive = m_crossoverTarget;
    updateFilters();
    clearState();
    m_flushRequested.store(false, std::memory_order_relaxed);
}

void ClientMbLimiter::setEnabled(bool on) noexcept
{
    const bool was = m_enabled.exchange(on, std::memory_order_acq_rel);
    if (on && !was) {
        // The filters and the delay lines still hold the last run.
        m_flushRequested.store(true, std::memory_order_release);
    }
}

bool ClientMbLimiter::isEnabled() const noexcept
{
    return m_enabled.load(std::memory_order_acquire);
}

float ClientMbLimiter::clampCrossover(int index, float hz, float below, float above) noexcept
{
    const auto i = static_cast<std::size_t>(index);
    float lo = kMinCrossoverHz[i];
    float hi = kMaxCrossoverHz[i];
    if (below > 0.0f) {
        lo = std::max(lo, below * kMinCrossoverRatio);
    }
    if (above > 0.0f) {
        hi = std::min(hi, above / kMinCrossoverRatio);
    }
    // The ranges are chosen so this window is never empty.
    return std::round(std::clamp(finiteOr(hz, kDefaultCrossoverHz[i]), lo, std::max(lo, hi)));
}

void ClientMbLimiter::setCrossoverHz(int index, float hz) noexcept
{
    if (index < 0 || index >= kCrossovers) {
        return;
    }
    const float below = index > 0 ? m_crossoverHz[static_cast<std::size_t>(index - 1)].load() : 0.0f;
    const float above =
        index + 1 < kCrossovers ? m_crossoverHz[static_cast<std::size_t>(index + 1)].load() : 0.0f;
    m_crossoverHz[static_cast<std::size_t>(index)].store(clampCrossover(index, hz, below, above),
                                                         std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

void ClientMbLimiter::setCrossoversHz(float x1, float x2, float x3) noexcept
{
    const float mid = clampCrossover(1, x2, 0.0f, 0.0f);
    const float low = clampCrossover(0, x1, 0.0f, mid);
    const float high = clampCrossover(2, x3, mid, 0.0f);
    m_crossoverHz[0].store(low, std::memory_order_relaxed);
    m_crossoverHz[1].store(mid, std::memory_order_relaxed);
    m_crossoverHz[2].store(high, std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

float ClientMbLimiter::crossoverHz(int index) const noexcept
{
    if (index < 0 || index >= kCrossovers) {
        return 0.0f;
    }
    return m_crossoverHz[static_cast<std::size_t>(index)].load(std::memory_order_relaxed);
}

void ClientMbLimiter::setThresholdDb(int band, float db) noexcept
{
    if (!validBand(band)) {
        return;
    }
    m_thresholdDb[static_cast<std::size_t>(band)].store(
        std::clamp(finiteOr(db, kDefaultThresholdDb), kMinThresholdDb, kMaxThresholdDb),
        std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

float ClientMbLimiter::thresholdDb(int band) const noexcept
{
    return validBand(band) ? m_thresholdDb[static_cast<std::size_t>(band)].load() : 0.0f;
}

void ClientMbLimiter::setReleaseMs(int band, float ms) noexcept
{
    if (!validBand(band)) {
        return;
    }
    m_releaseMs[static_cast<std::size_t>(band)].store(
        std::clamp(finiteOr(ms, kDefaultReleaseMs), kMinReleaseMs, kMaxReleaseMs),
        std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

float ClientMbLimiter::releaseMs(int band) const noexcept
{
    return validBand(band) ? m_releaseMs[static_cast<std::size_t>(band)].load() : 0.0f;
}

void ClientMbLimiter::reset() noexcept
{
    m_flushRequested.store(true, std::memory_order_release);
}

float ClientMbLimiter::takeGainReductionDb(int band) noexcept
{
    if (!validBand(band)) {
        return 0.0f;
    }
    const float g = m_grPeak[static_cast<std::size_t>(band)].exchange(1.0f, std::memory_order_relaxed);
    return g < 1.0f ? 20.0f * std::log10(std::max(g, 1e-6f)) : 0.0f;
}

void ClientMbLimiter::applyParameters() noexcept
{
    const double fs = m_sampleRate;
    for (int i = 0; i < kCrossovers; ++i) {
        m_crossoverTarget[static_cast<std::size_t>(i)] =
            m_crossoverHz[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
    }
    for (int b = 0; b < kBands; ++b) {
        const auto i = static_cast<std::size_t>(b);
        m_bands[i].configure(std::pow(10.0f, m_thresholdDb[i].load() / 20.0f), m_releaseMs[i].load(),
                             fs);
    }
}

bool ClientMbLimiter::glideCrossovers() noexcept
{
    bool moving = false;
    for (int i = 0; i < kCrossovers; ++i) {
        double& live = m_crossoverLive[static_cast<std::size_t>(i)];
        const double target = m_crossoverTarget[static_cast<std::size_t>(i)];
        if (live == target) {
            continue;
        }
        const double ratio = std::clamp(target / live, 1.0 / kGlideStep, kGlideStep);
        live = std::fabs(target / (live * ratio) - 1.0) < 1e-9 ? target : live * ratio;
        moving = true;
    }
    if (moving) {
        updateFilters();
    }
    return moving;
}

void ClientMbLimiter::updateFilters() noexcept
{
    const double fs = m_sampleRate;
    const double x1 = m_crossoverLive[0];
    const double x2 = m_crossoverLive[1];
    const double x3 = m_crossoverLive[2];
    for (int s = 0; s < 2; ++s) {
        const auto i = static_cast<std::size_t>(s);
        m_lp2[i].setCoefficients(Biquad::Type::LowPass, fs, x2, kButterworthQ);
        m_hp2[i].setCoefficients(Biquad::Type::HighPass, fs, x2, kButterworthQ);
        m_lp1[i].setCoefficients(Biquad::Type::LowPass, fs, x1, kButterworthQ);
        m_hp1[i].setCoefficients(Biquad::Type::HighPass, fs, x1, kButterworthQ);
        m_lp3[i].setCoefficients(Biquad::Type::LowPass, fs, x3, kButterworthQ);
        m_hp3[i].setCoefficients(Biquad::Type::HighPass, fs, x3, kButterworthQ);
    }
    // An LR4 low-pass plus its high-pass is this all-pass.
    m_ap3Low.setCoefficients(Biquad::Type::AllPass, fs, x3, kButterworthQ);
    m_ap1High.setCoefficients(Biquad::Type::AllPass, fs, x1, kButterworthQ);
}

void ClientMbLimiter::BandLimiter::configure(float thresholdLinear, float releaseMsValue,
                                             double sampleRate) noexcept
{
    threshold = thresholdLinear;
    const double tau = std::max(1.0f, releaseMsValue) / 1000.0 * sampleRate;
    releaseCoeff = static_cast<float>(1.0 - std::exp(-1.0 / tau));
}

void ClientMbLimiter::BandLimiter::clear() noexcept
{
    delay.fill(0.0f);
    delayPos = 0;
    held = 1.0f;
    qHead = 0;
    qSize = 0;
    avgRing.fill(1.0f);
    avgPos = 0;
    avgSum = kWindow;
    minGain = 1.0f;
}

void ClientMbLimiter::clearState() noexcept
{
    for (auto* set : {&m_lp2, &m_hp2, &m_lp1, &m_hp1, &m_lp3, &m_hp3}) {
        for (auto& s : *set) {
            s.reset();
        }
    }
    m_ap3Low.reset();
    m_ap1High.reset();
    for (auto& band : m_bands) {
        band.clear();
    }
    m_sampleIndex = 0;
}

float ClientMbLimiter::BandLimiter::process(float x, int64_t n) noexcept
{
    // The gain this sample needs, released toward 1 from the last one.
    const float mag = std::fabs(x);
    const float need = mag > threshold ? threshold / mag : 1.0f;
    held = need < held ? need : held + (need - held) * releaseCoeff;

    // Sliding minimum over the last kWindow samples. The expired head goes
    // first, so the ring never holds more than kWindow entries.
    if (qSize > 0 && qIndex[static_cast<std::size_t>(qHead)] <= n - kWindow) {
        qHead = (qHead + 1) % kWindow;
        --qSize;
    }
    while (qSize > 0) {
        const int back = (qHead + qSize - 1) % kWindow;
        if (qGain[static_cast<std::size_t>(back)] < held) {
            break;
        }
        --qSize;
    }
    const int tail = (qHead + qSize) % kWindow;
    qGain[static_cast<std::size_t>(tail)] = held;
    qIndex[static_cast<std::size_t>(tail)] = n;
    ++qSize;
    const float windowMin = qGain[static_cast<std::size_t>(qHead)];

    // Its moving average over the same length: a ramp that lands on the
    // minimum by the time the sample that needed it leaves the delay.
    avgSum += windowMin - avgRing[static_cast<std::size_t>(avgPos)];
    avgRing[static_cast<std::size_t>(avgPos)] = windowMin;
    avgPos = (avgPos + 1) % kWindow;
    const float gain = std::min(1.0f, static_cast<float>(avgSum / kWindow));

    const float delayed = delay[static_cast<std::size_t>(delayPos)];
    delay[static_cast<std::size_t>(delayPos)] = x;
    delayPos = (delayPos + 1) % kLookahead;
    minGain = std::min(minGain, gain);
    return delayed * gain;
}

void ClientMbLimiter::process(float* interleaved, int frames, int channels) noexcept
{
    if (!interleaved || frames <= 0 || channels < 1) {
        return;
    }
    const uint64_t version = m_version.load(std::memory_order_acquire);
    if (version != m_appliedVersion) {
        m_appliedVersion = version;
        applyParameters();
    }
    if (m_flushRequested.exchange(false, std::memory_order_acq_rel)) {
        clearState();
    }
    // While a crossover glides, run in short pieces with a filter update
    // between each.
    for (int at = 0; at < frames;) {
        const int n = glideCrossovers() ? std::min(kGlideFrames, frames - at) : frames - at;
        processFrames(interleaved + at * channels, n, channels);
        at += n;
    }
    publishGainReduction();
}

void ClientMbLimiter::processFrames(float* interleaved, int frames, int channels) noexcept
{
    for (int f = 0; f < frames; ++f) {
        const float x = interleaved[f * channels];
        float low = m_lp2[1].process(m_lp2[0].process(x));
        float high = m_hp2[1].process(m_hp2[0].process(x));
        low = m_ap3Low.process(low);
        high = m_ap1High.process(high);
        const float b0 = m_lp1[1].process(m_lp1[0].process(low));
        const float b1 = m_hp1[1].process(m_hp1[0].process(low));
        const float b2 = m_lp3[1].process(m_lp3[0].process(high));
        const float b3 = m_hp3[1].process(m_hp3[0].process(high));
        const int64_t n = m_sampleIndex++;
        const float y = m_bands[0].process(b0, n) + m_bands[1].process(b1, n)
                        + m_bands[2].process(b2, n) + m_bands[3].process(b3, n);
        for (int c = 0; c < channels; ++c) {
            interleaved[f * channels + c] = y;
        }
    }
}

void ClientMbLimiter::publishGainReduction() noexcept
{
    for (int b = 0; b < kBands; ++b) {
        auto& band = m_bands[static_cast<std::size_t>(b)];
        auto& peak = m_grPeak[static_cast<std::size_t>(b)];
        float current = peak.load(std::memory_order_relaxed);
        while (band.minGain < current
               && !peak.compare_exchange_weak(current, band.minGain, std::memory_order_relaxed)) {
        }
        band.minGain = 1.0f;
    }
}

}  // namespace AetherSDR
