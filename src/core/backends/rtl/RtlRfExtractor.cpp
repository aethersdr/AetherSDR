#include "RtlRfExtractor.h"
#include "core/Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <numeric>

namespace AetherSDR::rtl {
RtlRfExtractor::RtlRfExtractor(Config config) : m_config(std::move(config))
{
    const auto& capture = m_config.capture;
    const auto& slice = m_config.slice;
    if (!std::isfinite(capture.achievedSampleRateHz)
        || capture.achievedSampleRateHz != std::floor(capture.achievedSampleRateHz)
        || capture.achievedSampleRateHz < 225001 || capture.achievedSampleRateHz > 3000000
        || m_config.outputRateHz < 48000 || m_config.outputRateHz > 192000
        || m_config.outputRateHz > capture.achievedSampleRateHz
        || m_config.blockSize < 64 || m_config.blockSize > 4096
        || (m_config.blockSize & (m_config.blockSize - 1)) != 0
        || slice.filterLowHz < -0.45 * m_config.outputRateHz
        || slice.filterHighHz > 0.45 * m_config.outputRateHz) {
        return;
    }
    const SharedCapturePolicy::CenterDomain fixed{capture.centerHz, capture.centerHz, capture.centerHz, 1};
    const auto fit = SharedCapturePolicy::restoreFixedCapture(capture, std::span(&slice, 1),
        std::span(&fixed, 1), {SharedCapturePolicy::kMaxEntries, 1});
    if (fit.error != SharedCapturePolicy::Error::None || fit.accepted.size() != 1) { return; }
    const double step = -2 * std::numbers::pi
        * (slice.carrierHz + slice.translationHz - capture.centerHz) / capture.achievedSampleRateHz;
    m_step = {std::cos(step), std::sin(step)};
    // Fixed staging makes converter calls independent of USB packet partition.
    // The 10% transition leaves the declared +/-45% DSP-rate passband intact.
    m_i = std::make_unique<Resampler>(capture.achievedSampleRateHz, m_config.outputRateHz, kInputChunk, 10.0);
    m_q = std::make_unique<Resampler>(capture.achievedSampleRateHz, m_config.outputRateHz, kInputChunk, 10.0);
    // Downsampling only; room includes r8brain's burst output from its staging.
    m_convertedI.reserve(16384 * sizeof(float));
    m_convertedQ.reserve(16384 * sizeof(float));
    m_blockI.resize(m_config.blockSize);
    m_blockQ.resize(m_config.blockSize);
    m_valid = true;
}
RtlRfExtractor::~RtlRfExtractor() = default;
int RtlRfExtractor::groupDelayInputFrames() const noexcept { return m_i ? m_i->groupDelayInputFrames() : 0; }
bool RtlRfExtractor::process(const SharedCapturePolicy::CaptureDescriptor& capture, std::uint64_t firstSample,
                            std::span<const std::complex<float>> input, Sink& sink, bool discontinuity) noexcept
{
    if (!m_valid || m_withdrawn || capture != m_config.capture) { return false; }
    if (input.empty() || !input.data() || input.size() > kMaxInput
        || firstSample > std::numeric_limits<std::uint64_t>::max() - input.size()
        || (m_seenInput && (discontinuity || firstSample != m_nextInput))
        || !std::ranges::all_of(input, [](const auto& sample) {
            return std::isfinite(sample.real()) && std::isfinite(sample.imag());
        })) {
        m_withdrawn = true;
        return false;
    }
    if (!m_seenInput) {
        // New receivers join the SAME rational sample lattice as siblings.
        // Wait for the next capture/output coincidence, rather than rounding
        // a fractional start and silently shifting its audio by part of a frame.
        // All supported rates are integral hardware readbacks. This wait is
        // bounded by one second even for coprime rates; it consumes no storage.
        const auto rate = static_cast<std::uint64_t>(capture.achievedSampleRateHz);
        const std::uint64_t period = rate / std::gcd(rate, static_cast<std::uint64_t>(m_config.outputRateHz));
        const std::uint64_t skip = (period - firstSample % period) % period;
        if (firstSample > std::numeric_limits<std::uint64_t>::max() - skip) {
            m_withdrawn = true; return false;
        }
        m_startInput = firstSample + skip;
        m_seenInput = true;
    }
    m_nextInput = firstSample + input.size();
    if (!m_started) {
        if (m_nextInput <= m_startInput) { return true; }
        input = input.subspan(static_cast<std::size_t>(m_startInput - firstSample));
        const long double origin = static_cast<long double>(m_startInput)
            * m_config.outputRateHz / capture.achievedSampleRateHz;
        if (origin > std::numeric_limits<std::uint64_t>::max() - kMaxInput) {
            m_withdrawn = true; return false;
        }
        m_outputOrigin = static_cast<std::uint64_t>(std::round(origin));
        const long double phase = std::remainder(-2 * std::numbers::pi_v<long double>
            * (m_config.slice.carrierHz + m_config.slice.translationHz - capture.centerHz)
            * m_startInput / capture.achievedSampleRateHz, 2 * std::numbers::pi_v<long double>);
        m_phasor = {static_cast<double>(std::cos(phase)), static_cast<double>(std::sin(phase))};
        m_started = true;
    }
    for (const std::complex<float>& sample : input) {
        const std::complex<double> shifted = std::complex<double>(sample) * m_phasor;
        m_phasor *= m_step;
        if (++m_normalize == 4096) { m_phasor /= std::abs(m_phasor); m_normalize = 0; }
        m_inputI[m_stagedInput] = static_cast<float>(shifted.real());
        m_inputQ[m_stagedInput++] = static_cast<float>(shifted.imag());
        if (m_stagedInput != kInputChunk) { continue; }
        const int countI = m_i->process(m_inputI.data(), kInputChunk, m_convertedI);
        const int countQ = m_q->process(m_inputQ.data(), kInputChunk, m_convertedQ);
        m_stagedInput = 0;
        if (countI != countQ || countI < 0 || countI > 16384) { m_withdrawn = true; return false; }
        for (int index = 0; index < countI; ++index) {
            std::memcpy(&m_blockI[m_stagedOutput], m_convertedI.constData() + index * sizeof(float), sizeof(float));
            std::memcpy(&m_blockQ[m_stagedOutput], m_convertedQ.constData() + index * sizeof(float), sizeof(float));
            if (!std::isfinite(m_blockI[m_stagedOutput]) || !std::isfinite(m_blockQ[m_stagedOutput])) {
                m_withdrawn = true; return false;
            }
            if (++m_stagedOutput != m_config.blockSize) { continue; }
            if (m_outputFrames > std::numeric_limits<std::uint64_t>::max() - m_config.blockSize
                || m_outputOrigin > std::numeric_limits<std::uint64_t>::max() - m_outputFrames - m_config.blockSize
                || !sink.iqBlock(m_blockI, m_blockQ, m_outputOrigin + m_outputFrames)) {
                m_withdrawn = true; return false;
            }
            m_outputFrames += m_config.blockSize;
            m_stagedOutput = 0;
        }
    }
    return true;
}
} // namespace AetherSDR::rtl
