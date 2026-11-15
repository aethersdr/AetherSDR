#include "DecoderPcmAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR {
bool DecoderPcmAdapter::selectRoute(RouteLane lane, int key)
{
    if (key < 0 || (lane != RouteLane::NativeSlice && lane != RouteLane::Dax
                    && lane != RouteLane::RxDemod)) {
        return false;
    }
    const Route route{lane, key};
    if (m_route != route) {
        reset();
        m_route = route;
    }
    return true;
}

void DecoderPcmAdapter::clearRoute()
{
    reset();
    m_route.reset();
}

void DecoderPcmAdapter::retireRoute(RouteLane lane, int key)
{
    const Route route{lane, key};
    for (Pin& pin : m_pins) {
        if (pin.route == route) {
            pin.retired = true;
        }
    }
    if (m_route == route) {
        clearRoute();
    }
}

void DecoderPcmAdapter::reset()
{
    m_segmentSource = {};
    m_segmentFirstInputSample = 0;
    m_inputEndSample = 0;
    m_outputSamples = 0;
    m_stagedFrames = 0;
    m_resampler.reset();
    m_converted.clear();
}

std::optional<DecoderPcmBlock> DecoderPcmAdapter::accept(const PcmFrame& frame)
{
    if (!m_route || !frame.current()) {
        return std::nullopt;
    }
    const PcmStreamDescriptor& stream = frame.stream();
    if ((m_route->lane == RouteLane::NativeSlice
         && (stream.purpose != PcmPurpose::Slice || stream.sliceId != m_route->key))
        || (m_route->lane == RouteLane::Dax && stream.purpose != PcmPurpose::Auxiliary)
        || (m_route->lane == RouteLane::RxDemod && stream.purpose != PcmPurpose::Speaker)) {
        return std::nullopt;
    }

    Pin* available = nullptr;
    Pin* matching = nullptr;
    for (Pin& pin : m_pins) {
        if (!pin.source.current()) {
            available = &pin;
        }
        if (pin.route != m_route) {
            continue;
        }
        const PcmStreamDescriptor& pinned = pin.source.stream();
        const bool sameReceiver = pinned.source == stream.source
            && pinned.session == stream.session
            && pinned.receiverInstance == stream.receiverInstance;
        if (pin.retired && sameReceiver) {
            return std::nullopt;
        }
        if (!pin.retired && pin.source.current() && !sameReceiver) {
            return std::nullopt;
        }
        if (!pin.retired && sameReceiver) {
            matching = &pin;
        }
    }
    Pin* destination = matching ? matching : available;
    if (!destination || !m_gate.accept(frame)) {
        return std::nullopt;
    }
    *destination = Pin{m_route, frame.epochLease(), false};

    const bool discontinuity = !m_segmentSource.current()
        || m_segmentSource.stream() != stream || frame.discontinuity()
        || frame.firstSample() != m_inputEndSample;
    if (discontinuity) {
        reset();
        m_segmentSource = frame.epochLease();
        m_segmentFirstInputSample = frame.firstSample();
        if (stream.format.sampleRateHz == 48000) {
            m_resampler = std::make_unique<Resampler>(48000, DecoderPcmBlock::kSampleRateHz,
                                                     kInputBatchFrames);
        }
    }
    m_inputEndSample = frame.firstSample() + static_cast<quint64>(frame.frameCount());
    DecoderPcmBlock block;
    block.source = frame.epochLease();
    block.segmentFirstInputSample = m_segmentFirstInputSample;
    block.inputEndSample = m_inputEndSample;
    block.firstOutputSample = m_outputSamples;
    block.inputSampleRateHz = stream.format.sampleRateHz;
    block.groupDelayInputFrames = m_resampler ? m_resampler->groupDelayInputFrames() : 0;
    block.discontinuity = discontinuity;
    block.samples.reserve(frame.frameCount());
    const int channels = stream.format.channels();
    for (qsizetype index = 0; index < frame.frameCount(); ++index) {
        const float sample = channels == 1 ? frame.samples()[index]
            : frame.samples()[2 * index] * 0.5f + frame.samples()[2 * index + 1] * 0.5f;
        if (!m_resampler) {
            block.samples.append(sample);
            continue;
        }
        m_staged[static_cast<std::size_t>(m_stagedFrames++)] = sample;
        if (m_stagedFrames == kInputBatchFrames) {
            const int converted = m_resampler->process(m_staged.data(), kInputBatchFrames,
                                                       m_converted);
            const qsizetype firstOutput = block.samples.size();
            block.samples.resize(firstOutput + converted);
            if (converted > 0) {
                std::memcpy(block.samples.data() + firstOutput, m_converted.constData(),
                            static_cast<std::size_t>(converted) * sizeof(float));
            }
            m_stagedFrames = 0;
        }
    }
    // Producer validation accepts every finite float. A linear-phase filter
    // can overshoot even those finite values; never poison a decoder with an
    // overflow created here. Send the reset immediately, even if overflow
    // persists indefinitely, so consumers retire their lock/history now.
    if (std::any_of(block.samples.cbegin(), block.samples.cend(),
                    [](float sample) { return !std::isfinite(sample); })) {
        block.samples.clear();
        block.discontinuity = true;
        block.firstOutputSample = 0;
        block.segmentFirstInputSample = frame.firstSample();
        reset();
    }
    if (!block.current()) {
        reset();
        return std::nullopt;
    }
    m_outputSamples += static_cast<quint64>(block.samples.size());
    return block;
}

} // namespace AetherSDR
