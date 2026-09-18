#pragma once

#include <QByteArray>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace AetherSDR {

class ClientComp;
class ClientDeEss;
class ClientEq;
class ClientFinalLimiter;
class ClientGate;
class ClientPudu;
class ClientQuindarTone;
class ClientReverb;
class ClientTube;
class ClientTxTestTone;
class RNNoiseFilter;
class Resampler;
class TxVoiceProcessorTestAccess;

// Headless, backend-independent TX voice rate-domain processor. AudioEngine
// remains responsible for capture normalization, mode routing, metering, and
// transport. This class makes the canonical 48 kHz float DSP island explicit
// and returns the existing 24 kHz stereo int16 voice-output representation.
// It is the final PCM boundary for Flex remote_audio_tx; host-modulating
// backends currently consume the same seam and may convert onward.
class TxVoiceProcessor {
public:
    static constexpr int kDspRate = 48000;
    static constexpr int kTransportRate = 24000;
    static constexpr int kChannels = 2;
    static constexpr int kMaxStages = 8;
    // Voice only: preserve the supported 10 kHz modulation passband while
    // avoiding the delay of r8brain's general-purpose 2% transition profile.
    static constexpr double kVoiceSrcTransitionBandPercent = 12.0;

    enum class Stage : uint8_t {
        None = 0,
        Gate = 1,
        Eq = 2,
        DeEss = 3,
        Comp = 4,
        Tube = 5,
        Enh = 6,
        Reverb = 7,
    };

    struct Processors {
        using EqTap = void (*)(void* context, const float* stereo, int frames);

        ClientEq* eq{nullptr};
        ClientComp* comp{nullptr};
        ClientGate* gate{nullptr};
        ClientDeEss* deEss{nullptr};
        ClientTube* tube{nullptr};
        ClientPudu* pudu{nullptr};
        ClientReverb* reverb{nullptr};
        ClientFinalLimiter* finalLimiter{nullptr};
        ClientTxTestTone* testTone{nullptr};
        // RNNoiseFilter is deliberately absent: it is the one processor whose
        // owner can retire it while the audio thread is running, so it is
        // published through setRnnoise() alone rather than copied in as part
        // of a struct assignment.
        ClientQuindarTone* quindar{nullptr};
        EqTap eqTap{nullptr};
        void* eqTapContext{nullptr};
    };

    TxVoiceProcessor();
    ~TxVoiceProcessor();

    TxVoiceProcessor(const TxVoiceProcessor&) = delete;
    TxVoiceProcessor& operator=(const TxVoiceProcessor&) = delete;

    // Call outside the realtime callback when the capture format changes.
    // maxInputFrames sizes the normal realtime working set; an unusually
    // large capture callback is still processed rather than discarded.
    bool prepare(int inputRate, int maxInputFrames);
    void reset();

    void setProcessors(const Processors& processors) noexcept;
    void setStageOrder(uint64_t packedStages) noexcept;
    void setRnnoiseEnabled(bool enabled) noexcept;
    // Non-owning association, published with release semantics because the
    // owner writes it from its own thread while the audio thread reads it
    // every block. The owner must pass nullptr before the RNNoiseFilter can
    // stop being reachable; AudioEngine does that at its ownership seam and
    // then keeps the object alive, so a racing acquire load that still sees
    // the old pointer dereferences a live filter rather than freed memory.
    void setRnnoise(RNNoiseFilter* rnnoise) noexcept;
    void setMicGain(float gain) noexcept;
    void setMeasurementCaptureEnabled(bool enabled) noexcept;

    // Input must already be canonical duplicated-stereo int16. The channel
    // selection/averaging policy remains in TxMicChannelNormalizer. On
    // success, the consumed capture block is replaced in-place with the final
    // 24 kHz stereo int16 transport block. The caller retains ownership, so a
    // queued consumer can keep that immutable block without sharing storage
    // that this processor will write on the next callback.
    bool processCapturedInt16(QByteArray& canonicalInputOutput);

    // Float32 sibling of processCapturedInt16(). Input is canonical duplicated
    // mono stereo float32 at the negotiated device rate (what
    // TxMicChannelNormalizer::canonicalizeFloat32ToMonoStereo produces); output
    // is the same transport Int16 the Int16 route returns, so nothing
    // downstream of this call has to know which one ran.
    //
    // Non-finite input samples are replaced with silence on the way in — unlike
    // the Int16 route, this one CAN carry NaN/Inf, and the ingress resampler it
    // feeds is stateful. No caller precondition: sanitizing here is what keeps
    // that guarantee from depending on a different file's invariant.
    bool processCapturedFloat32(QByteArray& canonicalInputOutput);

    // Offline/test entry point for audio already in the canonical DSP domain.
    // Input is interleaved stereo float32 at exactly 48 kHz. The caller owns
    // the returned 24 kHz stereo int16 transport block.
    bool processFloat48(const float* interleavedStereo,
                        int frames,
                        QByteArray& transportInt16Output);

    // A matched egress pair should always return equal frame counts. If it
    // does not, processing preserves the aligned common prefix and raises this
    // request. The owner must service it between realtime callbacks; resetting
    // r8brain from processWorkBuffer() would violate Resampler's contract.
    bool egressRecoveryPending() const noexcept
    {
        return m_egressRecoveryPending;
    }
    void recoverEgressAfterMismatch();

    const QByteArray& transportFloat32Stereo() const noexcept;
    const QByteArray& normalizedFloat48Stereo() const noexcept;
    const QByteArray& postChannelStripFloat48Stereo() const noexcept;

    int inputRate() const noexcept { return m_inputRate; }
    bool isPrepared() const noexcept { return m_prepared; }

    // Configured-path delay expressed in 48 kHz DSP frames. Includes exact
    // serial ingress/egress SRC group delay, RNNoise's nominal one-frame delay,
    // and enabled gate lookahead. The matched L/R egress SRCs run in parallel
    // and therefore contribute one delay. RNNoise's callback-sized streaming
    // adapter can add startup silence for irregular block partitions, so its
    // term is not an exact wall-clock guarantee. Reverb pre-delay is an
    // artistic wet-path parameter rather than whole-signal latency.
    int latencyFrames() const noexcept;

private:
    friend class TxVoiceProcessorTestAccess;

    static constexpr uint64_t kDitherSeed = 0x6A09E667F3BCC909ULL;

    // Shared tail of both capture routes: m_inputMono already holds
    // inputFrames of mono float at the device rate.
    bool processCapturedMono(int inputFrames, QByteArray& transportInt16Output);
    void processChannelStrip(QByteArray& float48Stereo) noexcept;
    bool processWorkBuffer(int frames48, QByteArray& transportInt16Output);
    int reconcileEgressFrameCounts(int leftFrames, int rightFrames);
    void prepareProcessors();
    uint32_t nextDitherRandom24() noexcept;
    float nextTpdfDitherLsb() noexcept;
    static int16_t quantizeTransportSample(
        float sample, float ditherLsb) noexcept;

    int m_inputRate{kDspRate};
    int m_maxInputFrames{0};
    int m_maxDspFrames{0};
    bool m_prepared{false};
    // Both are read on the audio thread and written from the owner's thread
    // (setRnnoise at the RN2 ownership seam; setRnnoiseEnabled from the
    // capture callback today, but latencyFrames() is a public const accessor
    // any thread may call). Atomics make that well-defined for the cost of
    // one relaxed/acquire load per block.
    std::atomic<RNNoiseFilter*> m_rnnoise{nullptr};
    std::atomic<bool> m_rnnoiseEnabled{false};
    bool m_captureMeasurements{false};
    bool m_warnedEgressFrameMismatch{false};
    bool m_egressRecoveryPending{false};
    float m_micGain{1.0f};
    uint64_t m_packedStages{0};
    uint64_t m_ditherState{kDitherSeed};
    Processors m_processors;

    std::unique_ptr<Resampler> m_inputResampler;
    std::unique_ptr<Resampler> m_outputLeftResampler;
    std::unique_ptr<Resampler> m_outputRightResampler;
    std::vector<float> m_inputMono;
    std::vector<float> m_outputLeft;
    std::vector<float> m_outputRight;
    QByteArray m_resampledMono48;
    QByteArray m_resampledLeft24;
    QByteArray m_resampledRight24;
    QByteArray m_rnnoiseOutput48;
    QByteArray m_work48;
    QByteArray m_normalized48;
    QByteArray m_postStrip48;
    QByteArray m_transportFloat;
};

} // namespace AetherSDR
