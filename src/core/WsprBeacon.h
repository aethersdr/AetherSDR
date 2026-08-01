#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <QString>

namespace AetherSDR {

// Standard WSPR type-1 message encoder and sample-accurate continuous-phase
// 4-FSK generator. The UI prepares the 162 channel symbols; the audio thread
// consumes them without locks or allocation.
class WsprBeacon {
public:
    static constexpr int kSymbolCount = 162;
    static constexpr int kSampleRate = 24000;
    static constexpr int kFramesPerSymbol = 16384;
    static constexpr double kToneSpacingHz = 12000.0 / 8192.0;
    static constexpr int kPreRollFrames = kSampleRate;

    // Amplitude taper at both ends of the frame.
    //
    // WSJT-X fades its tail rather than cutting it: 0.017 symbols before the
    // end it starts multiplying the amplitude by 0.98 per sample at 48 kHz
    // (Modulator.cpp `if (m_ic > i0) m_amp = 0.98 * m_amp`, with
    // `i0 = (m_symbolsLength - 0.017) * 4.0 * m_nsps`), reaching about -98 dB
    // by the final sample. Stopping a full-scale tone dead at an arbitrary
    // phase is a step discontinuity, and the resulting click is broadband — in
    // a 200 Hz-wide sub-band shared by every WSPR station on the air, that
    // lands on everyone's slot, not just ours.
    //
    // Same 11.6 ms duration at our 24 kHz rate, which is half as many samples,
    // so the same total decay needs the per-sample factor squared:
    // 0.98^2 = 0.9604 over 278 frames ≈ -97.5 dB.
    static constexpr int kTaperFrames = kFramesPerSymbol * 17 / 1000;
    static constexpr float kTaperDecayPerFrame = 0.9604f;
    static constexpr int64_t kMessageFrames =
        static_cast<int64_t>(kSymbolCount) * kFramesPerSymbol;
    static constexpr int64_t kTotalFrames = kPreRollFrames + kMessageFrames;
    static constexpr int kTransmitDurationMs =
        1000 + static_cast<int>(
            kMessageFrames * 1000 / kSampleRate);
    static constexpr int kMinimumInterlockTimeoutMs = 120000;

    static constexpr int64_t framesForElapsedNanoseconds(int64_t nanoseconds)
    {
        return nanoseconds * kSampleRate / 1000000000LL;
    }

    static constexpr bool isInterlockTimeoutSufficient(int timeoutMs)
    {
        return timeoutMs == 0 || timeoutMs >= kMinimumInterlockTimeoutMs;
    }

    using Symbols = std::array<uint8_t, kSymbolCount>;

    struct EncodeResult {
        Symbols symbols{};
        QString error;

        explicit operator bool() const { return error.isEmpty(); }
    };

    static EncodeResult encode(const QString& callsign,
                               const QString& locator,
                               int powerDbm);
    static bool isStandardPower(int powerDbm);

    void prepare(double sampleRate);
    // `toneZeroHz` is the frequency of channel symbol 0 — the LOWEST of the
    // four tones — matching WSJT-X, where the Tx-frequency spin box sets
    // `m_frequency` and the modulator emits `m_frequency + itone[isym] *
    // m_toneSpacing` with itone in 0..3 (Modulator.cpp). An earlier revision
    // centred the constellation on this value instead, which put every tone
    // 1.5 spacings — 2.197 Hz — below where WSJT-X would have put it and made
    // every resulting spot report read 2.2 Hz low.
    //
    // `messageSkipFrames` starts the frame that far in, for a slot boundary
    // that was already missed. WSJT-X does the same: when it is late it sets
    // `m_ic = (mstr - delay_ms) * m_frameRate / 1000` and truncates the HEAD
    // of the waveform, rather than sliding the whole 111.6 s frame later and
    // reporting the lateness as DT (Modulator.cpp).
    void start(const Symbols& symbols, double toneZeroHz, float levelDb,
               int preRollFrames = kPreRollFrames, int messageSkipFrames = 0);
    void stop() noexcept;

    bool isActive() const noexcept;
    bool isComplete() const noexcept;
    int currentSymbol() const noexcept;

    // Audio thread. While active, always replaces the supplied buffer. After
    // the message completes it holds silence until stop(), preventing mic
    // leakage during the queued unkey edge.
    //
    // Float is the only output form. An int16 overload existed alongside this
    // one and had no caller left once the DAX pump stopped quantizing; keeping
    // it would have meant the beacon's tests covering pre-roll, completion and
    // tail silence all ran against a path that never ships.
    void process(float* interleaved, int frames, int channels) noexcept;

private:
    void beginPendingTransmission() noexcept;
    // Advances the generator by exactly one frame and returns that frame's
    // sample: silence through the pre-roll and after the final symbol, the
    // tapered 4-FSK tone in between.
    float nextFrameSample() noexcept;
    bool beginProcess() noexcept;

    std::array<std::atomic<uint8_t>, kSymbolCount> m_pendingSymbols{};
    std::atomic<double> m_pendingToneZeroHz{1500.0};
    std::atomic<float> m_pendingLevelDb{-20.0f};
    std::atomic<int> m_pendingPreRollFrames{kSampleRate};
    std::atomic<int> m_pendingMessageSkipFrames{0};
    std::atomic<uint64_t> m_version{0};
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_complete{false};
    std::atomic<int> m_currentSymbol{-1};

    Symbols m_symbols{};
    uint64_t m_cachedVersion{0};
    double m_sampleRate{kSampleRate};
    double m_toneZeroHz{1500.0};
    float m_amplitude{0.1f};
    // Double, as in WSJT-X (`double m_phi, m_dphi`). A float accumulator holds
    // ~4.8e-7 rad of resolution near 2*pi, which it re-quantizes on every one
    // of the 2.65 million samples in a frame; there is no reason to carry that
    // when the alternative costs nothing.
    double m_phase{0.0};
    double m_phaseIncrement{0.0};
    float m_envelope{0.0f};
    int m_preRollRemaining{0};
    int m_symbolIndex{0};
    int m_framesIntoSymbol{0};
    // Tone frames actually emitted, which is NOT the position within the
    // message once a late start has skipped into it. The ramp-up keys off this
    // one so that a skipped start is still ramped; the tail keys off position.
    int64_t m_framesEmitted{0};
};

} // namespace AetherSDR
