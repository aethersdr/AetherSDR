#include "HdFmIqAdapter.h"
#include "core/Resampler.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace AetherSDR::rtl {
HdFmIqAdapter::HdFmIqAdapter(const SharedCapturePolicy::CaptureDescriptor& capture,
    const SharedCapturePolicy::SliceDescriptor& footprint)
    : m_capture(capture), m_footprint(footprint)
{
    if (!std::isfinite(capture.achievedSampleRateHz)
        || capture.achievedSampleRateHz != std::floor(capture.achievedSampleRateHz)
        || capture.achievedSampleRateHz < 900001 || capture.achievedSampleRateHz > 3000000) { return; }
    const SharedCapturePolicy::CenterDomain fixed{capture.centerHz, capture.centerHz, capture.centerHz, 1};
    const auto fit = SharedCapturePolicy::restoreFixedCapture(capture, std::span(&footprint, 1),
        std::span(&fixed, 1), {SharedCapturePolicy::kMaxEntries, 1});
    if (fit.error != SharedCapturePolicy::Error::None || fit.accepted.size() != 1) { return; }
    const double step = -2 * std::numbers::pi
        * (footprint.carrierHz + footprint.translationHz - capture.centerHz) / capture.achievedSampleRateHz;
    m_step = {std::cos(step), std::sin(step)};
    m_i = std::make_unique<Resampler>(capture.achievedSampleRateHz, kOutputRate, kChunk, 10.0);
    m_q = std::make_unique<Resampler>(capture.achievedSampleRateHz, kOutputRate, kChunk, 10.0);
    m_outI.reserve(16384 * sizeof(float));
    m_outQ.reserve(16384 * sizeof(float));
    m_valid = true;
}
HdFmIqAdapter::~HdFmIqAdapter() = default;
bool HdFmIqAdapter::process(std::uint64_t first,
    std::span<const std::complex<float>> samples, Sink& sink) noexcept
{
    if (!m_valid || samples.empty() || samples.data() == nullptr || samples.size() > kMaxInput
        || first > std::numeric_limits<std::uint64_t>::max() - samples.size()
        || (m_started && first != m_next)
        || !std::ranges::all_of(samples, [](const auto& value) {
            return std::isfinite(value.real()) && std::isfinite(value.imag());
        })) { m_valid = false; return false; }
    if (!m_started) {
        const long double phase = std::remainder(-2 * std::numbers::pi_v<long double>
            * (m_footprint.carrierHz + m_footprint.translationHz - m_capture.centerHz)
            * first / m_capture.achievedSampleRateHz, 2 * std::numbers::pi_v<long double>);
        m_phase = {static_cast<double>(std::cos(phase)), static_cast<double>(std::sin(phase))};
        m_started = true;
    }
    m_next = first + samples.size();
    for (const auto& value : samples) {
        const std::complex<double> shifted = std::complex<double>(value) * m_phase;
        m_inI[m_staged] = static_cast<float>(shifted.real());
        m_inQ[m_staged++] = static_cast<float>(shifted.imag());
        m_phase *= m_step;
        if (++m_normalize == 1024) { m_phase /= std::abs(m_phase); m_normalize = 0; }
        if (m_staged != kChunk) { continue; }
        m_staged = 0;
        const int count = m_i->process(m_inI.data(), kChunk, m_outI);
        const int countQ = m_q->process(m_inQ.data(), kChunk, m_outQ);
        if (count < 0 || count != countQ || count > 16384
            || m_outputFrames > std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(count)) { m_valid = false; return false; }
        const auto* i = reinterpret_cast<const float*>(m_outI.constData());
        const auto* q = reinterpret_cast<const float*>(m_outQ.constData());
        for (int n = 0; n < count; ++n) {
            if (!std::isfinite(i[n]) || !std::isfinite(q[n])) { m_valid = false; return false; }
            m_interleaved[2 * n] = i[n]; m_interleaved[2 * n + 1] = q[n];
        }
        if (count && !sink.hdIq(std::span(m_interleaved).first(2 * count))) { m_valid = false; return false; }
        m_outputFrames += static_cast<std::uint64_t>(count);
    }
    return true;
}
} // namespace AetherSDR::rtl
