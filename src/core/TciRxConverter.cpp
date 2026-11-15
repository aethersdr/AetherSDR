#include "TciRxConverter.h"

#include "Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace AetherSDR {
namespace {
bool supportedOutputRate(int rate)
{
    return rate == 8000 || rate == 12000 || rate == 24000
        || rate == 44100 || rate == 48000;
}
} // namespace

TciRxConverter::TciRxConverter(PcmFormat inputFormat, int outputRate)
    : m_inputFormat(inputFormat)
    , m_outputRate(outputRate)
    , m_valid(inputFormat.valid() && supportedOutputRate(outputRate))
{
    if (m_valid && inputFormat.sampleRateHz != outputRate) {
        // Keep the existing TCI filter quality (Resampler's 2% transition).
        // processStereoToStereo() downmixes; independent mono filters preserve
        // antiphase and wide stereo instead of cancelling either channel.
        m_left = std::make_unique<Resampler>(inputFormat.sampleRateHz, outputRate,
                                             kInputBlockFrames);
        m_right = std::make_unique<Resampler>(inputFormat.sampleRateHz, outputRate,
                                              kInputBlockFrames);
    }
}

TciRxConverter::~TciRxConverter() = default;

bool TciRxConverter::process(std::span<const float> input, const Output& output)
{
    const int channels = m_inputFormat.channels();
    if (!m_valid || !output || input.empty() || !input.data()
        || input.size() % channels != 0
        || input.size() / channels > static_cast<std::size_t>(PcmFrame::kMaxFrames)
        || !std::ranges::all_of(input, [](float sample) { return std::isfinite(sample); })) {
        discard();
        return false;
    }
    const quint64 frames = input.size() / channels;
    if (m_inputFrames > std::numeric_limits<quint64>::max() - frames) {
        discard();
        return false;
    }
    m_inputFrames += frames;

    // Equal rates bypass filtering and batching, including for tiny inputs.
    // Limit each owning output payload even when one input call is maximal.
    if (!m_left) {
        for (quint64 offset = 0; offset < frames;) {
            const int count = static_cast<int>(std::min<quint64>(frames - offset,
                                                                kMaxOutputBlockFrames));
            QVector<float> block(count * 2);
            if (channels == 2) {
                std::copy_n(input.data() + offset * 2, count * 2, block.data());
            } else {
                for (int frame = 0; frame < count; ++frame) {
                    block[2 * frame] = input[offset + frame];
                    block[2 * frame + 1] = input[offset + frame];
                }
            }
            if (m_outputFrames > std::numeric_limits<quint64>::max() - count
                || !output(std::move(block))) {
                discard();
                return false;
            }
            m_outputFrames += count;
            offset += count;
        }
        return true;
    }

    for (quint64 offset = 0; offset < frames;) {
        const int count = static_cast<int>(std::min<quint64>(frames - offset,
                                             kInputBlockFrames - m_stagedInputFrames));
        for (int frame = 0; frame < count; ++frame) {
            const std::size_t sample = (offset + frame) * channels;
            m_leftInput[m_stagedInputFrames + frame] = input[sample];
            m_rightInput[m_stagedInputFrames + frame] = input[sample + channels - 1];
        }
        offset += count;
        m_stagedInputFrames += count;
        if (m_stagedInputFrames != kInputBlockFrames) {
            continue;
        }

        // Every resampler call has the same source block boundary regardless
        // of producer packet partitioning. There is no finite-stream drain.
        const int leftFrames = m_left->process(m_leftInput.data(), kInputBlockFrames,
                                               m_leftOutput);
        const int rightFrames = m_right->process(m_rightInput.data(), kInputBlockFrames,
                                                 m_rightOutput);
        m_stagedInputFrames = 0;
        if (leftFrames != rightFrames || leftFrames < 0
            || leftFrames > kMaxOutputBlockFrames
            || m_leftOutput.size() != leftFrames * static_cast<int>(sizeof(float))
            || m_rightOutput.size() != rightFrames * static_cast<int>(sizeof(float))) {
            discard();
            return false;
        }
        if (leftFrames == 0) {
            continue;
        }
        QVector<float> block(leftFrames * 2);
        for (int frame = 0; frame < leftFrames; ++frame) {
            // QByteArray does not promise float alignment to its callers.
            std::memcpy(&block[2 * frame], m_leftOutput.constData() + frame * sizeof(float), sizeof(float));
            std::memcpy(&block[2 * frame + 1], m_rightOutput.constData() + frame * sizeof(float), sizeof(float));
        }
        if (!std::ranges::all_of(block, [](float sample) { return std::isfinite(sample); })
            || m_outputFrames > std::numeric_limits<quint64>::max() - leftFrames
            || !output(std::move(block))) {
            discard();
            return false;
        }
        m_outputFrames += leftFrames;
        m_leftOutput.resize(0);
        m_rightOutput.resize(0);
    }
    return true;
}

void TciRxConverter::discard()
{
    m_stagedInputFrames = 0;
    m_inputFrames = 0;
    m_outputFrames = 0;
    m_leftInput.fill(0.0f);
    m_rightInput.fill(0.0f);
    m_leftOutput.resize(0);
    m_rightOutput.resize(0);
    if (m_left) {
        m_left->reset();
        m_right->reset();
    }
}

int TciRxConverter::groupDelayInputFrames() const
{
    return m_left ? m_left->groupDelayInputFrames() : 0;
}
} // namespace AetherSDR
