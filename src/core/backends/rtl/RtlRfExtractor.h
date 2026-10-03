#pragma once

#include "core/SharedCapturePolicy.h"

#include <QByteArray>
#include <array>
#include <complex>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace AetherSDR {
class Resampler;
namespace rtl {

// Prepared off acquisition. Independent I/Q histories, phase-continuous NCO,
// and fixed planar DSP blocks. A gap withdraws this instance; its replacement
// is prepared off-thread rather than resetting allocating conversion state in
// the callback. Configuration and capture readback are immutable for its life.
class RtlRfExtractor final {
public:
    enum class FailureReason {
        InvalidConfiguration, CaptureMismatch, EmptyInput, NullInput, InputTooLarge,
        CapturePositionOverflow, Discontinuity, CapturePositionMismatch, NonFiniteInput,
        AlignmentOverflow, OutputOriginOverflow, ConvertedCountMismatch,
        ConvertedCountOutOfRange, NonFiniteOutput, OutputPositionOverflow, SinkRejected
    };
    struct Failure {
        FailureReason reason = FailureReason::InvalidConfiguration;
        std::uint64_t expectedCaptureFirst = 0;
        std::uint64_t captureFirst = 0;
        std::uint64_t captureFrames = 0;
        // The next/attempted IQ block, including any samples already staged.
        // hasIqFirst is false before an exact output origin is established.
        std::uint64_t iqFirst = 0;
        std::uint64_t iqFrames = 0;
        bool hasExpectedCaptureFirst = false;
        bool hasIqFirst = false;
        int convertedI = 0;
        int convertedQ = 0;
        bool operator==(const Failure&) const = default;
    };
    struct Config {
        SharedCapturePolicy::CaptureDescriptor capture;
        SharedCapturePolicy::SliceDescriptor slice;
        int outputRateHz = 48000;
        std::size_t blockSize = 1024;
        // The final audio lattice can be slower than the extracted IQ clock.
        // Zero preserves the existing outputRateHz alignment.
        int alignmentRateHz = 0;
    };
    class Sink {
    public:
        virtual ~Sink() = default;
        virtual bool iqBlock(std::span<const float> i, std::span<const float> q,
                             std::uint64_t firstSample) noexcept = 0;
    };
    // First integral capture sample coinciding with the requested sample
    // lattice. Fixed integer arithmetic; nullopt preserves overflow refusal.
    static std::optional<std::uint64_t> alignedCaptureFirst(std::uint64_t firstSample,
        std::uint64_t captureRateHz, std::uint64_t alignmentRateHz) noexcept;
    static constexpr std::size_t kInputChunk = 256;
    static constexpr std::size_t kMaxInput = 65536;
    explicit RtlRfExtractor(Config config);
    ~RtlRfExtractor();
    bool valid() const noexcept { return m_valid; }
    bool withdrawn() const noexcept { return m_withdrawn; }
    // Acquisition context only, or after it stops. The first failure survives
    // later calls, including attempts to use an already withdrawn instance.
    std::optional<Failure> failure() const noexcept { return m_failure; }
    const Config& config() const noexcept { return m_config; }
    int groupDelayInputFrames() const noexcept;
    std::uint64_t outputFrames() const noexcept { return m_outputFrames; }
    // firstSample is in capture-rate samples. Output positions use the same
    // capture origin at outputRateHz. Acoustic filter delay is exposed above;
    // no arrival-clock timestamp or per-call rounding enters this conversion.
    bool process(const SharedCapturePolicy::CaptureDescriptor& capture,
                 std::uint64_t firstSample, std::span<const std::complex<float>> input,
                 Sink& sink, bool discontinuity = false) noexcept;
private:
    const Config m_config;
    bool m_valid = false;
    bool m_withdrawn = false;
    std::optional<Failure> m_failure;
    bool m_started = false;
    bool m_seenInput = false;
    std::uint64_t m_startInput = 0;
    std::uint64_t m_nextInput = 0;
    std::uint64_t m_outputOrigin = 0;
    std::uint64_t m_outputFrames = 0;
    std::complex<double> m_phasor{1, 0};
    std::complex<double> m_step{1, 0};
    std::uint32_t m_normalize = 0;
    std::unique_ptr<Resampler> m_i;
    std::unique_ptr<Resampler> m_q;
    std::array<float, kInputChunk> m_inputI{};
    std::array<float, kInputChunk> m_inputQ{};
    std::size_t m_stagedInput = 0;
    QByteArray m_convertedI;
    QByteArray m_convertedQ;
    std::vector<float> m_blockI;
    std::vector<float> m_blockQ;
    std::size_t m_stagedOutput = 0;
};
} // namespace rtl
} // namespace AetherSDR
