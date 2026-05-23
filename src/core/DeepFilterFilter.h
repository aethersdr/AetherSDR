#pragma once

#ifdef HAVE_DFNR

#include <QByteArray>
#include <atomic>
#include <memory>

struct DFState;

namespace MasterSDR {

class Resampler;

// Client-side neural noise reduction using DeepFilterNet3.
// Processes 24kHz stereo Int16 audio by upsampling to 48kHz mono,
// running the DeepFilter model, and downsampling back to 24kHz stereo.
//
// DeepFilterNet expects 48kHz mono float [-1.0, 1.0] input.
// Frame size determined at runtime via df_get_frame_length().
// Thread-safe parameter setters (main thread writes, audio thread reads).

class DeepFilterFilter {
public:
    DeepFilterFilter();
    ~DeepFilterFilter();

    DeepFilterFilter(const DeepFilterFilter&) = delete;
    DeepFilterFilter& operator=(const DeepFilterFilter&) = delete;

    // Process a block of 24kHz stereo Int16 PCM.
    // Returns the processed block (same format, same size).
    QByteArray process(const QByteArray& pcm24kStereo);

    // Returns true if df_create() succeeded.
    bool isValid() const { return m_state != nullptr; }

    // Reset internal state (e.g., on band change).
    void reset();

    // Attenuation limit in dB (0 = passthrough, 100 = max removal)
    void setAttenLimit(float db);
    float attenLimit() const { return m_attenLimit.load(); }

    // Post-filter beta (0 = disabled, 0.05–0.3 typical)
    void setPostFilterBeta(float beta);
    float postFilterBeta() const { return m_postFilterBeta.load(); }

private:
    DFState* m_state{nullptr};
    int m_frameSize{0};                     // samples per frame (from df_get_frame_length)
    std::unique_ptr<Resampler> m_up;        // 24kHz mono → 48kHz mono
    std::unique_ptr<Resampler> m_down;      // 48kHz mono → 24kHz mono
    QByteArray m_inAccum;                   // accumulate 48kHz mono float input
    QByteArray m_outAccum;                  // accumulate 24kHz stereo int16 output
    std::atomic<float> m_attenLimit{100.0f};
    std::atomic<float> m_postFilterBeta{0.0f};
    std::atomic<bool>  m_paramsDirty{false};
};

} // namespace MasterSDR

#endif // HAVE_DFNR
