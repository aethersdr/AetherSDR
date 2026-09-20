#include "QsoPcmConverter.h"

#include "Resampler.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace AetherSDR {
namespace {
constexpr int kProcessFrames = 256;

bool supportedRate(int rate)
{
    return rate == 24000 || rate == 44100 || rate == 48000;
}

bool supportedChannels(int channels)
{
    return channels == 1 || channels == 2;
}

// Splitting quotient/remainder avoids overflowing before the division. The
// caller limits inputFrames so both the multiplication and final sum fit.
std::uint64_t roundedFrames(std::uint64_t frames, int sourceRate, int outputRate)
{
    const std::uint64_t source = static_cast<std::uint64_t>(sourceRate);
    const std::uint64_t output = static_cast<std::uint64_t>(outputRate);
    return (frames / source) * output
        + ((frames % source) * output + source / 2) / source;
}

float nativeFloat(const char* bytes)
{
    float value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

} // namespace

QsoPcmConverter::QsoPcmConverter(const Configuration& configuration)
    : m_configuration(configuration)
{
    const bool floatInput = configuration.sourceEncoding == InputEncoding::Float32Native;
    const bool pcmInput = configuration.sourceEncoding == InputEncoding::Int16Native
        || configuration.sourceEncoding == InputEncoding::Int16LittleEndian;
    const bool validOutput = configuration.outputEncoding == OutputEncoding::Int16LittleEndian
        || configuration.outputEncoding == OutputEncoding::Int16Native
        || configuration.outputEncoding == OutputEncoding::Float32Native;
    m_valid = (floatInput || pcmInput) && validOutput
        && supportedRate(configuration.sourceRate)
        && (!floatInput || configuration.sourceRate != 44100)
        && configuration.outputRate >= kMinOutputRate
        && configuration.outputRate <= kMaxOutputRate
        && supportedChannels(configuration.sourceChannels)
        && supportedChannels(configuration.outputChannels);
    if (!m_valid || configuration.sourceRate == configuration.outputRate) {
        return;
    }

    m_leftResampler = std::make_unique<Resampler>(configuration.sourceRate,
                                                 configuration.outputRate);
    m_rightResampler = std::make_unique<Resampler>(configuration.sourceRate,
                                                  configuration.outputRate);
    const int delay = m_leftResampler->groupDelayInputFrames();
    const int phasePeriod = configuration.sourceRate
        / std::gcd(configuration.sourceRate, configuration.outputRate);
    const int padding = (phasePeriod - delay % phasePeriod) % phasePeriod;
    // Resampler already prepended `delay` silent source frames. Round the
    // complete prefix UP to an exact rational output-frame boundary, instead
    // of rounding a fractional trim and shifting the finite file's content.
    // Padding is less than one source-rate second, only during construction;
    // its emitted silence is discarded here, so it adds no live backlog.
    m_skipOutputFrames = static_cast<std::uint64_t>(delay + padding)
        * configuration.outputRate / configuration.sourceRate;
    if (padding > 0) {
        const std::vector<float> silence(static_cast<std::size_t>(padding), 0.0f);
        const QByteArray left = m_leftResampler->process(silence.data(), padding);
        const QByteArray right = m_rightResampler->process(silence.data(), padding);
        if (left.size() != right.size()
            || static_cast<std::uint64_t>(left.size() / sizeof(float)) > m_skipOutputFrames) {
            m_valid = false;
            return;
        }
        m_skipOutputFrames -= static_cast<std::uint64_t>(left.size() / sizeof(float));
    }
}

QsoPcmConverter::~QsoPcmConverter() = default;

std::uint64_t QsoPcmConverter::targetOutputFrames() const noexcept
{
    if (!m_valid) {
        return 0;
    }
    return roundedFrames(m_inputFrames, m_configuration.sourceRate,
                         m_configuration.outputRate);
}

bool QsoPcmConverter::decode(QByteArrayView input, int& frames)
{
    const bool floatInput = m_configuration.sourceEncoding == InputEncoding::Float32Native;
    const int sampleBytes = floatInput ? sizeof(float) : sizeof(qint16);
    const int frameBytes = sampleBytes * m_configuration.sourceChannels;
    if (input.empty() || input.size() % frameBytes != 0
        || input.size() / frameBytes > kMaxInputFrames) {
        return false;
    }

    frames = static_cast<int>(input.size() / frameBytes);
    // The largest supported ratio is 8. This conservative ceiling also keeps
    // the rounded target and all cumulative counters representable.
    constexpr std::uint64_t kMaxSourceFrames = std::numeric_limits<std::uint64_t>::max() / 8;
    if (m_inputFrames > kMaxSourceFrames - static_cast<std::uint64_t>(frames)) {
        return false;
    }

    if (floatInput) {
        // Validate the complete block before changing histories or counters.
        for (qsizetype offset = 0; offset < input.size(); offset += sizeof(float)) {
            if (!std::isfinite(nativeFloat(input.data() + offset))) {
                return false;
            }
        }
    }

    m_leftInput.resize(static_cast<std::size_t>(frames));
    m_rightInput.resize(static_cast<std::size_t>(frames));
    for (int frame = 0; frame < frames; ++frame) {
        float samples[2];
        for (int channel = 0; channel < m_configuration.sourceChannels; ++channel) {
            const char* bytes = input.data() + frame * frameBytes + channel * sampleBytes;
            if (floatInput) {
                samples[channel] = std::clamp(nativeFloat(bytes), -1.0f, 1.0f);
            } else {
                qint16 value;
                if (m_configuration.sourceEncoding == InputEncoding::Int16LittleEndian) {
                    value = qFromLittleEndian<qint16>(bytes);
                } else {
                    std::memcpy(&value, bytes, sizeof(value));
                }
                samples[channel] = value / 32768.0f;
            }
        }
        m_leftInput[static_cast<std::size_t>(frame)] = samples[0];
        m_rightInput[static_cast<std::size_t>(frame)] =
            samples[m_configuration.sourceChannels == 1 ? 0 : 1];
    }
    return true;
}

bool QsoPcmConverter::process(QByteArrayView input, QByteArray& output)
{
    if (!m_valid || m_finished) {
        return false;
    }
    int frames = 0;
    if (!decode(input, frames)) {
        return false;
    }
    m_inputFrames += static_cast<std::uint64_t>(frames);
    if (!m_leftResampler) {
        return convert(m_leftInput.data(), m_rightInput.data(), frames, output);
    }

    QByteArray result;
    QByteArray block;
    int offset = 0;
    while (offset < frames) {
        const int count = std::min(frames - offset,
            kProcessFrames - static_cast<int>(m_leftPending.size()));
        m_leftPending.insert(m_leftPending.end(), m_leftInput.begin() + offset,
                             m_leftInput.begin() + offset + count);
        m_rightPending.insert(m_rightPending.end(), m_rightInput.begin() + offset,
                              m_rightInput.begin() + offset + count);
        offset += count;
        if (m_leftPending.size() == kProcessFrames) {
            if (!convert(m_leftPending.data(), m_rightPending.data(), kProcessFrames, block)) {
                return false;
            }
            result.append(block);
            m_leftPending.clear();
            m_rightPending.clear();
        }
    }
    output = std::move(result);
    return true;
}

bool QsoPcmConverter::convert(const float* left, const float* right, int frames,
                            QByteArray& output)
{
    if (!m_leftResampler) {
        pack(left, right, frames, output);
        m_outputFrames += static_cast<std::uint64_t>(frames);
        return true;
    }
    const QByteArray leftOutput = m_leftResampler->process(left, frames);
    const QByteArray rightOutput = m_rightResampler->process(right, frames);
    return packConverted(leftOutput, rightOutput, output);
}

bool QsoPcmConverter::packConverted(QByteArrayView left, QByteArrayView right,
                                  QByteArray& output)
{
    if (left.size() != right.size() || left.size() % sizeof(float) != 0) {
        discard();
        return false;
    }
    const std::uint64_t available = static_cast<std::uint64_t>(left.size() / sizeof(float));
    const std::uint64_t skip = std::min(m_skipOutputFrames, available);
    m_skipOutputFrames -= skip;
    const std::uint64_t remaining = targetOutputFrames() - m_outputFrames;
    const int frames = static_cast<int>(std::min(available - skip, remaining));
    std::vector<float> leftSamples(static_cast<std::size_t>(frames));
    std::vector<float> rightSamples(static_cast<std::size_t>(frames));
    const qsizetype skipBytes = static_cast<qsizetype>(skip * sizeof(float));
    if (frames > 0) {
        std::memcpy(leftSamples.data(), left.data() + skipBytes, frames * sizeof(float));
        std::memcpy(rightSamples.data(), right.data() + skipBytes, frames * sizeof(float));
    }
    pack(leftSamples.data(), rightSamples.data(), frames, output);
    m_outputFrames += static_cast<std::uint64_t>(frames);
    return true;
}

void QsoPcmConverter::pack(const float* left, const float* right, int frames,
                         QByteArray& output) const
{
    const bool floatOutput = m_configuration.outputEncoding == OutputEncoding::Float32Native;
    const int sampleBytes = floatOutput ? sizeof(float) : sizeof(qint16);
    output.resize(frames * m_configuration.outputChannels * sampleBytes);
    const float scale = m_configuration.sourceEncoding == InputEncoding::Float32Native
        ? 32767.0f : 32768.0f;
    for (int frame = 0; frame < frames; ++frame) {
        const float samples[2] = {
            m_configuration.outputChannels == 1
                ? left[frame] * 0.5f + right[frame] * 0.5f : left[frame],
            right[frame]
        };
        for (int channel = 0; channel < m_configuration.outputChannels; ++channel) {
            const float value = std::clamp(samples[channel], -1.0f, 1.0f);
            char* bytes = output.data()
                + (frame * m_configuration.outputChannels + channel) * sampleBytes;
            if (floatOutput) {
                std::memcpy(bytes, &value, sizeof(value));
            } else {
                const qint16 integer = static_cast<qint16>(
                    std::clamp(value * scale, -32768.0f, 32767.0f));
                if (m_configuration.outputEncoding == OutputEncoding::Int16LittleEndian) {
                    qToLittleEndian<qint16>(integer, bytes);
                } else {
                    std::memcpy(bytes, &integer, sizeof(integer));
                }
            }
        }
    }
}

bool QsoPcmConverter::finish(QByteArray& output)
{
    if (!m_valid || m_discarded) {
        return false;
    }
    if (m_finished || !m_leftResampler || m_inputFrames == 0) {
        m_finished = true;
        output.clear();
        return true;
    }
    QByteArray tail;
    QByteArray block;
    if (!m_leftPending.empty()) {
        if (!convert(m_leftPending.data(), m_rightPending.data(),
                     static_cast<int>(m_leftPending.size()), block)) {
            return false;
        }
        tail.append(block);
        m_leftPending.clear();
        m_rightPending.clear();
    }

    // Resampler::drain resets after feeding the input-domain group delay.
    // Multi-stage upsampling can still owe a few destination frames then.
    // Feed a bounded extra block without resetting, stopping at the exact
    // sample-derived target. Padding never becomes additional file duration.
    int silenceBudget = m_leftResampler->groupDelayInputFrames() + kProcessFrames;
    const std::vector<float> silence(kProcessFrames, 0.0f);
    while (m_outputFrames < targetOutputFrames() && silenceBudget > 0) {
        const int frames = std::min(silenceBudget, kProcessFrames);
        if (!convert(silence.data(), silence.data(), frames, block)) {
            return false;
        }
        tail.append(block);
        silenceBudget -= frames;
    }
    if (m_outputFrames != targetOutputFrames()) {
        discard();
        return false;
    }
    m_finished = true;
    output = std::move(tail);
    return true;
}

void QsoPcmConverter::discard() noexcept
{
    m_finished = true;
    m_discarded = true;
    m_leftResampler.reset();
    m_rightResampler.reset();
    m_leftInput.clear();
    m_rightInput.clear();
    m_leftPending.clear();
    m_rightPending.clear();
}

} // namespace AetherSDR
