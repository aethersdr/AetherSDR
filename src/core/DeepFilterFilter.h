#pragma once

#ifdef HAVE_DFNR

#include "MonoDspStereoAdapter.h"

#include <QByteArray>
#include <atomic>
#include <memory>
#include <vector>

struct DFState;

namespace AetherSDR {

class Resampler;

// Client-side neural noise reduction using DeepFilterNet3.
// The immutable input/output domain is 24 or 48 kHz stereo float32. Legacy24
// retains the existing SRC pair; native48 reaches the model without SRC.
// The existing mono analysis and delayed stereo level-balance policy remain.
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

    // Returns true if df_create() succeeded.
    bool isValid() const { return m_state != nullptr; }

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
    const int m_sampleRate;
    DFState* m_state{nullptr};
    int m_frameSize{0};                     // samples per frame (from df_get_frame_length)
    std::unique_ptr<Resampler> m_up;        // 24kHz mono → 48kHz mono
    std::unique_ptr<Resampler> m_down;      // 48kHz mono → 24kHz mono
    QByteArray m_inAccum;                   // accumulate 48kHz mono float input
    QByteArray m_outAccum;                  // accumulate configured-rate stereo float output
    std::vector<float> m_monoInput;
    std::vector<float> m_processed48k;
    MonoDspStereoAdapter m_stereoAdapter;
    std::atomic<float> m_attenLimit{100.0f};
    std::atomic<float> m_postFilterBeta{0.0f};
    std::atomic<bool>  m_paramsDirty{false};
};

} // namespace AetherSDR

#endif // HAVE_DFNR
