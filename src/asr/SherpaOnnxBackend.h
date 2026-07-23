#pragma once

#include "asr/IAsrBackend.h"

#include <QString>

#include <functional>
#include <memory>

namespace AetherSDR {

// Non-whisper ASR backend (RFC #4333 follow-up): runs a sherpa-onnx offline model
// through its C API — encoder/decoder/joiner, Moonshine, Paraformer, CTC, etc.
// The C API does feature extraction + decoding internally, so we just hand it
// raw 16 kHz mono samples and read back text. Uses the ONNX Runtime that
// sherpa-onnx bundles.
//
// `load()` takes a MODEL DIRECTORY (sherpa models ship as multi-file bundles) and
// auto-detects the model type from the files present. Compiles to an inert stub
// (load() fails) when HAVE_SHERPA is undefined.
class SherpaOnnxBackend : public IAsrBackend {
public:
    explicit SherpaOnnxBackend(int numThreads = 2);
    ~SherpaOnnxBackend() override;

    // modelPath is a directory containing a sherpa-onnx model bundle.
    bool load(const QString& modelPath, QString* error) override;
    bool isLoaded() const override;
    AsrTranscript transcribe(const std::vector<float>& pcm16k, QString* error) override;
    void unload() override;

private:
    struct Impl;                 // hides the sherpa-onnx C API from the header
    std::unique_ptr<Impl> m_impl;
    int m_numThreads;
};

// True when sherpa-onnx support was compiled in (HAVE_SHERPA). The UI hides the
// "sherpa-onnx model…" option when this is false.
bool sherpaOnnxAvailable();

// Factory (mirrors whisperAsrBackendFactory): the model directory is supplied
// later via AsrEngine::setModelPath → IAsrBackend::load.
std::function<std::unique_ptr<IAsrBackend>()> sherpaOnnxBackendFactory(int numThreads = 2);

} // namespace AetherSDR
