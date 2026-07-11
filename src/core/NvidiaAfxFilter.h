#pragma once

#ifdef HAVE_NVIDIA_AFX

#include "MonoDspStereoAdapter.h"

#include <QByteArray>
#include <QString>
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
// Mirrors DeepFilterFilter's contract: processes 24 kHz stereo float32 audio
// (the RX DSP path's canonical format) by upsampling to 48 kHz mono, running
// the AFX denoiser, and downsampling back to 24 kHz stereo. Returns the same
// number of bytes as the input.
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
    explicit NvidiaAfxFilter(const QString& packDir = QString());
    ~NvidiaAfxFilter();

    NvidiaAfxFilter(const NvidiaAfxFilter&) = delete;
    NvidiaAfxFilter& operator=(const NvidiaAfxFilter&) = delete;

    // Process a block of 24 kHz stereo float32 PCM. Returns the processed block
    // (same format, same byte count). No-op passthrough until the engine is ready.
    QByteArray process(const QByteArray& pcm24kStereo);

    // Flush all carried state — the jitter accumulators and the resamplers — so
    // the next process() starts clean. Call on any audio discontinuity (stream
    // restart, RX source switch, TX→RX) to avoid replaying stale/pre-gap audio.
    void reset();

    // True once the effect is created, model loaded, and the engine is ready.
    bool isValid() const { return m_ready; }

    // Human-readable reason the filter failed to init (for status UI / logs).
    QString lastError() const { return m_lastError; }

    // Effect strength, 0.0 (passthrough) .. 1.0 (max). Mapped to AFX intensity.
    void setIntensity(float ratio);
    float intensity() const { return m_intensity.load(); }

private:
    bool loadRuntime(const QString& packDir);   // dlopen the pack libs + dlsym API
    bool createDenoiser(const QString& packDir); // CreateEffect..Load
    void teardown();

    struct Api;                                 // dlsym'd NvAFX_* function pointers
    std::unique_ptr<Api> m_api;
    void* m_handle{nullptr};                    // NvAFX_Handle (opaque)
    std::vector<void*> m_dlHandles;             // dlopen handles, closed in dtor

    int m_afxFrame{0};                          // AFX samples/frame @ 48 kHz (e.g. 480)
    bool m_ready{false};
    QString m_lastError;

    std::unique_ptr<Resampler> m_up;            // 24 kHz stereo → 48 kHz mono
    std::unique_ptr<Resampler> m_down;          // 48 kHz mono → 24 kHz stereo
    QByteArray m_inAccum;                        // 48 kHz mono float input
    QByteArray m_outAccum;                       // 24 kHz stereo float output
    int        m_outReadPos{0};                 // read cursor into m_outAccum
    MonoDspStereoAdapter m_stereoAdapter;
    std::vector<float> m_mono24k;
    std::vector<float> m_runScratch;            // reused NvAFX_Run output buffer

    std::atomic<float> m_intensity{1.0f};
    std::atomic<bool>  m_paramsDirty{false};
};

} // namespace AetherSDR

#endif // HAVE_NVIDIA_AFX
