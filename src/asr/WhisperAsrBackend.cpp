#include "asr/WhisperAsrBackend.h"

#include <QFileInfo>
#include <QLoggingCategory>
#include <QThread>

#include <algorithm>

#include <ggml-backend.h>
#include <whisper.h>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcAsrWhisper, "aether.asr.whisper")

namespace {
// Whisper is chunk-based; on HF audio a single over is well under 30 s, so we
// cap inference threads modestly to leave the box responsive (esp. on a Pi).
int chooseThreadCount()
{
    const int hw = QThread::idealThreadCount();
    if (hw <= 0) {
        return 4;
    }
    return std::clamp(hw, 1, 4);
}
} // namespace

WhisperAsrBackend::WhisperAsrBackend()
    : WhisperAsrBackend(QStringLiteral("en"))
{
}

WhisperAsrBackend::WhisperAsrBackend(QString language, int gpuDevice)
    : m_language(std::move(language))
    , m_threads(chooseThreadCount())
    , m_gpuDevice(gpuDevice)
{
}

WhisperAsrBackend::~WhisperAsrBackend()
{
    unload();
}

bool WhisperAsrBackend::load(const QString& modelPath, QString* error)
{
    unload();

    if (!QFileInfo::exists(modelPath)) {
        if (error != nullptr) {
            *error = QStringLiteral("model file not found: %1").arg(modelPath);
        }
        return false;
    }

    whisper_context_params cparams = whisper_context_default_params();
    // gpu_device selects the GPU (index among GPU devices; see asrGpuDevices), or
    // -1 to force CPU. Otherwise use a GPU backend (Vulkan/Metal) when one is
    // compiled in and present; ggml falls back to CPU automatically when not.
    const bool useGpu = m_gpuDevice >= 0 && asrGpuAvailable();
    cparams.use_gpu = useGpu;
    cparams.gpu_device = useGpu ? m_gpuDevice : 0;

    const QByteArray pathUtf8 = modelPath.toUtf8();
    m_ctx = whisper_init_from_file_with_params(pathUtf8.constData(), cparams);
    if (m_ctx == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("whisper failed to load model: %1").arg(modelPath);
        }
        return false;
    }

    qCInfo(lcAsrWhisper) << "Loaded model" << modelPath << "(" << m_threads << "threads )";
    return true;
}

AsrTranscript WhisperAsrBackend::transcribe(const std::vector<float>& pcm16k, QString* error)
{
    if (m_ctx == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("no model loaded");
        }
        return {};
    }
    if (pcm16k.empty()) {
        return {};
    }

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.n_threads = m_threads;
    wparams.translate = false;
    wparams.no_context = true;    // each over is independent
    wparams.single_segment = true; // one utterance -> one segment
    wparams.no_timestamps = true;
    wparams.print_progress = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.print_special = false;
    wparams.suppress_blank = true;

    // "auto" (or empty) → let whisper detect the language from the audio and
    // then transcribe it; any other value pins decoding to that language.
    // whisper_full() treats a language of "auto" as detect-then-transcribe on
    // its own. detect_language MUST stay false: when it is true whisper detects
    // the language and returns early WITHOUT transcribing (whisper.cpp:
    // `if (params.detect_language) return 0;`), which would yield empty output.
    const QByteArray langUtf8 = m_language.toUtf8();
    const bool autoDetect = langUtf8.isEmpty() || langUtf8 == "auto";
    wparams.language = autoDetect ? "auto" : langUtf8.constData();
    wparams.detect_language = false;

    if (whisper_full(m_ctx, wparams, pcm16k.data(), static_cast<int>(pcm16k.size())) != 0) {
        if (error != nullptr) {
            *error = QStringLiteral("whisper_full() failed");
        }
        return {};
    }

    // Confidence = mean probability over the real (non-special) tokens. Special
    // tokens (timestamps, <eot>, etc.) have ids >= the end-of-text token and are
    // excluded so punctuation/formatting doesn't skew the score.
    const whisper_token specialFloor = whisper_token_eot(m_ctx);

    QString text;
    double probSum = 0.0;
    int probCount = 0;
    const int segments = whisper_full_n_segments(m_ctx);
    for (int i = 0; i < segments; ++i) {
        const char* seg = whisper_full_get_segment_text(m_ctx, i);
        if (seg != nullptr) {
            text += QString::fromUtf8(seg);
        }
        const int nTokens = whisper_full_n_tokens(m_ctx, i);
        for (int t = 0; t < nTokens; ++t) {
            if (whisper_full_get_token_id(m_ctx, i, t) >= specialFloor) {
                continue;
            }
            probSum += whisper_full_get_token_p(m_ctx, i, t);
            ++probCount;
        }
    }

    AsrTranscript result;
    result.text = text.trimmed();
    result.confidence = probCount > 0 ? static_cast<float>(probSum / probCount) : 0.0f;
    return result;
}

void WhisperAsrBackend::unload()
{
    if (m_ctx != nullptr) {
        whisper_free(m_ctx);
        m_ctx = nullptr;
    }
}

std::function<std::unique_ptr<IAsrBackend>()> whisperAsrBackendFactory(const QString& language,
                                                                       int gpuDevice)
{
    return [language, gpuDevice]() -> std::unique_ptr<IAsrBackend> {
        return std::make_unique<WhisperAsrBackend>(language, gpuDevice);
    };
}

std::vector<AsrGpuDevice> asrGpuDevices()
{
    // Enumerate GPU + integrated-GPU devices in the same order whisper's
    // gpu_device indexes them (see whisper_backend_init_gpu).
    std::vector<AsrGpuDevice> devices;
    int index = 0;
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        const auto type = ggml_backend_dev_type(dev);
        if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU) {
            AsrGpuDevice d;
            d.index = index++;
            d.name = QString::fromUtf8(ggml_backend_dev_description(dev));
            devices.push_back(std::move(d));
        }
    }
    return devices;
}

bool asrGpuAvailable()
{
    // True when ggml has at least one GPU (discrete or integrated) device.
    return !asrGpuDevices().empty();
}

std::vector<AsrLanguage> asrWhisperLanguages()
{
    // Enumerate every language the vendored whisper build supports, straight
    // from its own table (whisper_lang_str = ISO code, whisper_lang_str_full =
    // English name), so this never drifts out of sync with the model. The
    // multilingual checkpoints cover them all; the .en-only models ignore the
    // setting and always decode English. Sorted by display name. There is no
    // "auto-detect" entry — whisper's table has no such code, and the UI does
    // not add one (detection was unreliable on short VAD segments).
    std::vector<AsrLanguage> langs;
    const int maxId = whisper_lang_max_id();
    langs.reserve(static_cast<size_t>(maxId) + 1);
    for (int id = 0; id <= maxId; ++id) {
        const char* code = whisper_lang_str(id);
        const char* full = whisper_lang_str_full(id);
        if (code == nullptr || full == nullptr) {
            continue;
        }
        AsrLanguage lang;
        lang.code = QString::fromUtf8(code);
        // whisper's full names are lowercase ("english", "haitian creole");
        // title-case each word for display ("Haitian Creole") without pulling in
        // a locale.
        QString name = QString::fromUtf8(full);
        bool startOfWord = true;
        for (QChar& ch : name) {
            if (ch.isSpace()) {
                startOfWord = true;
            } else if (startOfWord && ch.isLetter()) {
                ch = ch.toUpper();
                startOfWord = false;
            }
        }
        lang.name = name;
        langs.push_back(std::move(lang));
    }
    std::sort(langs.begin(), langs.end(),
              [](const AsrLanguage& a, const AsrLanguage& b) { return a.name < b.name; });
    return langs;
}

} // namespace AetherSDR
