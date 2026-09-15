#pragma once

#include <QByteArray>
#include <QByteArrayView>

#include <cstdint>
#include <memory>
#include <vector>

namespace AetherSDR {

class Resampler;

// One serialized, independent source in a recording or finite WAV playback.
// Rates and encodings are immutable. This class has no device, source-epoch or
// thread-safety contract; the owner admits sources and serializes all calls.
class QsoPcmConverter {
public:
    enum class InputEncoding { Float32Native, Int16Native, Int16LittleEndian };
    enum class OutputEncoding { Int16LittleEndian, Int16Native, Float32Native };

    struct Configuration {
        int sourceRate{0};
        int sourceChannels{0};
        InputEncoding sourceEncoding{InputEncoding::Float32Native};
        int outputRate{0};
        int outputChannels{0};
        OutputEncoding outputEncoding{OutputEncoding::Int16LittleEndian};
    };

    static constexpr int kMaxInputFrames = 65536;
    static constexpr int kMinOutputRate = 8000;
    static constexpr int kMaxOutputRate = 192000;

    explicit QsoPcmConverter(const Configuration& configuration);
    ~QsoPcmConverter();
    QsoPcmConverter(const QsoPcmConverter&) = delete;
    QsoPcmConverter& operator=(const QsoPcmConverter&) = delete;

    bool isValid() const noexcept { return m_valid; }
    const Configuration& configuration() const noexcept { return m_configuration; }
    std::uint64_t inputFrames() const noexcept { return m_inputFrames; }
    std::uint64_t outputFrames() const noexcept { return m_outputFrames; }
    std::uint64_t targetOutputFrames() const noexcept;
    bool finished() const noexcept { return m_finished; }

    // Accept one complete, nonempty block, at most kMaxInputFrames. Every
    // sample must be finite. Rejected input preserves output and the cursor.
    // Native samples are read with memcpy: no pointer-alignment requirement.
    // Mono is duplicated before independent L/R conversion; mono output is
    // the arithmetic mean of converted L/R. Float input saturates to [-1,1]
    // before filtering, as legacy recording does before quantization. Output
    // packing clips filter overshoot too (including Float32Native output).
    // Float input uses legacy *32767, truncate-to-zero PCM16 quantization;
    // PCM16 input uses /32768 and *32768, preserving equal-rate PCM16 bits.
    // Unequal rates stage fixed 256-frame chunks (at most 255 pending source
    // frames) so even coprime ratios produce the same bits for any partition.
    bool process(QByteArrayView input, QByteArray& output);

    // Finish accepted history, return only its delayed real duration, and
    // seal the instance. Repeated finish succeeds with an empty output.
    // Total output is round(inputFrames * outputRate / sourceRate), ties up.
    // A bounded silent prefix aligns the resampler phase to an exact output
    // frame before real input; trimming that prefix removes startup delay.
    bool finish(QByteArray& output);

    // Abandon delayed samples and seal this instance. It cannot be resumed
    // or drained. A replacement source must receive a fresh converter.
    void discard() noexcept;

private:
    bool decode(QByteArrayView input, int& frames);
    bool convert(const float* left, const float* right, int frames,
                 QByteArray& output);
    bool packConverted(QByteArrayView left, QByteArrayView right,
                       QByteArray& output);
    void pack(const float* left, const float* right, int frames,
              QByteArray& output) const;

    const Configuration m_configuration;
    bool m_valid{false};
    bool m_finished{false};
    bool m_discarded{false};
    std::uint64_t m_inputFrames{0};
    std::uint64_t m_outputFrames{0};
    std::uint64_t m_skipOutputFrames{0};
    std::unique_ptr<Resampler> m_leftResampler;
    std::unique_ptr<Resampler> m_rightResampler;
    std::vector<float> m_leftInput;
    std::vector<float> m_rightInput;
    std::vector<float> m_leftPending;
    std::vector<float> m_rightPending;
};

} // namespace AetherSDR
