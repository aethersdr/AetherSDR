#pragma once

#include <QVector>
#include <QString>
#include <memory>

#ifdef HAVE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace AetherSDR {

// Result from the spectrogram CNN classifier.
struct ClassifierResult {
    float voiceProb{0.5f};    // probability [0,1] that the signal is voice/SSB
    float carrierProb{0.5f};  // probability [0,1] that it is a narrow carrier/digital
    bool  valid{false};       // false when ONNX is absent or model not loaded
};

// Thin wrapper around an ONNX Runtime inference session.
// Compiled out entirely when HAVE_ONNX is not defined.
// Input:  flat float array of shape [1, 1, timeFrames, freqBins] (NCHW)
// Output: 2-class softmax [voiceProb, carrierProb]
class SignalClassifier {
public:
    SignalClassifier();
    ~SignalClassifier();

    // Load model from disk.  Returns false (and logs) on error.
    bool loadModel(const QString& path);
    bool isLoaded() const { return m_loaded; }

    // Run inference.  patch must have timeFrames × freqBins elements.
    ClassifierResult classify(const QVector<float>& patch,
                              int timeFrames, int freqBins) const;

private:
    bool m_loaded{false};
#ifdef HAVE_ONNX
    Ort::Env         m_env;
    Ort::SessionOptions m_sessionOpts;
    std::unique_ptr<Ort::Session> m_session;
    Ort::MemoryInfo  m_memInfo;
#endif
};

} // namespace AetherSDR
