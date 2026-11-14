#include "asr/LivePreview.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#ifdef HAVE_SHERPA
#include "sherpa-onnx/c-api/c-api.h"
#endif

namespace AetherSDR {

#ifdef HAVE_SHERPA

namespace {

constexpr int kRate = 16000;
// Silence fed after the last real sample when a stream is finished, so the
// encoder's look-ahead covers the audio just before the split. Kroko decodes
// in chunks of about 1.3 s; with 0.5 s its last 0.5 s of words never came out.
constexpr int kTailPadding = kRate * 3 / 2;

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
// finalizeThrough(s) has to split the text at s, which a sherpa stream cannot
// do by itself. The stream is finished (tail padding, input finished, decoded
// out), the words that START before s become the final result, and a fresh
// stream starts at the next word's onset with the audio from there fed again.
// Splitting between whole words, not tokens, keeps a word that straddles s
// intact: cutting at s itself turned "background" into "backgr" + "ound".
// For that the stream needs audio past s, so a split waits until kLookahead
// has been fed beyond it. The provisional text keeps updating meanwhile.
//
// Results are reported on the caller's timeline: a final covers
// [m_reportOrigin, s), and the next stream's results start at s, so finals
// tile the timeline even though each stream really starts at a word onset
// (m_origin, from which its token times count).
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
            if (m_pendingSplit >= 0) {
                split(std::exchange(m_pendingSplit, -1));
            }
            split(m_fedEnd);
            restart(startSample);
            m_reportOrigin = startSample;
            m_lastFinal = {};
        } else if (m_stream == nullptr) {
            restart(startSample);
            m_reportOrigin = startSample;
        }
        m_audio.insert(m_audio.end(), samples, samples + count);
        accept(samples, count);
        m_fedEnd = startSample + count;
        publishProvisional();
        if (m_pendingSplit >= 0 && m_fedEnd - m_pendingSplit >= kLookahead) {
            split(std::exchange(m_pendingSplit, -1));
        }
    }

    void finalizeThrough(qint64 sample) override
    {
        if (m_stream == nullptr) {
            return;
        }
        const qint64 at = std::min(sample, m_fedEnd);
        if (at <= m_reportOrigin) {
            // A segment edge reported after a split already made past it (the
            // 10 s forcing ran first): split that final again at the edge, so
            // its words after the edge move to the next block.
            resplitLastFinal(at);
            return;
        }
        // Two splits pending: make the earlier one now, without waiting.
        if (m_pendingSplit >= 0 && m_pendingSplit != at) {
            split(std::min(m_pendingSplit, at));
            m_pendingSplit = std::max(m_pendingSplit, at);
        } else {
            m_pendingSplit = at;
        }
        if (m_pendingSplit >= 0 && m_fedEnd - m_pendingSplit >= kLookahead) {
            split(std::exchange(m_pendingSplit, -1));
        }
    }

private:
    struct Word {
        qint64 start = 0;     // its first token, on the caller's timeline
        qint64 lastToken = 0; // its last token
        QString text;
    };
    struct Final {
        qint64 start = -1;
        qint64 end = -1;
        std::vector<Word> words;
    };

    // Waiting this long past a split lets the word across it be decoded whole.
    static constexpr qint64 kLookahead = kRate * 6 / 10;
    // How late a word's first token is timed after the word starts, at most:
    // measured at 0.2-0.3 s on Kroko.
    static constexpr qint64 kTokenLag = kRate * 4 / 10;
    // Audio a fresh stream hears before the first word it keeps.
    static constexpr qint64 kPrime = kRate * 2;

    // Where the word whose first token is at `wordAt` starts: the quietest
    // 20 ms of the stretch where its onset can be, which is no earlier than
    // kTokenLag before that token and no earlier than just before the
    // previous word's last token (`prevTokenAt`, -1 when none). A word
    // starting with a stop ("birch", "canoe") opens with a closure, so this
    // finds the gap between words even in fast speech.
    qint64 quietestBefore(qint64 wordAt, qint64 prevTokenAt) const
    {
        constexpr qint64 kFrame = kRate / 50;
        constexpr qint64 kHop = kRate / 100;
        qint64 lo = std::max(m_origin, wordAt - kTokenLag);
        if (prevTokenAt >= 0) {
            lo = std::max(lo, prevTokenAt - kRate / 10);
        }
        const qint64 hi = std::min(m_fedEnd - kFrame, wordAt - kRate / 20);
        if (hi <= lo) {
            return std::max(m_origin, std::min(wordAt, m_fedEnd));
        }
        qint64 best = lo;
        double bestEnergy = -1.0;
        for (qint64 at = lo; at <= hi; at += kHop) {
            double e = 0.0;
            const float* x = m_audio.data() + (at - m_origin);
            for (qint64 i = 0; i < kFrame; ++i) {
                e += double(x[i]) * x[i];
            }
            if (bestEnergy < 0.0 || e < bestEnergy) {
                bestEnergy = e;
                best = at;
            }
        }
        return best;
    }

    void split(qint64 at)
    {
        if (at <= m_reportOrigin) {
            return;
        }
        if (at <= m_origin) {
            // The stream holds no audio before the split: nothing to decode.
            deliverFinal({m_reportOrigin, at, {}});
            m_reportOrigin = at;
            return;
        }

        // Finish the stream so every word fed is decoded.
        const std::vector<float> silence(kTailPadding, 0.0f);
        accept(silence.data(), kTailPadding);
        SherpaOnnxOnlineStreamInputFinished(m_stream);
        decodeReady();
        qint64 lastTokenAt = -1;
        qint64 nextWordAt = -1;
        std::vector<Word> before = wordsSplitAt(at, &lastTokenAt, &nextWordAt);
        deliverFinal({m_reportOrigin, at, std::move(before)});

        // Resume at the next word's onset, which may lie before the split.
        // Without a next word in the audio fed, the rest is the tail of the
        // last word kept (or silence): skip past it.
        //
        // A fresh stream misses or mangles its first word, so it starts
        // kPrime earlier, on audio the final already covers, and drops the
        // words it hears there: those whose first token comes before
        // `keepFrom`, between the last token kept and the next word's first.
        qint64 resume = at;
        qint64 keepFrom = -1;
        if (nextWordAt >= 0) {
            resume = quietestBefore(nextWordAt, lastTokenAt);
            if (lastTokenAt >= 0) {
                keepFrom = (lastTokenAt + nextWordAt) / 2;
            }
        } else if (lastTokenAt >= 0) {
            resume = std::max(resume, std::min(m_fedEnd, lastTokenAt + kTokenLag));
            keepFrom = lastTokenAt + kRate / 10;
        }
        if (keepFrom >= 0) {
            resume -= kPrime;
        }
        resume = std::clamp(resume, m_origin, m_fedEnd);
        const auto keepAudioFrom = static_cast<size_t>(resume - m_origin);
        std::vector<float> rest(m_audio.begin() + static_cast<std::ptrdiff_t>(keepAudioFrom),
                                m_audio.end());
        const qint64 fedEnd = m_fedEnd;
        restart(resume);
        m_reportOrigin = at;
        m_keepFrom = keepFrom;
        if (!rest.empty()) {
            m_audio = std::move(rest);
            accept(m_audio.data(), static_cast<int>(m_audio.size()));
            m_fedEnd = fedEnd;
            publishProvisional();
        }
    }

    void resplitLastFinal(qint64 at)
    {
        if (at <= m_lastFinal.start || at >= m_lastFinal.end) {
            return;
        }
        Final head{m_lastFinal.start, at, {}};
        Final tail{at, m_lastFinal.end, {}};
        for (Word& w : m_lastFinal.words) {
            (w.start < at ? head : tail).words.push_back(std::move(w));
        }
        // The head replaces the earlier final (same start); the tail follows.
        deliverFinal(std::move(head));
        deliverFinal(std::move(tail));
    }

    void deliverFinal(Final f)
    {
        QStringList words;
        for (const Word& w : f.words) {
            words << w.text;
        }
        const qint64 start = f.start;
        const qint64 end = f.end;
        m_lastFinal = std::move(f);
        if (m_callbacks.onResult) {
            // Always delivered, even empty: it supersedes the provisional text
            // for this span.
            m_callbacks.onResult({start, end, words.join(QLatin1Char(' ')), true});
        }
    }

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
        m_keepFrom = -1;
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
        QStringList kept;
        for (const Word& w : words()) {
            kept << w.text;
        }
        const QString text = kept.join(QLatin1Char(' '));
        if (text.isEmpty() || text == m_lastText) {
            return;
        }
        m_lastText = text;
        if (m_callbacks.onResult) {
            m_callbacks.onResult({m_reportOrigin, m_fedEnd, text, false});
        }
    }

    // The stream's words, minus those before m_keepFrom (heard while priming).
    // Token times are seconds from the stream's start (a stream is never
    // reset, only replaced, so that start is m_origin). A token opens a word
    // when it starts with a space (SentencePiece's U+2581, which the C API may
    // already have turned into one). A model without token times gives its
    // whole text as one word.
    std::vector<Word> words() const
    {
        std::vector<Word> out;
        const SherpaOnnxOnlineRecognizerResult* r =
            SherpaOnnxGetOnlineStreamResult(m_recognizer, m_stream);
        if (r == nullptr) {
            return out;
        }
        if (r->timestamps == nullptr) {
            const QString text = QString::fromUtf8(r->text).simplified();
            if (!text.isEmpty()) {
                out.push_back({m_origin, m_origin, text});
            }
        } else {
            bool dropping = false;
            bool open = true; // the next token with text starts a word
            for (int i = 0; i < r->count; ++i) {
                QString token = QString::fromUtf8(r->tokens_arr[i]);
                token.replace(QChar(0x2581), QLatin1Char(' '));
                const qint64 t = m_origin + static_cast<qint64>(r->timestamps[i] * kRate);
                open = open || token.startsWith(QLatin1Char(' '));
                token = token.trimmed();
                if (token.isEmpty()) {
                    continue; // a bare word marker: the word starts at the next token
                }
                if (open) {
                    open = false;
                    dropping = t < m_keepFrom;
                    if (!dropping) {
                        out.push_back({t, t, token});
                    }
                } else if (!dropping && !out.empty()) {
                    out.back().text += token;
                    out.back().lastToken = t;
                }
            }
        }
        SherpaOnnxDestroyOnlineRecognizerResult(r);
        return out;
    }

    // The stream's words that start before `at`, with the time of the last
    // token among them and the start of the first word after them (-1 when
    // there is none).
    std::vector<Word> wordsSplitAt(qint64 at, qint64* lastTokenAt, qint64* nextWordAt) const
    {
        std::vector<Word> before = words();
        const auto after = std::find_if(before.begin(), before.end(),
                                        [at](const Word& w) { return w.start >= at; });
        if (after != before.end()) {
            *nextWordAt = after->start;
        }
        before.erase(after, before.end());
        if (!before.empty()) {
            *lastTokenAt = before.back().lastToken;
        }
        return before;
    }

    LivePreviewCallbacks m_callbacks;
    std::shared_ptr<const SherpaModel> m_model; // outlives m_stream
    const SherpaOnnxOnlineRecognizer* m_recognizer = nullptr;
    const SherpaOnnxOnlineStream* m_stream = nullptr;
    qint64 m_origin = 0;       // where the stream's audio starts
    qint64 m_reportOrigin = 0; // where its results start (the last split)
    qint64 m_fedEnd = 0;
    qint64 m_pendingSplit = -1;
    qint64 m_keepFrom = -1;     // words starting before this were heard priming
    std::vector<float> m_audio; // [m_origin, m_fedEnd)
    QString m_lastText;
    Final m_lastFinal;          // for a late edge inside it
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
