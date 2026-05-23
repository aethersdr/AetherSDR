#pragma once

#ifdef HAVE_SPECBLEACH

#include <QByteArray>
#include <atomic>
#include <vector>

typedef void* SpectralBleachHandle;

namespace MasterSDR {

// SpecbleachFilter — wrapper around libspecbleach for NR4 noise reduction.
// Processes 24 kHz stereo int16 audio (same interface as RNNoiseFilter).
// Thread-safe parameter setters (main thread writes, audio thread reads).
class SpecbleachFilter {
public:
    SpecbleachFilter();
    ~SpecbleachFilter();

    SpecbleachFilter(const SpecbleachFilter&) = delete;
    SpecbleachFilter& operator=(const SpecbleachFilter&) = delete;

    // Process stereo int16 PCM at 24 kHz. Returns processed audio.
    QByteArray process(const QByteArray& pcm24kStereo);

    bool isValid() const { return m_handle != nullptr; }
    void reset();

    // User-adjustable parameters (thread-safe)
    void setReductionAmount(float dB);   // 0–40 dB
    void setSmoothingFactor(float pct);  // 0–100 %
    void setWhiteningFactor(float pct);  // 0–100 %
    void setAdaptiveNoise(bool on);
    void setNoiseEstimationMethod(int method); // 0=SPP-MMSE, 1=Brandt, 2=Martin
    void setMaskingDepth(float v);       // 0.0–1.0
    void setSuppressionStrength(float v); // 0.0–1.0

    float reductionAmount() const  { return m_reduction.load(); }
    float smoothingFactor() const  { return m_smoothing.load(); }
    float whiteningFactor() const  { return m_whitening.load(); }

private:
    void applyParams();

    SpectralBleachHandle m_handle{nullptr};

    // Param atomics
    std::atomic<float> m_reduction{10.0f};
    std::atomic<float> m_smoothing{0.0f};
    std::atomic<float> m_whitening{0.0f};
    std::atomic<bool>  m_adaptive{true};
    std::atomic<int>   m_noiseMethod{0};
    std::atomic<float> m_maskingDepth{0.5f};
    std::atomic<float> m_suppression{0.5f};
    std::atomic<bool>  m_paramsDirty{true};

    // Frame counter for noise profile learning period — pass audio through
    // unprocessed during the first ~1 second to avoid silence (#827)
    int m_frameCount{0};
    static constexpr int kLearningFrames = 25;  // ~1 sec at 40ms/frame

    // Buffers
    std::vector<float> m_monoIn;
    std::vector<float> m_monoOut;
};

} // namespace MasterSDR

#endif // HAVE_SPECBLEACH
