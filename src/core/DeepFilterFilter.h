#pragma once

#ifdef HAVE_DFNR

#include <QByteArray>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

struct DFState;

namespace AetherSDR {

class Resampler;

// Client-side neural noise reduction using DeepFilterNet3.
// The immutable input/output domain is 24 or 48 kHz stereo float32. Legacy24
// retains the existing SRC pairs; native48 reaches the model without SRC.
// Each channel runs its own DeepFilterNet state, as RN2 runs one RNNoise
// state per channel, so the two sides of a diversity pair are denoised
// against their own noise and a pan change is heard immediately.
//
// DeepFilterNet expects 48kHz mono float [-1.0, 1.0] input.
// Frame size determined at runtime via df_get_frame_length().
// Thread-safe parameter setters (main thread writes, audio thread reads).

class DeepFilterFilter {
public:
    // Unsupported rates leave isValid() false. Recreate for a new rate/source.
    explicit DeepFilterFilter(int sampleRate = 24000);
    ~DeepFilterFilter();

    DeepFilterFilter(const DeepFilterFilter&) = delete;
    DeepFilterFilter& operator=(const DeepFilterFilter&) = delete;

    // Process a block of 24/48 kHz stereo float32 PCM.
    // Returns the processed block (same format, same size).
    QByteArray process(const QByteArray& pcmStereo);

    // Returns true if df_create() succeeded for both channels.
    bool isValid() const { return m_states[0] != nullptr && m_states[1] != nullptr; }

    int sampleRate() const { return m_sampleRate; }

    // Reset internal state (e.g., on band change).
    void reset();

    // Attenuation limit in dB (0 = passthrough, 100 = max removal)
    void setAttenLimit(float db);
    float attenLimit() const { return m_attenLimit.load(); }

    // Post-filter beta (0 = disabled, 0.05–0.3 typical)
    void setPostFilterBeta(float beta);
    float postFilterBeta() const { return m_postFilterBeta.load(); }

private:
    void createStates();
    void freeStates();

    const int m_sampleRate;
    std::array<DFState*, 2> m_states{};
    int m_frameSize{0};                     // samples per frame (from df_get_frame_length)
    // Per channel, indexed 0 = left, 1 = right.
    std::array<std::unique_ptr<Resampler>, 2> m_up;    // 24kHz → 48kHz
    std::array<std::unique_ptr<Resampler>, 2> m_down;  // 48kHz → 24kHz
    std::array<QByteArray, 2> m_inAccum;               // 48kHz float input
    std::array<std::vector<float>, 2> m_channelInput;
    std::array<std::vector<float>, 2> m_processed48k;
    std::array<QByteArray, 2> m_channelOutput;         // configured-rate float output
    bool m_lockstepWarned{false};                      // L/R output length mismatch logged
    QByteArray m_outAccum;                  // accumulate configured-rate stereo float output
    std::atomic<float> m_attenLimit{100.0f};
    std::atomic<float> m_postFilterBeta{0.0f};
    std::atomic<bool>  m_paramsDirty{false};
};

} // namespace AetherSDR

#endif // HAVE_DFNR
