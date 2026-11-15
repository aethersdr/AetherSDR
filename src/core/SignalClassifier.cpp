#include "SignalClassifier.h"
#include "LogManager.h"

#include <QFile>

namespace AetherSDR {

SignalClassifier::SignalClassifier()
#ifdef HAVE_ONNX
    : m_env(ORT_LOGGING_LEVEL_WARNING, "AetherSDR")
    , m_memInfo(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
#endif
{
#ifdef HAVE_ONNX
    m_sessionOpts.SetIntraOpNumThreads(1);
    m_sessionOpts.SetGraphOptimizationLevel(ORT_ENABLE_BASIC);
#endif
}

SignalClassifier::~SignalClassifier() = default;

bool SignalClassifier::loadModel(const QString& path)
{
#ifdef HAVE_ONNX
    if (!QFile::exists(path)) {
        qCWarning(lcSHistory, "SignalClassifier: model file not found: %s",
                  qPrintable(path));
        return false;
    }
    try {
#ifdef Q_OS_WIN
        const std::wstring wpath = path.toStdWString();
        m_session = std::make_unique<Ort::Session>(m_env, wpath.c_str(), m_sessionOpts);
#else
        m_session = std::make_unique<Ort::Session>(m_env, path.toUtf8().constData(),
                                                   m_sessionOpts);
#endif
        m_loaded = true;
        qCInfo(lcSHistory, "SignalClassifier: model loaded from %s", qPrintable(path));
        return true;
    } catch (const Ort::Exception& ex) {
        qCWarning(lcSHistory, "SignalClassifier: failed to load model: %s", ex.what());
        return false;
    }
#else
    Q_UNUSED(path)
    return false;
#endif
}

ClassifierResult SignalClassifier::classify(const QVector<float>& patch,
                                             int timeFrames, int freqBins) const
{
    ClassifierResult res;
#ifdef HAVE_ONNX
    if (!m_loaded || !m_session) { return res; }
    if (patch.size() != timeFrames * freqBins) { return res; }

    try {
        // Shape: [N=1, C=1, H=timeFrames, W=freqBins]
        const std::array<int64_t, 4> shape{1, 1, timeFrames, freqBins};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            m_memInfo,
            const_cast<float*>(patch.constData()),
            static_cast<size_t>(patch.size()),
            shape.data(), shape.size());

        const char* inputNames[]  = {"input"};
        const char* outputNames[] = {"output"};

        auto outputs = m_session->Run(Ort::RunOptions{nullptr},
                                      inputNames, &inputTensor, 1,
                                      outputNames, 1);

        const float* data = outputs[0].GetTensorData<float>();
        res.voiceProb   = data[0];
        res.carrierProb = data[1];
        res.valid       = true;
    } catch (const Ort::Exception& ex) {
        qCWarning(lcSHistory, "SignalClassifier: inference error: %s", ex.what());
    }
#else
    Q_UNUSED(patch); Q_UNUSED(timeFrames); Q_UNUSED(freqBins);
#endif
    return res;
}

} // namespace AetherSDR
