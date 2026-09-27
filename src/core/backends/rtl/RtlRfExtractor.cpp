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
        || m_config.outputRateHz < 48000 || m_config.outputRateHz > 384000
        || m_config.outputRateHz > 2 * capture.achievedSampleRateHz
        || m_config.alignmentRateHz < 0
        || (m_config.alignmentRateHz != 0
            && (m_config.alignmentRateHz > m_config.outputRateHz
                || m_config.outputRateHz % m_config.alignmentRateHz != 0))
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
    // At most 2x upsampling; room includes r8brain's staging burst. The RF
    // fit still uses actual capture coverage, never the interpolated IQ rate.
    m_convertedI.reserve(16384 * sizeof(float));
    m_convertedQ.reserve(16384 * sizeof(float));
    m_blockI.resize(m_config.blockSize);
    m_blockQ.resize(m_config.blockSize);
    m_valid = true;
}
std::optional<std::uint64_t> RtlRfExtractor::alignedCaptureFirst(std::uint64_t firstSample,
    std::uint64_t captureRateHz, std::uint64_t alignmentRateHz) noexcept
{
    if (captureRateHz < 225001 || captureRateHz > 3000000
        || alignmentRateHz == 0 || alignmentRateHz > 384000) { return std::nullopt; }
    const std::uint64_t period = captureRateHz / std::gcd(captureRateHz, alignmentRateHz);
    const std::uint64_t skip = (period - firstSample % period) % period;
    if (firstSample > std::numeric_limits<std::uint64_t>::max() - skip) { return std::nullopt; }
    return firstSample + skip;
}
RtlRfExtractor::~RtlRfExtractor() = default;
int RtlRfExtractor::groupDelayInputFrames() const noexcept { return m_i ? m_i->groupDelayInputFrames() : 0; }
bool RtlRfExtractor::process(const SharedCapturePolicy::CaptureDescriptor& capture, std::uint64_t firstSample,
                            std::span<const std::complex<float>> input, Sink& sink, bool discontinuity) noexcept
{
    const std::uint64_t captureFrames = input.size();
    const std::uint64_t expectedCaptureFirst = m_nextInput;
    const bool hasExpectedCaptureFirst = m_seenInput;
    const auto fail = [&](FailureReason reason, bool withdraw = true,
                          int convertedI = 0, int convertedQ = 0) {
        if (!m_failure) {
            Failure failure;
            failure.reason = reason;
            failure.expectedCaptureFirst = expectedCaptureFirst;
            failure.captureFirst = firstSample;
            failure.captureFrames = captureFrames;
            failure.hasExpectedCaptureFirst = hasExpectedCaptureFirst;
            failure.iqFrames = m_stagedOutput;
            failure.hasIqFirst = m_started
                && m_outputOrigin <= std::numeric_limits<std::uint64_t>::max() - m_outputFrames;
            if (failure.hasIqFirst) { failure.iqFirst = m_outputOrigin + m_outputFrames; }
            failure.convertedI = convertedI;
            failure.convertedQ = convertedQ;
            m_failure = failure;
        }
        if (withdraw) { m_withdrawn = true; }
        return false;
    };
    if (!m_valid) { return fail(FailureReason::InvalidConfiguration, false); }
    if (m_withdrawn) { return false; }
    if (capture != m_config.capture) { return fail(FailureReason::CaptureMismatch, false); }
    if (input.empty()) { return fail(FailureReason::EmptyInput); }
    if (!input.data()) { return fail(FailureReason::NullInput); }
    if (input.size() > kMaxInput) { return fail(FailureReason::InputTooLarge); }
    if (firstSample > std::numeric_limits<std::uint64_t>::max() - input.size()) {
        return fail(FailureReason::CapturePositionOverflow);
    }
    if (m_seenInput && discontinuity) { return fail(FailureReason::Discontinuity); }
    if (m_seenInput && firstSample != m_nextInput) { return fail(FailureReason::CapturePositionMismatch); }
    if (!std::ranges::all_of(input, [](const auto& sample) {
        return std::isfinite(sample.real()) && std::isfinite(sample.imag());
    })) { return fail(FailureReason::NonFiniteInput); }
    if (!m_seenInput) {
        // New receivers join the SAME rational sample lattice as siblings.
        // Wait for the next capture/output coincidence, rather than rounding
        // a fractional start and silently shifting its audio by part of a frame.
        // All supported rates are integral hardware readbacks. This wait is
        // bounded by one second even for coprime rates; it consumes no storage.
        const auto rate = static_cast<std::uint64_t>(capture.achievedSampleRateHz);
        const int alignmentRate = m_config.alignmentRateHz != 0
            ? m_config.alignmentRateHz : m_config.outputRateHz;
        const auto start = alignedCaptureFirst(firstSample, rate, static_cast<std::uint64_t>(alignmentRate));
        if (!start) { return fail(FailureReason::AlignmentOverflow); }
        m_startInput = *start;
        m_seenInput = true;
    }
    m_nextInput = firstSample + input.size();
    if (!m_started) {
        if (m_nextInput <= m_startInput) { return true; }
        input = input.subspan(static_cast<std::size_t>(m_startInput - firstSample));
        const auto rate = static_cast<std::uint64_t>(capture.achievedSampleRateHz);
        const int alignmentRate = m_config.alignmentRateHz != 0
            ? m_config.alignmentRateHz : m_config.outputRateHz;
        const std::uint64_t common = std::gcd(rate, static_cast<std::uint64_t>(alignmentRate));
        const std::uint64_t periods = m_startInput / (rate / common);
        const std::uint64_t framesPerPeriod = static_cast<std::uint64_t>(m_config.outputRateHz) / common;
        if (periods > (std::numeric_limits<std::uint64_t>::max() - kMaxInput) / framesPerPeriod) {
            return fail(FailureReason::OutputOriginOverflow);
        }
        // Exact integer time mapping also holds beyond floating-point's exact
        // integer range. The RF oscillator separately retains its phase model.
        m_outputOrigin = periods * framesPerPeriod;
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
        if (countI != countQ) { return fail(FailureReason::ConvertedCountMismatch, true, countI, countQ); }
        if (countI < 0 || countI > 16384) {
            return fail(FailureReason::ConvertedCountOutOfRange, true, countI, countQ);
        }
        for (int index = 0; index < countI; ++index) {
            std::memcpy(&m_blockI[m_stagedOutput], m_convertedI.constData() + index * sizeof(float), sizeof(float));
            std::memcpy(&m_blockQ[m_stagedOutput], m_convertedQ.constData() + index * sizeof(float), sizeof(float));
            if (!std::isfinite(m_blockI[m_stagedOutput]) || !std::isfinite(m_blockQ[m_stagedOutput])) {
                return fail(FailureReason::NonFiniteOutput, true, countI, countQ);
            }
            if (++m_stagedOutput != m_config.blockSize) { continue; }
            if (m_outputFrames > std::numeric_limits<std::uint64_t>::max() - m_config.blockSize
                || m_outputOrigin > std::numeric_limits<std::uint64_t>::max() - m_outputFrames - m_config.blockSize) {
                return fail(FailureReason::OutputPositionOverflow);
            }
            if (!sink.iqBlock(m_blockI, m_blockQ, m_outputOrigin + m_outputFrames)) {
                return fail(FailureReason::SinkRejected);
            }
            m_outputFrames += m_config.blockSize;
            m_stagedOutput = 0;
        }
    }
    return true;
}
} // namespace AetherSDR::rtl
