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

AsrWorker::AsrWorker(AsrBackendFactory factory, AsrSegmenter::Config segConfig)
    : m_factory(std::move(factory))
    , m_segmenter(segConfig)
    , m_vadModelPath(segConfig.vadModelPath)
    , m_clusterer(segConfig.speakerThreshold)
    , m_speakerModelPath(segConfig.speakerModelPath)
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
    m_backend = m_factory ? m_factory() : nullptr;
    if (m_backend == nullptr) {
        emit errorOccurred(QStringLiteral("ASR backend could not be created."));
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

    // Speaker-embedding model for per-utterance A/B/C labeling (worker thread).
    if (!m_speakerModelPath.empty()) {
        auto embedder = std::make_unique<SpeakerEmbedder>();
        if (embedder->load(m_speakerModelPath)) {
            m_embedder = std::move(embedder);
            qCInfo(lcAsrEngine, "ASR: speaker embedder loaded from %s",
                   m_speakerModelPath.c_str());
        } else {
            qCWarning(lcAsrEngine, "ASR: speaker embedder load failed (%s) — no labeling",
                      m_speakerModelPath.c_str());
        }
    }
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
        if (m_embedder) {
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
                     QObject* parent)
    : QObject(parent)
{
    startThread(std::move(factory), segConfig);
}

void AsrEngine::startThread(AsrBackendFactory factory, const AsrSegmenter::Config& segConfig)
{
    m_thread = new QThread(this);
    m_worker = new AsrWorker(std::move(factory), segConfig);
    m_worker->moveToThread(m_thread);

    // Worker lifecycle: create the backend once the thread is running. The
    // worker is deleted manually after quit()+wait() in the destructor (a
    // finished->deleteLater would need a live event loop that no longer exists
    // once the thread has stopped), so it is intentionally parentless.
    connect(m_thread, &QThread::started, m_worker, &AsrWorker::init);

    // Engine -> worker (queued across threads).
    connect(this, &AsrEngine::requestLoad, m_worker, &AsrWorker::loadModel);
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
    connect(m_worker, &AsrWorker::segmentText, this, &AsrEngine::finalText);
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
