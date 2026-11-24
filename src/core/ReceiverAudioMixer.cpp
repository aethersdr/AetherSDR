#include "core/ReceiverAudioMixer.h"

#include <algorithm>
#include <limits>

namespace AetherSDR {

ReceiverAudioMixer::ReceiverAudioMixer(std::size_t maxLagFrames)
    : m_maxLagFrames(maxLagFrames)
{
}

void ReceiverAudioMixer::setInputActive(int input, bool active)
{
    if (input < 0 || static_cast<std::size_t>(input) >= kMaxInputs) {
        return;
    }
    Input& in = m_inputs[static_cast<std::size_t>(input)];
    if (in.active == active) {
        return;
    }
    in.active = active;
    in.primed = false;
    in.queue.clear();
    restartLagWindow();
}

bool ReceiverAudioMixer::isInputActive(int input) const noexcept
{
    return input >= 0 && static_cast<std::size_t>(input) < kMaxInputs
        && m_inputs[static_cast<std::size_t>(input)].active;
}

std::span<const float> ReceiverAudioMixer::push(int input, std::span<const float> interleaved)
{
    if (input < 0 || static_cast<std::size_t>(input) >= kMaxInputs) {
        return {};
    }
    Input& in = m_inputs[static_cast<std::size_t>(input)];
    if (!in.active) {
        return {};
    }
    // An odd count would swap the channels of everything queued after it.
    const std::span<const float> frames = interleaved.first(interleaved.size() & ~std::size_t{1});
    in.primed = true;
    m_largestBlockFrames = std::max(m_largestBlockFrames, frames.size() / 2);

    std::size_t contributors = 0;
    for (const Input& other : m_inputs) {
        contributors += contributes(other) ? 1 : 0;
    }
    if (contributors == 1 && in.queue.empty()) {
        return frames;
    }
    in.queue.insert(in.queue.end(), frames.begin(), frames.end());

    std::size_t shallowest = std::numeric_limits<std::size_t>::max();
    std::size_t deepest = 0;
    for (const Input& other : m_inputs) {
        if (contributes(other)) {
            shallowest = std::min(shallowest, other.queue.size() / 2);
            deepest = std::max(deepest, other.queue.size() / 2);
        }
    }
    // As far as every input reaches, or past an input that has fallen more
    // than the lag behind, which is mixed as silence for the difference.
    const std::size_t n = std::max(shallowest,
                                   deepest > m_maxLagFrames ? deepest - m_maxLagFrames : 0);
    if (n == 0) {
        return {};
    }

    m_out.assign(2 * n, 0.0f);
    for (Input& other : m_inputs) {
        if (!contributes(other)) {
            continue;
        }
        const std::size_t take = 2 * std::min(n, other.queue.size() / 2);
        for (std::size_t k = 0; k < take; ++k) {
            m_out[k] += other.queue[k];
        }
        other.queue.erase(other.queue.begin(),
                          other.queue.begin() + static_cast<std::ptrdiff_t>(take));
    }
    // An input mixed as silence is the stall rule's to handle; only a mix
    // that every input filled from its own queue shows a lag that persists.
    if (n > shallowest) {
        restartLagWindow();
    } else {
        trackPersistentLag(n);
    }
    // Clamped, not scaled by the input count: opening a second receiver must
    // not make the first quieter.
    if (contributors > 1) {
        for (float& s : m_out) {
            s = std::clamp(s, -1.0f, 1.0f);
        }
    }
    return m_out;
}

void ReceiverAudioMixer::trackPersistentLag(std::size_t mixedFrames)
{
    for (Input& in : m_inputs) {
        if (contributes(in)) {
            in.leastQueuedFrames = std::min(in.leastQueuedFrames, in.queue.size() / 2);
        }
    }
    m_windowFrames += mixedFrames;
    if (m_windowFrames < m_maxLagFrames) {
        return;
    }
    // The input that has been behind at some mix ends each one empty; one
    // that never got below more than a block for the whole window holds a
    // lag the stall left, and drops it, oldest first.
    for (Input& in : m_inputs) {
        if (contributes(in) && in.leastQueuedFrames != kUnmeasured
            && in.leastQueuedFrames > m_largestBlockFrames) {
            in.queue.erase(in.queue.begin(),
                           in.queue.begin() + static_cast<std::ptrdiff_t>(2 * in.leastQueuedFrames));
        }
    }
    restartLagWindow();
}

void ReceiverAudioMixer::restartLagWindow() noexcept
{
    m_windowFrames = 0;
    for (Input& in : m_inputs) {
        in.leastQueuedFrames = kUnmeasured;
    }
}

void ReceiverAudioMixer::reset() noexcept
{
    for (Input& in : m_inputs) {
        in.primed = false;
        in.queue.clear();
    }
    m_largestBlockFrames = 0;
    restartLagWindow();
}

std::size_t ReceiverAudioMixer::queuedFrames(int input) const noexcept
{
    if (input < 0 || static_cast<std::size_t>(input) >= kMaxInputs) {
        return 0;
    }
    const Input& in = m_inputs[static_cast<std::size_t>(input)];
    return in.active ? in.queue.size() / 2 : 0;
}

}  // namespace AetherSDR
