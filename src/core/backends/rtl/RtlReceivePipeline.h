#pragma once
#include "RtlAudioMixer.h"
#include "RtlCaptureTransaction.h"
#include "RtlReceiverRegistry.h"
#include "RtlSquelchGate.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace AetherSDR::rtl {
// One acquisition context and one serialized control owner. Preparation,
// service and stop are control operations. adopt/process/push are acquisition
// operations. A command mailbox must fence those two ownership domains.
class RtlReceivePipeline final : private RtlReceiverRegistry::BlockProcessor,
                                 private RtlReceiverRegistry::AudioSink,
                                 private RtlAudioMixer::Sink {
public:
    using Transaction = RtlCaptureTransaction;
    struct Diagnostics {
        bool observed = false;
        std::uint64_t droppedPackets = 0;
        std::uint64_t mixerLateFrames = 0;
        std::uint64_t mixerRejectedBlocks = 0;
        std::uint64_t mixerConfigurationFailures = 0;
        std::uint64_t droppedTraceEvents = 0;
    };
    // Independently sampled lifetime counters, not one atomic point-in-time
    // transaction. The control owner caches these for healthSnapshot().
    Diagnostics diagnostics() const noexcept;
    struct Packet {
        Transaction::Token token;
        std::uint64_t captureEpoch = 0;
        std::uint64_t instance = 0;
        std::uint64_t receiverEpoch = 0;
        std::uint64_t audioEpoch = 0; // nonzero for HD, separately retired on sync/audio loss
        std::uint64_t producedMonotonicMs = 0; // oldest decoded contribution; zero for intentional HD silence
        std::uint64_t firstSample = 0;
        int slot = -1;
        std::size_t frames = 0;
        int sampleRateHz = 48000;
        int channelCount = 2;
        bool discontinuity = false;
        // Present only on the independent WFM slice packet. Travels with the
        // same accepted session/revision and receiver epoch as its PCM.
        std::optional<bool> wfmStereoDetected;
        std::optional<AetherSDR::WfmReceptionDiagnostics> wfmReception;
        std::array<float, 2048> samples{};
    };
    struct HdFmObservation {
        Transaction::Token token;
        std::uint64_t captureEpoch = 0;
        std::uint64_t instance = 0;
        std::uint64_t receiverEpoch = 0;
        std::uint64_t audioEpoch = 0;
        int slot = -1;
        HdFmRawReception reception;
    };
    bool takeHdObservation(HdFmObservation& output) noexcept;
    // Bounded acquisition observations; no formatting, clock reads, allocation,
    // or logging on the sample callback. The owner drains across bank adoption
    // so repair cannot erase the fault that requested it.
    struct TraceEvent {
        enum class Kind { ReceiverFailure, MixerMissing };
        Kind kind = Kind::ReceiverFailure;
        Transaction::Token token;
        std::uint64_t hardwareGeneration = 0;
        std::uint64_t captureEpoch = 0;
        int slot = -1;
        int stableId = -1;
        std::uint64_t instance = 0;
        std::uint64_t receiverEpoch = 0;
        std::uint64_t captureFirst = 0;
        std::uint64_t captureFrames = 0;
        std::uint64_t captureClock = 0; // end of current capture block, at 48 kHz
        std::uint64_t quantumFirst = 0;
        RtlAudioMixer::MissingMask missingMask{};
        std::optional<RtlReceiverRegistry::ProcessingFailure> failure;
    };
    static constexpr unsigned kTraceEvents = 64;
    bool takeTraceEvent(TraceEvent& output) noexcept;
    // One internal build gate: qualification fixtures opt in explicitly. The
    // backend uses the same constant for its controls and legacy publication.
    static constexpr bool kQualifiedWfmEnabled = true;
    explicit RtlReceivePipeline(std::size_t capacity = 1,
                                bool enableWfm = kQualifiedWfmEnabled);
    enum class Submission { Accepted, RetryRetiringSlot, RetryPlannerBusy, Failed };
    Submission prepareDetailed(const Transaction::State& state, bool resetCapture = false,
                               bool verifiedRollback = false);
    bool prepare(const Transaction::State& state, bool resetCapture = false,
                 bool verifiedRollback = false)
    { return prepareDetailed(state, resetCapture, verifiedRollback) == Submission::Accepted; }
    enum class Preparation { Pending, Ready, Failed };
    Preparation service();
    bool adopt() noexcept;
    bool process(std::uint64_t firstSample, std::span<const std::complex<float>> samples) noexcept;
    // Acquisition only, before process() for this capture block. Shares the
    // display's existing FFT; the caller supplies capture-sample identity.
    void observeSpectrum(std::span<const float> bins, std::uint64_t firstSample) noexcept;
    void stop(); // after acquisition joined
    bool takePacket(Packet& output) noexcept;
    void setMonitor(int slot, int gain, int pan, bool mute) noexcept;
    bool needsRepair() const noexcept { return m_faults.load(std::memory_order_acquire) != 0; }
    std::uint64_t droppedPackets() const noexcept { return m_drops.load(std::memory_order_relaxed); }
    bool legacy() const noexcept { return m_legacy; }
private:
    friend struct RtlReceivePipelineTestAccess;
    void process(const RtlReceiverRegistry::SampleBlock&, std::span<const RtlReceiverRegistry::ReceiverView>) noexcept override;
    void audioBlock(const RtlReceiverRegistry::ReceiverSpec&, std::uint64_t,
                    std::span<const float>, std::span<const float>, bool) noexcept override;
    void audioBlockWithStatus(const RtlReceiverRegistry::ReceiverSpec&, std::uint64_t,
                    std::span<const float>, std::span<const float>, bool, std::optional<AetherSDR::WfmReceptionDiagnostics>) noexcept override;
    void speakerBlock(std::uint64_t, std::span<const float>, bool) noexcept override;
    bool enqueue(const Packet&) noexcept;
    void missingFrames(const RtlAudioMixer::Input&, std::uint64_t, std::uint64_t,
                       const RtlAudioMixer::MissingMask&) noexcept override;
    void enqueueTrace(const TraceEvent&) noexcept;
    TraceEvent traceContext() const noexcept;
    const bool m_enableWfm;
    RtlReceiverRegistry m_registry;
    RtlReceiverRegistry::SampleReader m_reader;
    std::uint64_t m_session = 0;
    std::uint64_t m_prepared = 0;
    Transaction::Token m_nextToken;
    RtlReceiverRegistry::Capture m_nextCapture;
    std::uint64_t m_nextEpoch = 1;
    bool m_nextLegacy = true;
    std::uint8_t m_nextReceivingMask = 0;
    std::array<std::optional<RtlReceiverRegistry::Handle>, 8> m_handles;
    std::array<RtlReceiverRegistry::ReceiverSpec, 8> m_specs;
    std::array<std::uint64_t, 8> m_epochs{};
    // Acquisition-only values, copied by adopt() after the mailbox fence.
    Transaction::Token m_token;
    RtlReceiverRegistry::Capture m_capture;
    std::uint64_t m_captureEpoch = 0;
    bool m_legacy = true;
    std::uint8_t m_receivingMask = 0;
    RtlAudioMixer m_mixer;
    void hdObservation(const RtlReceiverRegistry::ReceiverSpec&, const HdFmRawReception&) noexcept override;
    void hdDecodedAudio(const RtlReceiverRegistry::ReceiverSpec&, const HdFmAudioIdentity&, std::uint64_t,
        std::span<const float>, std::span<const float>, bool) noexcept override;
    void hdSpeakerAudio(const RtlReceiverRegistry::ReceiverSpec&, const HdFmAudioIdentity&, std::uint64_t,
        std::span<const float>, std::span<const float>, bool) noexcept override;
    static constexpr unsigned kHdObservations = 64;
    std::array<HdFmObservation, kHdObservations> m_hdObservations;
    alignas(64) std::atomic<unsigned> m_hdWrite{0};
    alignas(64) std::atomic<unsigned> m_hdRead{0};
    std::uint64_t m_hdSpeakerEpoch = 0;
    std::array<HdFmAudioIdentity, RtlAudioMixer::kCapacity> m_hdSpeakerIdentities{};
    bool m_hdAudioValid = false;
    // Set on the first capture block of a new mixer epoch, then retained.
    // Unchanged epochs must keep their existing sample coordinate and buffers.
    std::optional<std::uint64_t> m_mixerOrigin;
    std::array<int, 8> m_traceStableIds{};
    std::uint64_t m_traceCaptureFirst = 0;
    std::uint64_t m_traceCaptureFrames = 0;
    std::uint64_t m_traceCaptureClock = 0;
    // Independent SPSC trace queue: overflow is counted and never delays PCM.
    std::array<TraceEvent, kTraceEvents> m_traceEvents;
    alignas(64) std::atomic<unsigned> m_traceWrite{0};
    alignas(64) std::atomic<unsigned> m_traceRead{0};
    std::atomic<std::uint64_t> m_traceDrops{0};
    std::atomic<unsigned> m_faults{0};
    std::array<std::atomic<unsigned>, 8> m_monitor;
    std::array<unsigned, 8> m_nextMonitor{};
    struct SquelchConfig { bool enabled = false; int level = 20; };
    std::array<SquelchConfig, 8> m_nextSquelch{};
    std::array<SquelchConfig, 8> m_squelchConfig{};
    std::array<RtlSquelchGate, 8> m_squelch;
    std::array<std::uint64_t, 8> m_squelchEpoch{};
    std::array<float, 2048> m_spectrum{};
    std::uint64_t m_spectrumFirstSample = 0;
    bool m_spectrumFresh = false;
    // SPSC queue. Overflow drops the new packet; the consumer observes the
    // sample-position gap and marks its next typed frame discontinuous.
    static constexpr unsigned kPackets = 128;
    std::array<Packet, kPackets> m_packets;
    alignas(64) std::atomic<unsigned> m_write{0};
    alignas(64) std::atomic<unsigned> m_read{0};
    std::atomic<std::uint64_t> m_drops{0};
    std::atomic<std::uint64_t> m_mixerLate{0};
    std::atomic<std::uint64_t> m_mixerRejected{0};
    std::atomic<std::uint64_t> m_mixerConfigurationFailures{0};
    std::atomic<bool> m_observed{false};
};
} // namespace AetherSDR::rtl
