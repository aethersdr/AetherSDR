#pragma once

#include "HdFmTypes.h"
#include <memory>
#include <span>

struct nrsc5_event_t;

namespace AetherSDR::rtl {
// Serialized decoder-thread owner. No device/tuner API is exposed.
class Nrsc5FmDecoder final {
public:
    class Sink {
    public:
        virtual ~Sink() = default;
        virtual bool decoded(const HdFmAudioIdentity& identity, std::uint64_t first,
            std::span<const float> left, std::span<const float> right) noexcept = 0;
        virtual void audioRetired(std::uint64_t epoch) noexcept = 0;
    };
    // The injectable serialized pipe is the socket-free event-test seam. Its
    // callback contract is the pinned upstream C API, including borrowed data.
    class Pipe {
    public:
        using Callback = void (*)(const ::nrsc5_event_t*, void*);
        virtual ~Pipe() = default;
        virtual bool valid() const noexcept = 0;
        virtual bool feed(std::span<const float>, Callback, void*) noexcept = 0;
    };
    Nrsc5FmDecoder(HdFmRawReception initial, Sink& sink, std::unique_ptr<Pipe> pipe = {});
    ~Nrsc5FmDecoder();
    bool valid() const noexcept;
    bool feed(std::span<const float> interleaved, std::uint64_t session,
              std::uint64_t revision) noexcept;
    const HdFmRawReception& reception() const noexcept;
    void withdraw() noexcept;
    void expireAudio() noexcept;
    void resetAudio() noexcept;
private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace AetherSDR::rtl
