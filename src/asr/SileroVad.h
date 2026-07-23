#pragma once

#include "asr/IVad.h"

#include <memory>
#include <string>
#include <vector>

#ifdef HAVE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace AetherSDR {

// Silero VAD (ONNX) implementation of IVad — a learned voice-activity detector
// that is far more robust in HF noise than an energy threshold (RFC #4333
// follow-up). Runs the ~2 MB Silero v5 model in an ONNX Runtime session, reusing
// the runtime AetherSDR already ships for the signal classifier.
//
// The model consumes fixed 512-sample (32 ms @ 16 kHz) windows and carries an
// LSTM state across them, so this class buffers the incoming stream into windows,
// runs one inference per window to get a speech probability, and reports
// isSpeech() = (latest probability >= threshold). State is dropped on reset().
//
// Compiles to an inert stub (load() returns false) when HAVE_ONNX is undefined,
// so the segmenter transparently keeps using the energy VAD.
class SileroVad : public IVad {
public:
    SileroVad();
    ~SileroVad() override;

    // Load the .onnx model. Returns false (leaving the VAD unloaded) on any error
    // or when ONNX Runtime is not compiled in.
    bool load(const std::string& modelPath);
    bool isLoaded() const { return m_loaded; }

    // Speech-probability threshold in [0, 1] (Silero's recommended default 0.5).
    void setThreshold(float t) { m_threshold = t; }
    float threshold() const { return m_threshold; }

    // Latest window probability (diagnostics/tests).
    float lastProbability() const { return m_lastProb; }

    bool isSpeech(const float* frame, int frameSamples) override;
    void reset() override;

    // Silero v5 window size at 16 kHz.
    static constexpr int kWindowSamples = 512;

private:
#ifdef HAVE_ONNX
    void runWindow(const float* window512);
#endif

    bool m_loaded = false;
    float m_threshold = 0.5f;
    float m_lastProb = 0.0f;
    std::vector<float> m_window;  // accumulates stream to kWindowSamples
    std::vector<float> m_state;   // [2,1,128] LSTM state carried across windows

#ifdef HAVE_ONNX
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::Session> m_session;
    Ort::SessionOptions m_opts;
    Ort::MemoryInfo m_mem{nullptr};
#endif
};

} // namespace AetherSDR
