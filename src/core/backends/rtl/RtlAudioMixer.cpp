#include "RtlAudioMixer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR::rtl {
bool RtlAudioMixer::configure(std::uint64_t session, std::uint64_t captureGeneration,
    std::span<const Input> inputs, std::uint64_t firstSample) noexcept
{
    if (!session || !captureGeneration || inputs.size() > kSlots) { return false; }
    std::array<bool, kSlots> used{};
    for (const Input& input : inputs) {
        if (input.slot < 0 || static_cast<std::size_t>(input.slot) >= kSlots || used[input.slot]
            || !input.instance || !std::isfinite(input.gain) || input.gain < 0 || input.gain > 1
            || !std::isfinite(input.pan) || input.pan < 0 || input.pan > 1) { return false; }
        used[input.slot] = true;
    }
    const bool reset = session != m_session || captureGeneration != m_capture;
    if (reset) {
        m_session = session; m_capture = captureGeneration; m_next = firstSample; m_first = true;
    }
    for (const Input& input : inputs) {
        Slot& slot = m_slots[input.slot];
        if (reset || !slot.active || slot.input.instance != input.instance || slot.input.epoch != input.epoch) {
            for (Frame& frame : slot.frames) { frame.valid = false; }
        }
        slot.input = input;
    }
    for (std::size_t i = 0; i < kSlots; ++i) { m_slots[i].active = used[i]; }
    m_count = inputs.size();
    return true;
}
void RtlAudioMixer::reset() noexcept
{
    m_session = 0; m_capture = 0; m_count = 0; m_first = true;
    for (Slot& slot : m_slots) { slot.active = false; }
}
bool RtlAudioMixer::push(int index, std::uint64_t instance, std::uint64_t epoch,
    std::uint64_t firstSample, std::span<const float> left, std::span<const float> right) noexcept
{
    if (index < 0 || static_cast<std::size_t>(index) >= kSlots || !m_slots[index].active
        || m_slots[index].input.instance != instance || m_slots[index].input.epoch != epoch
        || left.empty() || left.size() != right.size() || left.size() > kCapacity
        || !left.data() || !right.data()
        || firstSample > std::numeric_limits<std::uint64_t>::max() - left.size()
        || firstSample + left.size() <= m_next
        || firstSample + left.size() - m_next > kCapacity
        || !std::ranges::all_of(left, [](float value) { return std::isfinite(value); })
        || !std::ranges::all_of(right, [](float value) { return std::isfinite(value); })) {
        ++m_rejected; return false;
    }
    Slot& slot = m_slots[index];
    for (std::size_t i = 0; i < left.size(); ++i) {
        const std::uint64_t position = firstSample + i;
        if (position < m_next) { continue; }
        Frame& frame = slot.frames[position % kCapacity];
        // Duplicate delivery cannot overwrite already accepted audio.
        if (frame.valid && frame.position == position) { continue; }
        frame = {position, left[i], right[i], true};
    }
    return true;
}
void RtlAudioMixer::drain(std::uint64_t captureClock, Sink& sink) noexcept
{
    if (!m_session || !m_count) { return; }
    for (std::size_t emitted = 0; emitted < kCapacity / kQuantum; ++emitted) {
        if (m_next > std::numeric_limits<std::uint64_t>::max() - kQuantum - kDeadlineFrames) { return; }
        bool complete = true;
        for (const Slot& slot : m_slots) {
            if (!slot.active) { continue; }
            for (std::size_t i = 0; i < kQuantum; ++i) {
                const Frame& frame = slot.frames[(m_next + i) % kCapacity];
                complete &= frame.valid && frame.position == m_next + i;
            }
        }
        if (!complete && captureClock < m_next + kQuantum + kDeadlineFrames) { return; }
        m_output.fill(0);
        for (Slot& slot : m_slots) {
            if (!slot.active) { continue; }
            // Fixed summation headroom, independent of sample values. No
            // automatic level matching/normalization (S2) is introduced here.
            const float level = slot.input.mute ? 0 : slot.input.gain / static_cast<float>(m_count);
            const float leftGain = level * std::min(1.0f, 2 * (1 - slot.input.pan));
            const float rightGain = level * std::min(1.0f, 2 * slot.input.pan);
            for (std::size_t i = 0; i < kQuantum; ++i) {
                Frame& frame = slot.frames[(m_next + i) % kCapacity];
                if (frame.valid && frame.position == m_next + i) {
                    m_output[2 * i] += frame.left * leftGain;
                    m_output[2 * i + 1] += frame.right * rightGain;
                    frame.valid = false;
                } else { ++m_late; }
            }
        }
        for (float& value : m_output) { value = std::clamp(value, -1.0f, 1.0f); }
        sink.speakerBlock(m_next, m_output, m_first);
        m_next += kQuantum;
        m_first = false;
    }
}
} // namespace AetherSDR::rtl
