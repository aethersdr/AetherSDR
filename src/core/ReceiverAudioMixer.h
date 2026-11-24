#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace AetherSDR {

// Sums the demodulated audio of receivers that share one sample clock into the
// one interleaved-stereo speaker stream, for a backend that demodulates on this
// host. Each input arrives in its own block sizes at its own moments, so it is
// queued, and the mix advances as far as every contributing input reaches.
// Family-neutral; the ANAN backend is its first user (#5554 §2.5).
class ReceiverAudioMixer {
public:
    static constexpr std::size_t kMaxInputs = 8;

    // How far, in stereo frames, one input may run ahead of another before the
    // one behind is mixed as silence, so a stalled receiver cannot hold the
    // others silent.
    explicit ReceiverAudioMixer(std::size_t maxLagFrames);

    // Blocks pushed for an inactive input are ignored. An input activated here
    // holds nothing back until its first block; deactivating one drops its
    // queue. Out-of-range inputs are ignored.
    void setInputActive(int input, bool active);
    [[nodiscard]] bool isInputActive(int input) const noexcept;

    // Interleaved L,R; a trailing half frame is dropped. Returns the mixed
    // frames ready now, empty while an input is still awaited. Valid until the
    // next push(). With one contributing input and nothing queued the block
    // itself is returned, unchanged; a sum of several is clamped to +/-1.
    //
    // A lag between inputs that outlasts a stall (one input lost blocks the
    // others did not) would otherwise stay for good, as latency on the others.
    // Once every input has delivered for maxLagFrames of mixed audio and one
    // has stayed more than a block ahead all that time, its oldest frames are
    // dropped down to the others'. A lag of a block or less is the inputs'
    // ordinary skew and is kept.
    [[nodiscard]] std::span<const float> push(int input, std::span<const float> interleaved);

    // Empties every queue and awaits each active input's first block again.
    void reset() noexcept;

    // Stereo frames queued for one input, waiting for the others; 0 for an
    // inactive or out-of-range input.
    [[nodiscard]] std::size_t queuedFrames(int input) const noexcept;

private:
    static constexpr std::size_t kUnmeasured = static_cast<std::size_t>(-1);
    struct Input {
        bool active = false;
        bool primed = false;        // delivered since activation or reset
        std::vector<float> queue;   // interleaved, oldest first
        // The fewest frames left queued after any mix in the current window.
        std::size_t leastQueuedFrames = kUnmeasured;
    };
    [[nodiscard]] bool contributes(const Input& in) const noexcept
    {
        return in.active && in.primed;
    }
    // After a mix that took every frame from the inputs' own queues.
    void trackPersistentLag(std::size_t mixedFrames);
    void restartLagWindow() noexcept;

    std::array<Input, kMaxInputs> m_inputs;
    std::size_t m_maxLagFrames;
    std::vector<float> m_out;
    // Mixed frames in the current window, and the largest block any input has
    // pushed, which is the skew kept.
    std::size_t m_windowFrames = 0;
    std::size_t m_largestBlockFrames = 0;
};

}  // namespace AetherSDR
