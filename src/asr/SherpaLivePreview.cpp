#include "asr/LivePreview.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#ifdef HAVE_SHERPA
#include "sherpa-onnx/c-api/c-api.h"
#endif

namespace AetherSDR {

#ifdef HAVE_SHERPA

namespace {

constexpr int kRate = 16000;
// Silence fed after the last real sample when a stream is finished, so the
// encoder's look-ahead covers the audio just before the split.
constexpr int kTailPadding = kRate / 2;

QString pickModelFile(const QDir& dir, const QString& stem)
{
    const QStringList files =
        dir.entryList({stem + QStringLiteral("*.onnx")}, QDir::Files, QDir::Name);
    for (const QString& f : files) {
        if (f.contains(QStringLiteral("int8"))) {
            return dir.absoluteFilePath(f);
        }
    }
    return files.isEmpty() ? QString() : dir.absoluteFilePath(files.first());
}

// A loaded sherpa-onnx streaming model. Loading the encoder takes about a
// second, and the engine starts a new preview session on every retune, so one
// model serves every session a factory starts; each session has its own stream.
class SherpaModel {
public:
    SherpaModel(const QString& modelDir, int numThreads)
    {
        const QDir dir(modelDir);
        const QString tokens = dir.absoluteFilePath(QStringLiteral("tokens.txt"));
        const QString encoder = pickModelFile(dir, QStringLiteral("encoder"));
        const QString decoder = pickModelFile(dir, QStringLiteral("decoder"));
        const QString joiner = pickModelFile(dir, QStringLiteral("joiner"));
        if (!QFileInfo::exists(tokens) || encoder.isEmpty() || decoder.isEmpty()
            || joiner.isEmpty()) {
            m_error = QCoreApplication::translate(
                          "AetherSDR::LivePreview",
                          "The live preview model in %1 is incomplete.")
                          .arg(QDir::toNativeSeparators(modelDir));
            return;
        }
        const QByteArray tokens8 = tokens.toUtf8();
        const QByteArray encoder8 = encoder.toUtf8();
        const QByteArray decoder8 = decoder.toUtf8();
        const QByteArray joiner8 = joiner.toUtf8();

        SherpaOnnxOnlineRecognizerConfig config;
        std::memset(&config, 0, sizeof(config));
        config.feat_config.sample_rate = kRate;
        config.feat_config.feature_dim = 80;
        config.model_config.transducer.encoder = encoder8.constData();
        config.model_config.transducer.decoder = decoder8.constData();
        config.model_config.transducer.joiner = joiner8.constData();
        config.model_config.tokens = tokens8.constData();
        config.model_config.num_threads = std::max(1, numThreads);
        config.model_config.provider = "cpu";
        config.decoding_method = "greedy_search";
        // No endpointing: the segmenter's edges and the feeder's 10 s forcing
        // decide where text is finalised (docs/asr-copy-assist.md).
        config.enable_endpoint = 0;
        m_recognizer = SherpaOnnxCreateOnlineRecognizer(&config);
        if (m_recognizer == nullptr) {
            m_error = QCoreApplication::translate(
                          "AetherSDR::LivePreview",
                          "The live preview model in %1 could not be loaded.")
                          .arg(QDir::toNativeSeparators(modelDir));
        }
    }

    ~SherpaModel()
    {
        if (m_recognizer != nullptr) {
            SherpaOnnxDestroyOnlineRecognizer(m_recognizer);
        }
    }

    SherpaModel(const SherpaModel&) = delete;
    SherpaModel& operator=(const SherpaModel&) = delete;

    const SherpaOnnxOnlineRecognizer* recognizer() const { return m_recognizer; }
    QString error() const { return m_error; }

private:
    const SherpaOnnxOnlineRecognizer* m_recognizer = nullptr;
    QString m_error;
};

// A sherpa-onnx streaming (online) transducer behind ILivePreview. Everything
// runs synchronously on the caller's thread (LivePreviewFeeder's), so results
// are delivered from inside feed() and finalizeThrough().
//
// The stream covers [m_origin, m_fedEnd) of the caller's timeline; m_audio
// keeps that audio. finalizeThrough(s) has to split the text exactly at s,
// which a sherpa stream cannot do by itself: the stream is finished (tail
// padding, input finished, decoded out), the tokens timed before s become the
// final result for [m_origin, s), and a fresh stream starts at s with the
// audio from s on fed again. The re-fed audio is only the feeder's lead over
// the worker, normally under a second.
class SherpaLivePreview final : public ILivePreview {
public:
    SherpaLivePreview(std::shared_ptr<const SherpaModel> model, LivePreviewCallbacks callbacks)
        : m_callbacks(std::move(callbacks))
        , m_model(std::move(model))
        , m_recognizer(m_model->recognizer())
    {
    }

    ~SherpaLivePreview() override
    {
        if (m_stream != nullptr) {
            SherpaOnnxDestroyOnlineStream(m_stream);
        }
    }

    void announceReady()
    {
        if (m_callbacks.onState) {
            m_callbacks.onState(LivePreviewState::Ready, QString());
        }
    }

    void feed(const float* samples, int count, qint64 startSample) override
    {
        if (count <= 0) {
            return;
        }
        if (m_stream != nullptr && startSample != m_fedEnd) {
            // The caller re-anchored (a jump in its timeline): close the text
            // where the old audio ended and start over at the new position.
            finalizeThrough(m_fedEnd);
            restart(startSample);
        } else if (m_stream == nullptr) {
            restart(startSample);
        }
        m_audio.insert(m_audio.end(), samples, samples + count);
        accept(samples, count);
        m_fedEnd = startSample + count;
        publishProvisional();
    }

    void finalizeThrough(qint64 sample) override
    {
        if (m_stream == nullptr) {
            return;
        }
        const qint64 split = std::min(sample, m_fedEnd);
        if (split <= m_origin) {
            return;
        }

        // Finish the stream so the words just before the split are decoded.
        const std::vector<float> silence(kTailPadding, 0.0f);
        accept(silence.data(), kTailPadding);
        SherpaOnnxOnlineStreamInputFinished(m_stream);
        decodeReady();
        const QString text = textBefore(split);
        if (m_callbacks.onResult) {
            // Always delivered, even empty: it supersedes the provisional text
            // for this span.
            m_callbacks.onResult({m_origin, split, text, true});
        }

        // Start again at the split with the audio fed after it.
        const auto keepFrom = static_cast<size_t>(split - m_origin);
        std::vector<float> rest(m_audio.begin() + static_cast<std::ptrdiff_t>(keepFrom),
                                m_audio.end());
        const qint64 fedEnd = m_fedEnd;
        restart(split);
        if (!rest.empty()) {
            m_audio = std::move(rest);
            accept(m_audio.data(), static_cast<int>(m_audio.size()));
            m_fedEnd = fedEnd;
            publishProvisional();
        }
    }

private:
    void restart(qint64 origin)
    {
        if (m_stream != nullptr) {
            SherpaOnnxDestroyOnlineStream(m_stream);
        }
        m_stream = SherpaOnnxCreateOnlineStream(m_recognizer);
        m_origin = origin;
        m_fedEnd = origin;
        m_audio.clear();
        m_lastText.clear();
    }

    void accept(const float* samples, int count)
    {
        SherpaOnnxOnlineStreamAcceptWaveform(m_stream, kRate, samples, count);
        decodeReady();
    }

    void decodeReady()
    {
        while (SherpaOnnxIsOnlineStreamReady(m_recognizer, m_stream) != 0) {
            SherpaOnnxDecodeOnlineStream(m_recognizer, m_stream);
        }
    }

    void publishProvisional()
    {
        const SherpaOnnxOnlineRecognizerResult* r =
            SherpaOnnxGetOnlineStreamResult(m_recognizer, m_stream);
        const QString text = r != nullptr ? QString::fromUtf8(r->text).trimmed() : QString();
        if (r != nullptr) {
            SherpaOnnxDestroyOnlineRecognizerResult(r);
        }
        if (text.isEmpty() || text == m_lastText) {
            return;
        }
        m_lastText = text;
        if (m_callbacks.onResult) {
            m_callbacks.onResult({m_origin, m_fedEnd, text, false});
        }
    }

    // The stream's text from tokens emitted before `split` on the caller's
    // timeline. Token times are seconds from the stream's start (a stream is
    // never reset, only replaced, so that start is m_origin). A model without
    // token times gives its whole text.
    QString textBefore(qint64 split) const
    {
        const SherpaOnnxOnlineRecognizerResult* r =
            SherpaOnnxGetOnlineStreamResult(m_recognizer, m_stream);
        if (r == nullptr) {
            return {};
        }
        QString text;
        if (r->timestamps == nullptr) {
            text = QString::fromUtf8(r->text);
        } else {
            for (int i = 0; i < r->count; ++i) {
                const qint64 at = m_origin + static_cast<qint64>(r->timestamps[i] * kRate);
                if (at >= split) {
                    break;
                }
                text += QString::fromUtf8(r->tokens_arr[i]);
            }
            // SentencePiece marks a word start with U+2581.
            text.replace(QChar(0x2581), QLatin1Char(' '));
        }
        SherpaOnnxDestroyOnlineRecognizerResult(r);
        return text.simplified();
    }

    LivePreviewCallbacks m_callbacks;
    std::shared_ptr<const SherpaModel> m_model; // outlives m_stream
    const SherpaOnnxOnlineRecognizer* m_recognizer = nullptr;
    const SherpaOnnxOnlineStream* m_stream = nullptr;
    qint64 m_origin = 0;
    qint64 m_fedEnd = 0;
    std::vector<float> m_audio;
    QString m_lastText;
};

} // namespace

bool sherpaLivePreviewSupported()
{
    return true;
}

LivePreviewFactory sherpaLivePreviewFactory(const QString& modelDir, int numThreads)
{
    // The model loads on the first session the factory starts and is shared by
    // every copy of the factory and every session after it, so a retune or an
    // engine rebuild does not load it again. A failed load is remembered too.
    // Copies can be called from two engines' feeder threads while one engine
    // replaces another, hence the lock.
    struct Cache {
        std::mutex mutex;
        std::shared_ptr<const SherpaModel> model;
    };
    auto cache = std::make_shared<Cache>();
    return [modelDir, numThreads, cache](LivePreviewCallbacks callbacks)
               -> std::unique_ptr<ILivePreview> {
        std::shared_ptr<const SherpaModel> model;
        {
            const std::lock_guard<std::mutex> lock(cache->mutex);
            if (!cache->model) {
                cache->model = std::make_shared<const SherpaModel>(modelDir, numThreads);
            }
            model = cache->model;
        }
        if (model->recognizer() == nullptr) {
            if (callbacks.onState) {
                callbacks.onState(LivePreviewState::Failed, model->error());
            }
            return nullptr;
        }
        auto preview = std::make_unique<SherpaLivePreview>(model, std::move(callbacks));
        preview->announceReady();
        return preview;
    };
}

#else

bool sherpaLivePreviewSupported()
{
    return false;
}

LivePreviewFactory sherpaLivePreviewFactory(const QString&, int)
{
    return {};
}

#endif

QString sherpaLivePreviewUnavailableReason()
{
    return sherpaLivePreviewSupported()
               ? QString()
               : QCoreApplication::translate(
                     "AetherSDR::LivePreview",
                     "This build has no sherpa-onnx, which the live preview needs.");
}

} // namespace AetherSDR
