#include "asr/AsrEngine.h"

#include "asr/SileroVad.h"
#include "asr/SpeakerEmbedder.h"
#include "core/Resampler.h"

#include <QLoggingCategory>
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

    std::vector<std::vector<float>> segments =
        m_segmenter.feed(pcm16k.data(), static_cast<int>(pcm16k.size()));
    if (segments.empty()) {
        return;
    }

    if (m_backend == nullptr || !m_backend->isLoaded()) {
        if (!m_warnedNoModel) {
            m_warnedNoModel = true;
            emit errorOccurred(QStringLiteral("Speech detected but no ASR model is loaded."));
        }
        return;
    }

    for (std::vector<float>& seg : segments) {
        QString error;
        const AsrTranscript result = m_backend->transcribe(seg, &error);
        if (!error.isEmpty()) {
            emit errorOccurred(error);
            continue;
        }
        if (result.text.isEmpty()) {
            continue;
        }
        // Speaker label (A/B/C…) from the utterance's embedding, when enabled.
        int speaker = -1;
        if (m_speakerLabelingEnabled && m_embedder) {
            speaker = m_clusterer.assign(
                m_embedder->embed(seg.data(), static_cast<int>(seg.size())));
        }
        emit segmentText(result.text, result.confidence, speaker);
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

void AsrWorker::setSpeakerThreshold(float t)
{
    m_clusterer.setThreshold(t);
}

void AsrWorker::reset()
{
    m_segmenter.reset();
    m_clusterer.reset(); // new session/frequency → relabel speakers from A
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
    connect(this, &AsrEngine::requestSetSpeakerThreshold, m_worker, &AsrWorker::setSpeakerThreshold);
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

void AsrEngine::setSpeakerThreshold(float threshold)
{
    emit requestSetSpeakerThreshold(threshold);
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
