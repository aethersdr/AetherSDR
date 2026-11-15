#pragma once

#include "PcmFrame.h"

#include <array>
#include <functional>
#include <memory>
#include <span>

namespace AetherSDR {
class Resampler;

// Pure continuous per-consumer RX conversion; admission/epoch tracking belongs
// to TciServer. Not thread-safe. A converter and its output callback run on one
// execution context; the callback must not reenter or destroy this converter.
class TciRxConverter final {
public:
    static constexpr int kInputBlockFrames = 256;
    static constexpr int kMaxOutputBlockFrames = 1024;
    using Output = std::function<bool(QVector<float>)>;

    TciRxConverter(PcmFormat inputFormat, int outputRate);
    ~TciRxConverter();
    TciRxConverter(const TciRxConverter&) = delete;
    TciRxConverter& operator=(const TciRxConverter&) = delete;

    bool valid() const { return m_valid; }
    PcmFormat inputFormat() const { return m_inputFormat; }
    int outputRate() const { return m_outputRate; }

    // Validates the whole call before processing: nonempty, finite, complete
    // input frames, at most PcmFrame::kMaxFrames. Emits owning interleaved stereo
    // blocks synchronously, each <= kMaxOutputBlockFrames. A false sink return
    // aborts further output. Any refusal discards ALL retained stream state.
    bool process(std::span<const float> input, const Output& output);

    // Retires both channel histories and partial input together. No tail is
    // emitted: this is a continuous stream, with no finish/drain/zero padding.
    void discard();

    // Counts since discard: source frames accepted and stereo frames accepted
    // by the sink. Native-rate output has no filter or staging latency. Other
    // ratios hold 0..255 source frames until a complete input block arrives;
    // emitted duration follows processedInput * outputRate / inputRate, within
    // one output frame of continuous rate-conversion rounding. Filter acoustic
    // delay is additional and expressed below in source-rate frames.
    quint64 inputFrames() const { return m_inputFrames; }
    quint64 outputFrames() const { return m_outputFrames; }
    int stagedInputFrames() const { return m_stagedInputFrames; }
    int groupDelayInputFrames() const;

private:
    const PcmFormat m_inputFormat;
    const int m_outputRate;
    const bool m_valid;
    std::unique_ptr<Resampler> m_left;
    std::unique_ptr<Resampler> m_right;
    std::array<float, kInputBlockFrames> m_leftInput{};
    std::array<float, kInputBlockFrames> m_rightInput{};
    QByteArray m_leftOutput;
    QByteArray m_rightOutput;
    int m_stagedInputFrames = 0;
    quint64 m_inputFrames = 0;
    quint64 m_outputFrames = 0;
};
} // namespace AetherSDR
