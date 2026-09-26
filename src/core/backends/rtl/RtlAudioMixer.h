#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace AetherSDR::rtl {
// Acquisition-owned, bounded 48 kHz stereo mixer. Slot taps precede this
// component. Silence fills expired holes; a late receiver cannot move the
// speaker clock or hold back a healthy sibling. No allocations after creation.
class RtlAudioMixer final {
public:
    static constexpr std::size_t kSlots = 8;
    static constexpr std::size_t kCapacity = 8192;
    static constexpr std::size_t kQuantum = 128;
    static constexpr std::uint64_t kDeadlineFrames = 2048;
    struct Input {
        int slot = -1;
        std::uint64_t instance = 0;
        std::uint64_t epoch = 0;
        float gain = 1;
        float pan = 0.5f;
        bool mute = false;
    };
    class Sink {
    public:
        virtual ~Sink() = default;
        virtual void speakerBlock(std::uint64_t firstSample,
            std::span<const float> interleaved, bool discontinuity) noexcept = 0;
    };
    bool configure(std::uint64_t session, std::uint64_t captureGeneration,
                   std::span<const Input> inputs, std::uint64_t firstSample) noexcept;
    void reset() noexcept; // discard buffered audio; retain lifetime diagnostics
    bool push(int slot, std::uint64_t instance, std::uint64_t epoch,
              std::uint64_t firstSample, std::span<const float> left,
              std::span<const float> right) noexcept;
    void drain(std::uint64_t captureClock, Sink& sink) noexcept;
    std::uint64_t nextSample() const noexcept { return m_next; }
    std::uint64_t lateFrames() const noexcept { return m_late; }
    std::uint64_t rejectedBlocks() const noexcept { return m_rejected; }
private:
    struct Frame { std::uint64_t position = 0; float left = 0; float right = 0; bool valid = false; };
    struct Slot { Input input; bool active = false; std::array<Frame, kCapacity> frames; };
    std::array<Slot, kSlots> m_slots;
    std::array<float, 2 * kQuantum> m_output{};
    std::uint64_t m_session = 0;
    std::uint64_t m_capture = 0;
    std::uint64_t m_next = 0;
    std::uint64_t m_late = 0;
    std::uint64_t m_rejected = 0;
    std::size_t m_count = 0;
    bool m_first = true;
};
} // namespace AetherSDR::rtl
