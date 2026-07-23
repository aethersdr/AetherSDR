#include "asr/SileroVad.h"

#include <algorithm>

#ifdef HAVE_ONNX
#include "asr/OrtPath.h" // ORTCHAR_T (wchar_t on Windows) model-path widening
#endif

namespace AetherSDR {

namespace {
constexpr int kStateSize = 2 * 1 * 128; // Silero v5 unified LSTM state
constexpr int64_t kSampleRate = 16000;
} // namespace

SileroVad::SileroVad()
    : m_state(kStateSize, 0.0f)
{
    m_window.reserve(kWindowSamples);
#ifdef HAVE_ONNX
    m_opts.SetIntraOpNumThreads(1);
    m_opts.SetGraphOptimizationLevel(ORT_ENABLE_BASIC);
    m_mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
#endif
}

SileroVad::~SileroVad() = default;

bool SileroVad::load(const std::string& modelPath)
{
    m_loaded = false;
#ifdef HAVE_ONNX
    try {
        m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "AetherSDR-VAD");
        m_session = std::make_unique<Ort::Session>(
            *m_env, asr_detail::toOrtPath(modelPath).c_str(), m_opts);
        reset();
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

void SileroVad::reset()
{
    m_window.clear();
    std::fill(m_state.begin(), m_state.end(), 0.0f);
    m_lastProb = 0.0f;
}

bool SileroVad::isSpeech(const float* frame, int frameSamples)
{
    if (!m_loaded || frame == nullptr || frameSamples <= 0) {
        return false;
    }
#ifdef HAVE_ONNX
    m_window.insert(m_window.end(), frame, frame + frameSamples);
    // Drain complete windows; the last one sets the current decision.
    while (static_cast<int>(m_window.size()) >= kWindowSamples) {
        runWindow(m_window.data());
        m_window.erase(m_window.begin(), m_window.begin() + kWindowSamples);
    }
#else
    (void)frame;
    (void)frameSamples;
#endif
    return m_lastProb >= m_threshold;
}

#ifdef HAVE_ONNX
void SileroVad::runWindow(const float* window512)
{
    try {
        const int64_t inShape[2] = {1, kWindowSamples};
        Ort::Value input = Ort::Value::CreateTensor<float>(
            m_mem, const_cast<float*>(window512), kWindowSamples, inShape, 2);

        const int64_t stateShape[3] = {2, 1, 128};
        Ort::Value state = Ort::Value::CreateTensor<float>(
            m_mem, m_state.data(), m_state.size(), stateShape, 3);

        int64_t sr = kSampleRate;
        Ort::Value srTensor = Ort::Value::CreateTensor<int64_t>(m_mem, &sr, 1, nullptr, 0);

        const char* inputNames[] = {"input", "state", "sr"};
        Ort::Value inputs[] = {std::move(input), std::move(state), std::move(srTensor)};
        const char* outputNames[] = {"output", "stateN"};

        auto outputs = m_session->Run(Ort::RunOptions{nullptr}, inputNames, inputs, 3,
                                      outputNames, 2);

        m_lastProb = outputs[0].GetTensorData<float>()[0];
        const float* newState = outputs[1].GetTensorData<float>();
        std::copy(newState, newState + m_state.size(), m_state.begin());
    } catch (const Ort::Exception&) {
        // Keep the previous probability on a transient inference error.
    }
}
#endif

} // namespace AetherSDR
