#include "asr/SpeakerEmbedder.h"

#include <cmath>

#ifdef HAVE_ONNX
#include "asr/OrtPath.h" // ORTCHAR_T (wchar_t on Windows) model-path widening
#endif

namespace AetherSDR {

SpeakerEmbedder::SpeakerEmbedder() = default;
SpeakerEmbedder::~SpeakerEmbedder() = default;

bool SpeakerEmbedder::load(const std::string& modelPath)
{
    m_loaded = false;
#ifdef HAVE_ONNX
    try {
        m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "AetherSDR-SpkEmb");
        m_opts.SetIntraOpNumThreads(1);
        m_opts.SetGraphOptimizationLevel(ORT_ENABLE_BASIC);
        m_mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        m_session = std::make_unique<Ort::Session>(
            *m_env, asr_detail::toOrtPath(modelPath).c_str(), m_opts);

        Ort::AllocatorWithDefaultOptions alloc;
        m_inputName = m_session->GetInputNameAllocated(0, alloc).get();
        m_outputName = m_session->GetOutputNameAllocated(0, alloc).get();
        m_loaded = true;
    } catch (const Ort::Exception&) {
        m_session.reset();
        m_env.reset();
        m_loaded = false;
    }
#else
    (void)modelPath;
#endif
    return m_loaded;
}

std::vector<float> SpeakerEmbedder::embed(const float* samples, int count) const
{
#ifdef HAVE_ONNX
    if (!m_loaded) {
        return {};
    }
    int frames = 0;
    std::vector<float> feats = m_fbank.compute(samples, count, &frames);
    if (frames <= 0) {
        return {};
    }
    try {
        const int64_t shape[3] = {1, frames, Fbank::kNumBins};
        Ort::Value input = Ort::Value::CreateTensor<float>(
            m_mem, feats.data(), feats.size(), shape, 3);

        const char* inNames[] = {m_inputName.c_str()};
        const char* outNames[] = {m_outputName.c_str()};
        auto outputs = m_session->Run(Ort::RunOptions{nullptr}, inNames, &input, 1, outNames, 1);

        const auto info = outputs[0].GetTensorTypeAndShapeInfo();
        const int dim = static_cast<int>(info.GetElementCount());
        const float* data = outputs[0].GetTensorData<float>();

        std::vector<float> emb(data, data + dim);
        // L2-normalize so cosine similarity == dot product.
        double norm = 0.0;
        for (float v : emb) {
            norm += static_cast<double>(v) * v;
        }
        norm = std::sqrt(norm);
        if (norm > 0.0) {
            for (float& v : emb) {
                v = static_cast<float>(v / norm);
            }
        }
        const_cast<SpeakerEmbedder*>(this)->m_dim = dim;
        return emb;
    } catch (const Ort::Exception&) {
        return {};
    }
#else
    (void)samples;
    (void)count;
    return {};
#endif
}

} // namespace AetherSDR
