#pragma once

#include "core/SharedCapturePolicy.h"
#include <QByteArray>
#include <array>
#include <complex>
#include <cstdint>
#include <memory>
#include <span>

namespace AetherSDR {
class Resampler;
namespace rtl {
// Worker-owned, exact fractional-rate RF conversion. Positions are local to
// this decoder epoch; they are never presented as decoded audio timestamps.
class HdFmIqAdapter final {
public:
    static constexpr double kOutputRate = 1488375.0 / 2.0;
    static constexpr std::size_t kMaxInput = 8192;
    class Sink {
    public:
        virtual ~Sink() = default;
        virtual bool hdIq(std::span<const float> interleaved) noexcept = 0;
    };
    HdFmIqAdapter(const SharedCapturePolicy::CaptureDescriptor& capture,
                  const SharedCapturePolicy::SliceDescriptor& footprint);
    ~HdFmIqAdapter();
    bool valid() const noexcept { return m_valid; }
    bool process(std::uint64_t first, std::span<const std::complex<float>> samples, Sink& sink) noexcept;
    std::uint64_t outputFrames() const noexcept { return m_outputFrames; }
private:
    static constexpr std::size_t kChunk = 256;
    const SharedCapturePolicy::CaptureDescriptor m_capture;
    const SharedCapturePolicy::SliceDescriptor m_footprint;
    std::unique_ptr<Resampler> m_i, m_q;
    QByteArray m_outI, m_outQ;
    std::array<float, kChunk> m_inI{}, m_inQ{};
    std::array<float, 32768> m_interleaved{};
    std::size_t m_staged = 0;
    std::complex<double> m_phase{1, 0}, m_step{1, 0};
    std::uint64_t m_next = 0, m_outputFrames = 0;
    std::uint32_t m_normalize = 0;
    bool m_valid = false, m_started = false;
};
} // namespace rtl
} // namespace AetherSDR
