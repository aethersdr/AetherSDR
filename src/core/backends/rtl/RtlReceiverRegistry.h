#pragma once

#include "core/SharedCapturePolicy.h"
#include "core/dsp/WdspChannel.h"

#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace AetherSDR::rtl {

// RFC #5468 F4. Compiled engine foundation; RtlSdrBackend keeps its existing
// DDC until M1 supplies RF extraction and the rate-aware audio consumers.
class RtlReceiverRegistry final
{
    struct State;
    struct Executor;

public:
    // Storage ceilings, NOT advertised/per-architecture receiver counts.
    static constexpr std::size_t kMaxSlots = 8;
    static constexpr std::size_t kMaxRegistries = 4;
    static constexpr std::size_t kMaxInputSamples = 65536;

    using Capture = SharedCapturePolicy::CaptureDescriptor;
    struct Limits {
        std::size_t slotCount = 0;
        std::size_t receiverCapacity = 0;
        // Includes preparing, offered, active AND acknowledged-but-not-freed
        // receivers. Replacements must fit old + new before old is withdrawn.
        std::size_t residentReceiverCapacity = 0;
    };
    struct Handle {
        std::uint64_t session = 0;
        std::uint64_t instance = 0;
        std::int32_t slot = -1;
        bool operator==(const Handle&) const = default;
    };
    struct ReceiverSpec {
        Handle handle;
        SharedCapturePolicy::SliceDescriptor passband;
        WdspChannel::Config dsp;
    };

    // Prepared fixed-block DSP, with no allocating setters exposed. M1 owns
    // the RF extraction/conversion feeding these exact-size planar blocks.
    // Returned output spans are borrowed until the next processIq() call.
    class Receiver {
    public:
        virtual ~Receiver() = default;
        virtual WdspChannel::ProcessResult processIq(std::span<const float> i,
            std::span<const float> q) noexcept = 0;
        virtual std::span<const float> left() const noexcept = 0;
        virtual std::span<const float> right() const noexcept = 0;
    };
    // Injection is for deterministic preparation failures/delays. The default
    // constructs real WDSP channels and output buffers on the Qt worker pool.
    // Called only there; must own its dependencies, never capture a backend.
    using Prepare = std::function<std::unique_ptr<Receiver>(
        const ReceiverSpec&, WdspChannel::Reservation&, std::string&)>;

    struct ReceiverView {
        const ReceiverSpec* spec = nullptr;
        Receiver* receiver = nullptr;
    };
    struct SampleBlock {
        // Token returned by beginSession(), captured by the acquisition owner.
        // Never relabel queued/old IQ by reading the registry's current token.
        std::uint64_t session = 0;
        Capture capture;
        std::uint64_t firstSample = 0;
        bool discontinuity = false;
        std::span<const std::complex<float>> samples;
    };
    class BlockProcessor {
    public:
        virtual ~BlockProcessor() = default;
        // Views and IQ may only be used during this call. This is the one
        // acquisition context; no references may escape it or run concurrently.
        virtual void process(const SampleBlock& block,
            std::span<const ReceiverView> receivers) noexcept = 0;
    };

    class SampleReader final {
    public:
        SampleReader() = default;
        SampleReader(SampleReader&& other) noexcept;
        SampleReader& operator=(SampleReader&& other) noexcept;
        ~SampleReader();
        SampleReader(const SampleReader&) = delete;
        SampleReader& operator=(const SampleReader&) = delete;
        // No locks, allocation, shared_ptr copies/releases, or destruction.
        // Publication and explicit retirement acknowledgment occur at entry.
        bool processBlock(const SampleBlock& block, BlockProcessor& processor) noexcept;
        // Call AFTER the acquisition context has stopped, outside its callback.
        // This is the acknowledgment path when no next sample block will arrive.
        void stop();
        explicit operator bool() const noexcept { return static_cast<bool>(m_state); }

    private:
        friend class RtlReceiverRegistry;
        explicit SampleReader(std::shared_ptr<State> state);
        std::shared_ptr<State> m_state;
        int m_active = -1; // exclusively owned by the acquisition context
        std::uint64_t m_nextSample = 0;
        bool m_continuous = false;
    };

    enum class Result {
        Accepted, Invalid, Busy, NoSlot, ResourceLimit, PreparationFailed, Closed
    };
    struct Status {
        Result result = Result::Closed;
        std::uint64_t session = 0;
        std::uint64_t requested = 0;
        std::uint64_t prepared = 0;
        std::uint64_t published = 0;
        std::size_t residentReceivers = 0;
        bool preparing = false;
        bool pending = false;
        std::string error;
    };

    explicit RtlReceiverRegistry(Limits limits, Prepare prepare = {});
    ~RtlReceiverRegistry();
    RtlReceiverRegistry(const RtlReceiverRegistry&) = delete;
    RtlReceiverRegistry& operator=(const RtlReceiverRegistry&) = delete;
    explicit operator bool() const noexcept { return static_cast<bool>(m_state); }

    // Control-side calls are serialized by the process-wide executor. They
    // may run on the model/control thread, never in processBlock(). A reader
    // may move, stop or destruct only before acquisition starts or after its
    // thread has joined; these operations must not race processBlock(). stop()
    // cancels the session, including pending/running preparation results.
    // One
    // executing task + one pending complete desired set across ALL owners.
    // A pending set from another owner returns Busy; it is never overwritten.
    std::uint64_t beginSession(const Capture& capture);
    std::optional<Handle> reserveSlot(std::optional<int> preferred = std::nullopt);
    Result cancelReservation(Handle handle);
    Result submit(const Capture& capture, std::span<const ReceiverSpec> desired);
    void cancelSession();
    SampleReader attachReader(); // at most one, acquired off the sample path
    // Call from the control event loop while active. Reaps acknowledged banks
    // off-thread and advances a coalesced request after publication pressure.
    Status service();

private:
    static std::shared_ptr<Executor> executor();
    std::shared_ptr<State> m_state;
};

} // namespace AetherSDR::rtl
