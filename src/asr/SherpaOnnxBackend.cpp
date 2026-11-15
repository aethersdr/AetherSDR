#include "asr/SherpaOnnxBackend.h"

#include <QDir>
#include <QFileInfo>

#include <cstring>

#ifdef HAVE_SHERPA
#include "sherpa-onnx/c-api/c-api.h"
#endif

namespace AetherSDR {

namespace {
#ifdef HAVE_SHERPA
// Pick the first file in `dir` matching any of `patterns`, preferring an int8
// variant when present (smaller + faster). Returns "" if none match.
QString pick(const QDir& dir, const QStringList& patterns)
{
    const QStringList files = dir.entryList(patterns, QDir::Files, QDir::Name);
    for (const QString& f : files) {
        if (f.contains(QStringLiteral("int8"))) {
            return dir.absoluteFilePath(f);
        }
    }
    return files.isEmpty() ? QString() : dir.absoluteFilePath(files.first());
}
#endif
} // namespace

struct SherpaOnnxBackend::Impl {
#ifdef HAVE_SHERPA
    const SherpaOnnxOfflineRecognizer* recognizer = nullptr;
    // Backing storage for the config's const char* paths (must outlive the
    // recognizer create call; sherpa copies them, but keep them alive to be safe).
    std::vector<QByteArray> keep;
    const char* keepUtf8(const QString& s)
    {
        keep.push_back(s.toUtf8());
        return keep.back().constData();
    }
#endif
};

SherpaOnnxBackend::SherpaOnnxBackend(int numThreads)
    : m_impl(std::make_unique<Impl>())
    , m_numThreads(numThreads > 0 ? numThreads : 1)
{
}

SherpaOnnxBackend::~SherpaOnnxBackend()
{
    unload();
}

bool SherpaOnnxBackend::load(const QString& modelPath, QString* error)
{
    unload();
#ifdef HAVE_SHERPA
    const QDir dir(modelPath);
    if (!dir.exists()) {
        if (error) *error = QStringLiteral("sherpa-onnx model directory not found: %1").arg(modelPath);
        return false;
    }
    const QString tokens = dir.absoluteFilePath(QStringLiteral("tokens.txt"));
    if (!QFileInfo::exists(tokens)) {
        if (error) *error = QStringLiteral("sherpa-onnx model: tokens.txt missing in %1").arg(modelPath);
        return false;
    }

    SherpaOnnxOfflineRecognizerConfig config;
    std::memset(&config, 0, sizeof(config));
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;
    config.model_config.tokens = m_impl->keepUtf8(tokens);
    config.model_config.num_threads = m_numThreads;
    config.model_config.provider = "cpu";
    config.decoding_method = "greedy_search";

    // Detect the model type from the files present (covers the common English
    // sherpa-onnx offline bundles; more types are one else-if each).
    const QString joiner = pick(dir, {QStringLiteral("joiner*.onnx")});
    if (QFileInfo::exists(dir.absoluteFilePath(QStringLiteral("preprocess.onnx")))) {
        auto& m = config.model_config.moonshine;
        m.preprocessor = m_impl->keepUtf8(dir.absoluteFilePath(QStringLiteral("preprocess.onnx")));
        m.encoder = m_impl->keepUtf8(pick(dir, {QStringLiteral("encode*.onnx")}));
        m.uncached_decoder = m_impl->keepUtf8(pick(dir, {QStringLiteral("uncached_decode*.onnx")}));
        m.cached_decoder = m_impl->keepUtf8(pick(dir, {QStringLiteral("cached_decode*.onnx")}));
    } else if (!joiner.isEmpty()) {
        auto& t = config.model_config.transducer;
        t.encoder = m_impl->keepUtf8(pick(dir, {QStringLiteral("encoder*.onnx")}));
        t.decoder = m_impl->keepUtf8(pick(dir, {QStringLiteral("decoder*.onnx")}));
        t.joiner = m_impl->keepUtf8(joiner);
    } else {
        const QString single = pick(dir, {QStringLiteral("model*.onnx"), QStringLiteral("*.onnx")});
        if (single.isEmpty()) {
            if (error) *error = QStringLiteral("sherpa-onnx model: no .onnx files in %1").arg(modelPath);
            return false;
        }
        // Single-model bundle: Paraformer (the common single-file offline layout).
        config.model_config.paraformer.model = m_impl->keepUtf8(single);
    }

    m_impl->recognizer = SherpaOnnxCreateOfflineRecognizer(&config);
    if (m_impl->recognizer == nullptr) {
        if (error) *error = QStringLiteral("sherpa-onnx failed to create recognizer (unsupported model layout in %1)").arg(modelPath);
        m_impl->keep.clear();
        return false;
    }
    return true;
#else
    (void)modelPath;
    if (error) *error = QStringLiteral("sherpa-onnx support was not compiled in (HAVE_SHERPA off)");
    return false;
#endif
}

bool SherpaOnnxBackend::isLoaded() const
{
#ifdef HAVE_SHERPA
    return m_impl->recognizer != nullptr;
#else
    return false;
#endif
}

AsrTranscript SherpaOnnxBackend::transcribe(const std::vector<float>& pcm16k, QString* error)
{
    AsrTranscript out;
#ifdef HAVE_SHERPA
    if (m_impl->recognizer == nullptr) {
        if (error) *error = QStringLiteral("sherpa-onnx: no model loaded");
        return out;
    }
    const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(m_impl->recognizer);
    SherpaOnnxAcceptWaveformOffline(stream, 16000, pcm16k.data(), static_cast<int32_t>(pcm16k.size()));
    SherpaOnnxDecodeOfflineStream(m_impl->recognizer, stream);
    const SherpaOnnxOfflineRecognizerResult* r = SherpaOnnxGetOfflineStreamResult(stream);
    if (r != nullptr && r->text != nullptr) {
        out.text = QString::fromUtf8(r->text).trimmed();
    }
    // sherpa-onnx offline results carry no scalar confidence; use a neutral high
    // value so the panel's confidence colouring stays readable.
    out.confidence = out.text.isEmpty() ? 0.0f : 0.9f;
    SherpaOnnxDestroyOfflineRecognizerResult(r);
    SherpaOnnxDestroyOfflineStream(stream);
#else
    (void)pcm16k;
    if (error) *error = QStringLiteral("sherpa-onnx support was not compiled in");
#endif
    return out;
}

void SherpaOnnxBackend::unload()
{
#ifdef HAVE_SHERPA
    if (m_impl->recognizer != nullptr) {
        SherpaOnnxDestroyOfflineRecognizer(m_impl->recognizer);
        m_impl->recognizer = nullptr;
    }
    m_impl->keep.clear();
#endif
}

bool sherpaOnnxAvailable()
{
#ifdef HAVE_SHERPA
    return true;
#else
    return false;
#endif
}

std::function<std::unique_ptr<IAsrBackend>()> sherpaOnnxBackendFactory(int numThreads)
{
    return [numThreads] { return std::make_unique<SherpaOnnxBackend>(numThreads); };
}

} // namespace AetherSDR
