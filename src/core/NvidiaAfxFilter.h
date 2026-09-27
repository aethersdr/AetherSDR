#pragma once

#ifdef HAVE_NVIDIA_AFX

#include <QByteArray>
#include <QString>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace AetherSDR {

class Resampler;

// Optional GPU noise removal using the NVIDIA Maxine Audio Effects SDK
// (the "denoiser"/BNR effect). The AFX runtime (libnv_audiofx + the bundled
// CUDA/TensorRT libs) is NOT linked at build time — it is dlopen'd at runtime
// from a downloaded/cached "pack" so the shipped app carries none of the
// ~2 GB GPU runtime. Requires an NVIDIA RTX/GeForce GPU (Turing+).
//
// The immutable input/output domain is 24 or 48 kHz stereo float32. Legacy24
// retains its SRC pairs; native48 reaches the model without SRC. Each channel
// runs its own denoiser effect, as RN2 runs one RNNoise state per channel, so
// the two sides of a diversity pair are denoised against their own noise.
//
// Thread-safe parameter setters (GUI thread writes atomics, audio thread reads).
// dlopen + TensorRT engine build happen in the constructor (call OFF the audio
// thread); process() is the only audio-thread entry point.

class NvidiaAfxFilter {
public:
    // packDir is the root of the AFX pack. Linux layout: nvafx/lib,
    // external/cuda/lib, features/denoiser/{lib,models/sm_XX}. Windows layout:
    // bin/ (NVAudioEffects.dll + sibling CUDA/TensorRT/feature DLLs) and the
    // same features/denoiser/models/sm_XX model tree. If empty, the pack is
    // resolved from $AETHER_NVAFX_DIR then the app data cache dir.
    explicit NvidiaAfxFilter(const QString& packDir = QString(), int sampleRate = 24000);
    int sampleRate() const { return m_sampleRate; }
    ~NvidiaAfxFilter();

    NvidiaAfxFilter(const NvidiaAfxFilter&) = delete;
    NvidiaAfxFilter& operator=(const NvidiaAfxFilter&) = delete;

    // Process configured-rate stereo float32 PCM. Returns the processed block
    // (same format, same byte count). No-op passthrough until the engine is ready.
    QByteArray process(const QByteArray& pcmStereo);

    // Flush wrapper jitter accumulators and resamplers.
    // This does not reset SDK recurrent state. Recreate the complete filter
    // for a new source or discontinuity that requires a clean algorithm epoch.
    void reset();

    // True once the effect is created, model loaded, and the engine is ready.
    bool isValid() const { return m_ready; }

    // Human-readable reason the filter failed to init (for status UI / logs).
    QString lastError() const { return m_lastError; }

    // Effect strength, 0.0 (passthrough) .. 1.0 (max). Mapped to AFX intensity.
    void setIntensity(float ratio);
    float intensity() const { return m_intensity.load(); }

private:
    const int m_sampleRate;
    bool loadRuntime(const QString& packDir);   // dlopen the pack libs + dlsym API
    bool createDenoiser(const QString& packDir, void** handle); // CreateEffect..Load
    void createResamplers();
    void teardown();

    struct Api;                                 // dlsym'd NvAFX_* function pointers
    std::unique_ptr<Api> m_api;
    std::array<void*, 2> m_handles{};           // NvAFX_Handle per channel (opaque)
    std::vector<void*> m_dlHandles;             // dlopen handles, closed in dtor

    int m_afxFrame{0};                          // AFX samples/frame @ 48 kHz (e.g. 480)
    bool m_ready{false};
    QString m_lastError;

    // Per channel, indexed 0 = left, 1 = right.
    std::array<std::unique_ptr<Resampler>, 2> m_up;    // 24 kHz → 48 kHz
    std::array<std::unique_ptr<Resampler>, 2> m_down;  // 48 kHz → 24 kHz
    std::array<QByteArray, 2> m_inAccum;               // 48 kHz float input
    std::array<std::vector<float>, 2> m_channelInput;
    std::array<std::vector<float>, 2> m_runScratch;    // reused NvAFX_Run output buffer
    std::array<QByteArray, 2> m_channelOutput;         // configured-rate float output
    QByteArray m_outAccum;                       // configured-rate stereo float output
    int        m_outReadPos{0};                 // read cursor into m_outAccum

    std::atomic<float> m_intensity{1.0f};
    std::atomic<bool>  m_paramsDirty{false};
};

} // namespace AetherSDR

#endif // HAVE_NVIDIA_AFX
