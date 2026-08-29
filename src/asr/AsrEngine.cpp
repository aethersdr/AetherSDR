#include "asr/AsrEngine.h"

#include "asr/SileroVad.h"
#include "asr/SpeakerEmbedder.h"
#include "core/Resampler.h"
#include "core/ShutdownTrace.h"

#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <utility>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcAsrEngine, "aether.asr.engine")

namespace {
constexpr int kAsrRate = 16000;      // whisper's required rate
constexpr int kResampleBlock = 4096; // max samples per r8brain process() call
// Wide transition band -> short FIR -> low latency. Speech lives well below the
// 8 kHz downsampled Nyquist, so a 10%-of-Nyquist guard is ample and keeps the
// resampler's group delay small (important for live copy and for prompt segment
// close-out).
constexpr double kResampleTransBand = 10.0;

// Segment-overlap de-dup (RFC #4821, Approach A). A cap-forced segment carries a
// little trailing audio into the next one, so the next decode re-transcribes the
// tail of the previous segment as its own leading words. We strip that repeated
// prefix at the text level — backend-agnostic, no whisper-param change, and it
// keeps the byte-safe segment text (no per-token UTF-8 reconstruction).

// Normalize a word for boundary comparison: drop leading/trailing non-alnum
// (punctuation whisper may add on one side of the cut but not the other) and
// lowercase, so "fox." and "Fox" match across the boundary.
QString normWord(const QString& w)
{
    int a = 0;
    int b = static_cast<int>(w.size());
    while (a < b && !w.at(a).isLetterOrNumber()) ++a;
    while (b > a && !w.at(b - 1).isLetterOrNumber()) --b;
    return w.mid(a, b - a).toLower();
}

// Hard ceiling on the compared window regardless of overlapMs — a coincidental
// long repeat must never swallow genuine new text.
constexpr int kMaxOverlapWords = 12;
// Rough shortest-plausible word length; overlapMs / this + a margin bounds how
// many leading words the carried window could actually duplicate. Keeping the
// strip near the real overlap (rather than the unconstrained longest match)
// limits over-stripping genuine repeated speech at the boundary (Approach A is
// deliberately lossy only at the margins — see RFC #4821).
constexpr int kMinWordMs = 200;

// Strip from `next` the longest run of leading words that duplicates the tail of
// `prev` (word-level, normalized); return the remaining text. `overlapMs` is the
// configured carry window — the comparison is bounded to about that many words.
// No match → `next` unchanged.
QString stripOverlapWords(const QString& prev, const QString& next, int overlapMs)
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList prevWords = prev.split(ws, Qt::SkipEmptyParts);
    const QStringList nextWords = next.split(ws, Qt::SkipEmptyParts);
    // Bound the window by the carry duration (words the overlap could hold),
    // then by the hard ceiling and the two token counts.
    const int overlapWordBudget = overlapMs > 0 ? overlapMs / kMinWordMs + 1 : 0;
    const int cap = std::min({overlapWordBudget, kMaxOverlapWords,
                              static_cast<int>(prevWords.size()),
                              static_cast<int>(nextWords.size())});
    // Pre-normalize only the tokens in range: prev's last `cap`, next's first `cap`.
    std::vector<QString> prevTail;
    std::vector<QString> nextHead;
    prevTail.reserve(cap);
    nextHead.reserve(cap);
    for (int i = 0; i < cap; ++i) {
        prevTail.push_back(normWord(prevWords.at(prevWords.size() - cap + i)));
        nextHead.push_back(normWord(nextWords.at(i)));
    }
    int best = 0;
    for (int k = 1; k <= cap; ++k) {
        bool match = true;
        for (int j = 0; j < k; ++j) {
            const QString& p = prevTail.at(cap - k + j);
            const QString& n = nextHead.at(j);
            // A punctuation-only token normalizes to "" — never let "" == ""
            // count as a boundary-word match (it isn't a real repeated word).
            if (p.isEmpty() || n.isEmpty() || p != n) {
                match = false;
                break;
            }
        }
        if (match) {
            best = k; // keep the longest k that fully matches
        }
    }
    if (best == 0) {
        return next;
    }
    // Return the ORIGINAL text sliced at the best-th word, not a rejoin of the
    // split tokens. split("\\s+")+join(' ') would collapse internal whitespace
    // runs, flatten tabs/non-breaking spaces, and drop whisper's leading space —
    // but only on segments that happened to match, so de-dup'd continuations
    // would be formatted differently from untouched ones (return next; above).
    // Find where the best-th word starts and slice there (robust to a leading
    // space; best == nextWords.size() slices to "" — a fully-overlapped segment).
    static const QRegularExpression word(QStringLiteral("\\S+"));
    QRegularExpressionMatchIterator it = word.globalMatch(next);
    qsizetype offset = next.size();
    for (int i = 0; it.hasNext(); ++i) {
        const QRegularExpressionMatch m = it.next();
        if (i == best) {
            offset = m.capturedStart();
            break;
        }
    }
    return next.mid(offset);
}
} // namespace

// ---- AsrWorker -------------------------------------------------------------

AsrWorker::AsrWorker(AsrBackendFactory factory, AsrSegmenter::Config segConfig,
                     AsrSpeakerEmbedderFactory speakerEmbedderFactory)
    : m_factory(std::move(factory))
    , m_speakerEmbedderFactory(std::move(speakerEmbedderFactory))
    , m_segmenter(segConfig)
    , m_vadModelPath(segConfig.vadModelPath)
    , m_clusterer(segConfig.speakerThreshold)
{
}

AsrWorker::~AsrWorker()
{
    if (m_backend != nullptr) {
        m_backend->unload();
    }
}

void AsrWorker::init()
{
    // ~AsrEngine() joins this thread, so every stage that can run long is a
    // stage the GUI thread may end up waiting on. A stage already in flight
    // still has to finish, but one that has not started must not begin after
    // shutdown was requested — that is what bounds the join (#4737).
    if (m_shuttingDown.load(std::memory_order_relaxed)) {
        return;
    }
    m_backend = m_factory ? m_factory() : nullptr;
    if (m_backend == nullptr) {
        emit errorOccurred(QStringLiteral("ASR backend could not be created."));
    }
    if (m_shuttingDown.load(std::memory_order_relaxed)) {
        return;
    }

    // Build the learned VAD on the worker thread (the ONNX session must live
    // here). On any failure, leave the segmenter on its energy VAD.
    if (!m_vadModelPath.empty()) {
        auto vad = std::make_unique<SileroVad>();
        if (vad->load(m_vadModelPath)) {
            m_vad = std::move(vad);
            m_segmenter.setVad(m_vad.get());
            qCInfo(lcAsrEngine, "ASR: Silero VAD loaded from %s",
                   m_vadModelPath.c_str());
        } else {
            qCWarning(lcAsrEngine, "ASR: Silero VAD load failed (%s) — using energy VAD",
                      m_vadModelPath.c_str());
        }
    }

    // No speaker embedder is built here: it is a runtime property loaded via
    // loadSpeakerModel() so the operator's labeling toggle never rebuilds the
    // engine (#4737). AsrEngine::setSpeakerModelPath() is its only entry point.
}

void AsrWorker::loadModel(const QString& modelPath)
{
    if (m_backend == nullptr) {
        emit loadFailed(QStringLiteral("No ASR backend."));
        return;
    }
    QString error;
    if (m_backend->load(modelPath, &error)) {
        m_warnedNoModel = false;
        m_segmenter.reset();
        m_clusterer.reset();
        m_prevSegmentText.clear();
        emit loaded();
    } else {
        emit loadFailed(error);
    }
}

void AsrWorker::loadSpeakerModel(const QString& modelPath)
{
    if (modelPath.isEmpty()) {
        return;
    }
    if (m_shuttingDown.load(std::memory_order_relaxed)) {
        // Building the ~24 MB ECAPA session here would be time the destructor's
        // join has to wait out. Report the request as unfulfilled instead; the
        // engine is going away, so nothing is left to observe it. NOT gated on
        // m_cancelPending — that flag is also set whenever the tap is disabled,
        // which is exactly when a speaker load is queued.
        emit speakerModelLoaded(modelPath, false);
        return;
    }

    std::unique_ptr<SpeakerEmbedder> embedder;
    if (m_speakerEmbedderFactory) {
        embedder = m_speakerEmbedderFactory(modelPath);
    } else {
        auto candidate = std::make_unique<SpeakerEmbedder>();
        if (candidate->load(modelPath.toStdString())) {
            embedder = std::move(candidate);
        }
    }

    if (embedder) {
        m_embedder = std::move(embedder);
        m_speakerModelPath = modelPath.toStdString();
        m_clusterer.reset();
        qCInfo(lcAsrEngine, "ASR: speaker embedder loaded from %s",
               m_speakerModelPath.c_str());
        emit speakerModelLoaded(modelPath, true);
    } else {
        // A replacement that fails must not keep assigning labels through the
        // previous model. Dropping the embedder is what guarantees that —
        // m_speakerLabelingEnabled stays the operator's intent and is left
        // alone, so the two never disagree about who owns which fact.
        m_embedder.reset();
        m_clusterer.reset();
        m_speakerModelPath.clear();
        qCWarning(lcAsrEngine, "ASR: speaker embedder load failed (%s) — no labeling",
                  qPrintable(modelPath));
        emit speakerModelLoaded(modelPath, false);
    }
}

void AsrWorker::setSpeakerLabelingEnabled(bool on)
{
    m_speakerLabelingEnabled = on;
}

std::vector<float> AsrWorker::toSixteenK(const QVector<float>& monoSamples, int sampleRate)
{
    const int rate = sampleRate > 0 ? sampleRate : kAsrRate;
    if (rate == kAsrRate) {
        return std::vector<float>(monoSamples.constBegin(), monoSamples.constEnd());
    }

    // Rebuild the resampler if the source rate changed (or first use).
    if (!m_resampler || m_resamplerSrcRate != rate) {
        m_resampler = std::make_unique<Resampler>(static_cast<double>(rate),
                                                  static_cast<double>(kAsrRate), kResampleBlock,
                                                  kResampleTransBand);
        m_resamplerSrcRate = rate;
    }

    // r8brain processes at most kResampleBlock samples per call; chunk the input.
    const int total = static_cast<int>(monoSamples.size());
    std::vector<float> out;
    out.reserve(static_cast<size_t>(total) * kAsrRate / rate + 16);
    for (int off = 0; off < total; off += kResampleBlock) {
        const int n = std::min(kResampleBlock, total - off);
        const QByteArray block = m_resampler->process(monoSamples.constData() + off, n);
        const auto* f = reinterpret_cast<const float*>(block.constData());
        const int count = block.size() / static_cast<int>(sizeof(float));
        out.insert(out.end(), f, f + count);
    }
    return out;
}

void AsrWorker::processAudio(const QVector<float>& monoSamples, int sampleRate)
{
    // Duration of this input chunk; reported as "processed" once the worker has
    // handled it (including any blocking transcription), so the engine can show a
    // backlog = received − processed. The body runs in a lambda so every exit
    // path reports exactly once.
    const int rate = sampleRate > 0 ? sampleRate : kAsrRate;
    const double chunkMs = 1000.0 * static_cast<double>(monoSamples.size()) / rate;

    [&] {
    if (m_cancelPending.load(std::memory_order_relaxed)) {
        // Shutting down or ASR was disabled: drop this queued chunk instead of
        // segmenting/blocking-transcribing it. Still falls through to
        // emit processedMs(chunkMs) below so the backlog meter's accounting
        // stays consistent.
        return;
    }
    if (monoSamples.isEmpty()) {
        return;
    }

    const std::vector<float> pcm16k = toSixteenK(monoSamples, sampleRate);
    if (pcm16k.empty()) {
        return;
    }

    std::vector<AsrSegmenter::ClosedSegment> segments =
        m_segmenter.feed(pcm16k.data(), static_cast<int>(pcm16k.size()));

    // A real pause (long idle silence) flushes any carried ASR context so a
    // noisy/garbled prior can't keep conditioning later utterances and never
    // recover. Consume the one-shot now (it must clear even on a feed that
    // produced no segment, so a gap spanning several silent buffers isn't held
    // pending), but apply the flush AFTER decoding this feed's own segments
    // below: a segment that closed BEFORE the gap should still be decoded with
    // the previous context — the flush protects only what comes after the pause.
    //
    // Caller constraint: this relies on a single feed() never both closing an
    // utterance and crossing longGapMs of idle silence. AsrAudioTap delivers
    // chunks far shorter than longGapMs (2.5 s), so a close and a long gap never
    // share one feed; a much larger buffer would break that and want revisiting.
    const bool longGap = m_segmenter.consumeLongGap();

    if (segments.empty()) {
        if (longGap && m_backend != nullptr) {
            m_backend->resetContext();
        }
        return;
    }

    if (m_backend == nullptr || !m_backend->isLoaded()) {
        if (!m_warnedNoModel) {
            m_warnedNoModel = true;
            emit errorOccurred(QStringLiteral("Speech detected but no ASR model is loaded."));
        }
        return;
    }

    for (AsrSegmenter::ClosedSegment& seg : segments) {
        QString error;
        const AsrTranscript result = m_backend->transcribe(seg.samples, &error);
        if (!error.isEmpty()) {
            emit errorOccurred(error);
            // A failed decode yields no tail — same as an empty one below: drop
            // the reference so the next continuation doesn't de-dup against a
            // stale, pre-failure segment.
            m_prevSegmentText.clear();
            continue;
        }
        if (result.text.isEmpty()) {
            // Nothing decoded. Clear the tail so the NEXT continuation (whose
            // carried audio overlaps THIS segment, not the one before it) doesn't
            // de-dup against a two-segments-old tail — m_prevSegmentText must
            // always mirror the immediately-preceding segment.
            m_prevSegmentText.clear();
            continue;
        }
        // Segment overlap (RFC #4821): when this segment was seeded with audio
        // carried across the previous cap-forced close, its leading words repeat
        // that segment's tail — strip them so nothing is emitted twice.
        QString emitText = result.text;
        if (seg.continuesPrevious && !m_prevSegmentText.isEmpty()) {
            // Use the window captured when THIS segment closed, not the live
            // slider — a mid-backlog slider move must not re-scope a queued strip.
            emitText = stripOverlapWords(m_prevSegmentText, result.text, seg.overlapMs);
        }
        m_prevSegmentText = result.text; // full decode is the tail source for the next continuation
        if (emitText.isEmpty()) {
            continue; // the whole segment was overlap (rare) — nothing new to emit
        }
        // Speaker label (A/B/C…) from the utterance's embedding, when enabled.
        // Known limitation (RFC #4821): for a continuation segment this embeds the
        // whole buffer, including the carried overlap prefix that also fed the
        // previous segment's embedding — a small same-speaker weighting bias
        // (never a different voice), only material when speaker labeling AND
        // overlap are both on with a small decode buffer. Not corrected here; a
        // fix would thread the carried-sample count through to skip the prefix.
        int speaker = -1;
        if (m_speakerLabelingEnabled && m_embedder) {
            speaker = m_clusterer.assign(
                m_embedder->embed(seg.samples.data(), static_cast<int>(seg.samples.size())));
        }
        emit segmentText(emitText, result.confidence, speaker);
    }

    // Apply the long-gap flush now, after this feed's own (pre-gap) segments have
    // been decoded with the context they should have had — so the pause only
    // affects the next utterance. (Guard kept though m_backend is non-null here.)
    if (longGap && m_backend != nullptr) {
        m_backend->resetContext();
    }
    }();

    emit processedMs(chunkMs);
}

void AsrWorker::setMaxSegmentMs(int ms)
{
    m_segmenter.setMaxSegmentMs(ms);
}

void AsrWorker::setSpeechRms(float rms)
{
    m_segmenter.setSpeechRms(rms);
}

void AsrWorker::setHangoverMs(int ms)
{
    m_segmenter.setHangoverMs(ms);
}

void AsrWorker::setOverlapMs(int ms)
{
    m_segmenter.setOverlapMs(ms);
}

void AsrWorker::setSpeakerThreshold(float t)
{
    m_clusterer.setThreshold(t);
}

void AsrWorker::setContextCarryEnabled(bool on)
{
    if (m_backend) {
        m_backend->setContextCarryEnabled(on);
    }
}

void AsrWorker::clearContext()
{
    // Explicit flush (the panel's Clear button): drop the carried prompt so the
    // next utterance starts clean, matching the cleared display.
    if (m_backend) {
        m_backend->resetContext();
    }
}

void AsrWorker::reset()
{
    m_segmenter.reset();
    // A retune/clear is a new context — don't prime the next decode with the
    // previous QSO's carried prompt (retune is the one boundary where the
    // operator has explicitly signalled a fresh start). Mirrors clearContext().
    if (m_backend) {
        m_backend->resetContext();
    }
    m_clusterer.reset(); // new session/frequency → relabel speakers from A
    m_prevSegmentText.clear(); // a Clear/retune must not de-dup against stale text
    m_resampler.reset();
    m_resamplerSrcRate = 0;
}

// ---- AsrEngine -------------------------------------------------------------

AsrEngine::AsrEngine(AsrBackendFactory factory, QObject* parent)
    : AsrEngine(std::move(factory), AsrSegmenter::Config{}, parent)
{
}

AsrEngine::AsrEngine(AsrBackendFactory factory, const AsrSegmenter::Config& segConfig,
                     QObject* parent, AsrSpeakerEmbedderFactory speakerEmbedderFactory)
    : QObject(parent)
{
    startThread(std::move(factory), segConfig, std::move(speakerEmbedderFactory));
}

void AsrEngine::startThread(AsrBackendFactory factory, const AsrSegmenter::Config& segConfig,
                            AsrSpeakerEmbedderFactory speakerEmbedderFactory)
{
    m_thread = new QThread(this);
    m_worker = new AsrWorker(std::move(factory), segConfig,
                             std::move(speakerEmbedderFactory));
    m_worker->moveToThread(m_thread);

    // Worker lifecycle: create the backend once the thread is running. The
    // worker is deleted manually after quit()+wait() in the destructor (a
    // finished->deleteLater would need a live event loop that no longer exists
    // once the thread has stopped), so it is intentionally parentless.
    connect(m_thread, &QThread::started, m_worker, &AsrWorker::init);

    // Engine -> worker (queued across threads).
    connect(this, &AsrEngine::requestLoad, m_worker, &AsrWorker::loadModel);
    connect(this, &AsrEngine::requestLoadSpeakerModel, m_worker, &AsrWorker::loadSpeakerModel);
    connect(this, &AsrEngine::requestSetSpeakerLabelingEnabled,
            m_worker, &AsrWorker::setSpeakerLabelingEnabled);
    connect(this, &AsrEngine::requestProcess, m_worker, &AsrWorker::processAudio);
    connect(this, &AsrEngine::requestSetMaxSegmentMs, m_worker, &AsrWorker::setMaxSegmentMs);
    connect(this, &AsrEngine::requestSetSpeechRms, m_worker, &AsrWorker::setSpeechRms);
    connect(this, &AsrEngine::requestSetHangoverMs, m_worker, &AsrWorker::setHangoverMs);
    connect(this, &AsrEngine::requestSetOverlapMs, m_worker, &AsrWorker::setOverlapMs);
    connect(this, &AsrEngine::requestSetSpeakerThreshold, m_worker, &AsrWorker::setSpeakerThreshold);
    connect(this, &AsrEngine::requestSetContextCarryEnabled, m_worker, &AsrWorker::setContextCarryEnabled);
    connect(this, &AsrEngine::requestClearContext, m_worker, &AsrWorker::clearContext);
    connect(this, &AsrEngine::requestReset, m_worker, &AsrWorker::reset);

    // Worker -> engine (queued back to the main thread).
    connect(m_worker, &AsrWorker::loaded, this, [this] {
        m_ready = true;
        emit ready();
    });
    connect(m_worker, &AsrWorker::loadFailed, this, [this](const QString& err) {
        m_ready = false;
        emit loadFailed(err);
    });
    // Forwarded verbatim: a failed load does NOT clear m_speakerLabelingEnabled.
    // That flag is the operator's intent and only setSpeakerLabelingEnabled()
    // writes it; the worker having dropped its embedder is what guarantees no
    // labels are produced. Deciding what a failure means for the UI (status,
    // un-checking the box) belongs to the controller, which owns that intent.
    connect(m_worker, &AsrWorker::speakerModelLoaded, this,
            &AsrEngine::speakerModelLoaded);
    connect(m_worker, &AsrWorker::segmentText, this,
            [this](const QString& text, float confidence, int speaker) {
                // A queued worker toggle can be behind an in-flight decode. Mask
                // its late result on the main side so the latest UI intent never
                // appends a stale [A]/[B] label.
                emit finalText(text, confidence, m_speakerLabelingEnabled ? speaker : -1);
            });
    connect(m_worker, &AsrWorker::processedMs, this, [this](double ms) {
        m_processedMs += ms;
        updateBacklog();
    });
    connect(m_worker, &AsrWorker::errorOccurred, this, &AsrEngine::error);

    m_thread->start();
}

AsrEngine::~AsrEngine()
{
    ShutdownTrace trace("asr.thread.join");
    if (m_thread != nullptr) {
        if (m_worker != nullptr) {
            // Qt's own quit() already stops the worker from starting any FURTHER
            // queued processAudio() calls once the current one returns — it does
            // not drain the whole backlog first. This guarantees that even more
            // reliably (in case that's ever platform/version-dependent) and
            // guards the one case Qt's quit() can't help with: a call already
            // in flight when shutdown starts still has to finish naturally (a
            // whisper decode, or a remote HTTP round-trip up to its own
            // timeout) — quit()+wait() below still waits for that one.
            m_worker->setCancelPending(true);
            // Separate from the cancel flag above, which is also raised every
            // time the audio tap is disabled: this one is set only here, so
            // init()/loadSpeakerModel() can skip a stage that has not started
            // without ever refusing a legitimate load (#4737).
            m_worker->requestShutdown();
        }
        m_thread->quit();
        m_thread->wait();
        // m_worker is deleted via the thread's finished -> deleteLater; but that
        // slot won't run without an event loop here, so delete it directly now
        // that the thread has stopped.
        delete m_worker;
        m_worker = nullptr;
    }
}

void AsrEngine::setEnabled(bool on)
{
    m_enabled = on;
    if (m_worker == nullptr) {
        return;
    }
    if (on) {
        m_worker->setCancelPending(false); // resume normal processing
    } else {
        // Drop any already-queued backlog now, not after it finishes: the
        // worker's still-blocking calls are cancel-checked, but reset() alone
        // (a queued signal) would just sit behind the same backlog it's meant
        // to clear.
        m_worker->setCancelPending(true);
        reset();
    }
}

void AsrEngine::setModelPath(const QString& modelPath)
{
    m_modelPath = modelPath;
    m_ready = false;
    emit requestLoad(modelPath);
}

void AsrEngine::setSpeakerModelPath(const QString& modelPath)
{
    if (!modelPath.isEmpty()) {
        emit requestLoadSpeakerModel(modelPath);
    }
}

void AsrEngine::setSpeakerLabelingEnabled(bool on)
{
    m_speakerLabelingEnabled = on;
    emit requestSetSpeakerLabelingEnabled(on);
}

void AsrEngine::pushAudio(const QVector<float>& monoSamples, int sampleRate)
{
    if (!m_enabled || monoSamples.isEmpty()) {
        return;
    }
    const int rate = sampleRate > 0 ? sampleRate : kAsrRate;
    m_pushedMs += 1000.0 * static_cast<double>(monoSamples.size()) / rate;
    updateBacklog();
    emit requestProcess(monoSamples, sampleRate);
}

void AsrEngine::updateBacklog()
{
    const double lag = std::max(0.0, (m_pushedMs - m_processedMs) / 1000.0);
    const double tenths = std::floor(lag * 10.0 + 0.5); // dedup at 0.1 s resolution
    if (tenths != m_lastBacklogTenths) {
        m_lastBacklogTenths = tenths;
        emit backlogChanged(tenths / 10.0);
    }
}

void AsrEngine::setDecodeBufferMs(int ms)
{
    emit requestSetMaxSegmentMs(ms);
}

void AsrEngine::setSpeechRms(float rms)
{
    emit requestSetSpeechRms(rms);
}

void AsrEngine::setSilenceDurationMs(int ms)
{
    emit requestSetHangoverMs(ms);
}

void AsrEngine::setOverlapMs(int ms)
{
    emit requestSetOverlapMs(ms);
}

void AsrEngine::setSpeakerThreshold(float threshold)
{
    emit requestSetSpeakerThreshold(threshold);
}

void AsrEngine::clearContext()
{
    emit requestClearContext();
}

void AsrEngine::setContextCarryEnabled(bool on)
{
    emit requestSetContextCarryEnabled(on);
}

void AsrEngine::reset()
{
    // A retune/clear drops buffered work; zero the backlog meter so it doesn't
    // carry a stale lag across the reset (late worker reports just clamp to 0).
    m_pushedMs = 0.0;
    m_processedMs = 0.0;
    updateBacklog();
    emit requestReset();
}

} // namespace AetherSDR
