#pragma once

#include <QByteArray>

#include <array>
#include <memory>
#include <vector>

struct DenoiseState;

namespace AetherSDR {

class Resampler;

// Client-side RNNoise on 24 kHz stereo float. One RNNoise/resampler path per
// RX channel preserves pan, diversity and binaural phase: a gain envelope from
// an L/R downmix makes the noise floor breathe on non-proportional channels
// (#4689). TX ProcessedMono uses one downmixed path, duplicated to L/R.
// RNNoise itself needs 48 kHz mono float in 480-sample (10 ms) frames.

class RNNoiseFilter {
public:
    enum class OutputMode {
        PreserveRxStereo,
        ProcessedMono,
    };

    enum class RateDomain {
        Legacy24k,
        Native48k,
    };

    explicit RNNoiseFilter(
        OutputMode outputMode = OutputMode::PreserveRxStereo,
        RateDomain rateDomain = RateDomain::Legacy24k);
    ~RNNoiseFilter();

    // Process a block of 24kHz stereo FLOAT32 PCM (NOT int16
    // — the original comment lied; callers must pass float32 sample
    // pairs interleaved as L,R,L,R,... with each sample in [-1.0, 1.0]).
    // Returns the processed block in the same format and frame count.
    QByteArray process(const QByteArray& pcm24kStereo);

    // Process native RNNoise-rate audio without the wrapper's 24 <-> 48 kHz
    // resamplers. Input and output are interleaved 48 kHz stereo float32 with
    // an identical frame count. This is the TX voice path's fixed-rate seam;
    // the existing process() entry point remains the 24 kHz RX-compatible API.
    int process48kStereo(const QByteArray& pcm48kStereo, QByteArray& output);

    // Fraction of the original spectrum retained in each RX frame, clamped to
    // [0, 1]. 0 (the default) is full suppression — RN2's behavior since it
    // shipped. Owned by Rn2SettingsModel; AudioEngine calls this under its
    // DSP lock, so there is no separate synchronization here.
    void setDryMix(float dryMix);
    float dryMix() const { return m_dryMix; }

    // Returns true if every state required by the selected output mode exists.
    bool isValid() const;

    // Reset internal state (e.g., on band change).
    void reset();

private:
    int processingChannels() const;
    void processStereoFrames(const float* interleavedStereo, int stereoFrames);

    std::array<DenoiseState*, 2> m_states{nullptr, nullptr};
    std::array<std::unique_ptr<Resampler>, 2> m_up;    // 24kHz → 48kHz per channel
    std::array<std::unique_ptr<Resampler>, 2> m_down;  // 48kHz → 24kHz per channel
    std::array<QByteArray, 2> m_inAccum;               // 48kHz mono float input
    // Native 48 kHz or legacy 24 kHz stereo float output.
    QByteArray m_outAccum;
    std::array<std::vector<float>, 2> m_input24k;
    std::array<std::vector<float>, 2> m_processed48k;
    std::array<std::vector<float>, 2> m_processed48kFloat;
    OutputMode m_outputMode{OutputMode::PreserveRxStereo};
    RateDomain m_rateDomain{RateDomain::Legacy24k};
    float m_dryMix{0.0f};
    bool m_warnedChannelLengthMismatch{false};
};

} // namespace AetherSDR
