#pragma once

#include "asr/Fbank.h"

#include <memory>
#include <string>
#include <vector>

#ifdef HAVE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace AetherSDR {

// Speaker-embedding extractor (RFC #4333 follow-up): turns one utterance of
// 16 kHz mono audio into an L2-normalized speaker vector, using a WeSpeaker /
// 3D-Speaker ECAPA-TDNN ONNX model run in the ONNX Runtime AetherSDR already
// ships. Feeds the model kaldi-style Fbank features (input "feats") and returns
// the "embs" output normalized so cosine similarity is a dot product.
//
// Inert stub (embed() returns empty) when HAVE_ONNX is undefined.
class SpeakerEmbedder {
public:
    SpeakerEmbedder();
    ~SpeakerEmbedder();

    bool load(const std::string& modelPath);
    bool isLoaded() const { return m_loaded; }
    int dim() const { return m_dim; }

    // L2-normalized embedding for one utterance; empty on failure / too-short
    // input / no ONNX.
    std::vector<float> embed(const float* samples, int count) const;

private:
    bool m_loaded = false;
    int m_dim = 0;
    Fbank m_fbank;

#ifdef HAVE_ONNX
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::Session> m_session;
    Ort::SessionOptions m_opts;
    Ort::MemoryInfo m_mem{nullptr};
    std::string m_inputName;
    std::string m_outputName;
#endif
};

} // namespace AetherSDR
