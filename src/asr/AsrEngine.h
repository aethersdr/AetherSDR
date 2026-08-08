#pragma once

#include "asr/AsrSegmenter.h"
#include "asr/IAsrBackend.h"
#include "asr/SpeakerClusterer.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

class QThread;

namespace AetherSDR {

class Resampler;
class SileroVad;
class SpeakerEmbedder;

// Factory that constructs an ASR backend. Invoked on the worker thread so the
// backend (and any model context) lives entirely there.
using AsrBackendFactory = std::function<std::unique_ptr<IAsrBackend>()>;
// Creates a fully loaded speaker embedder on the ASR worker thread. Production
// uses the default loader; tests may inject a deterministic slow loader to
// prove model preparation never occupies the caller thread.
using AsrSpeakerEmbedderFactory =
    std::function<std::unique_ptr<SpeakerEmbedder>(const QString& modelPath)>;

// Worker half of the ASR engine — runs on a dedicated thread, owns the backend
// and the segmenter, and does all CPU-heavy work (segmentation + inference).
// Never touched directly by callers; AsrEngine marshals to it via queued
// signals. (Declared here so AUTOMOC sees its Q_OBJECT.)
class AsrWorker : public QObject {
    Q_OBJECT
public:
    AsrWorker(AsrBackendFactory factory, AsrSegmenter::Config segConfig,
              AsrSpeakerEmbedderFactory speakerEmbedderFactory);
    ~AsrWorker() override;

    // Thread-safe; call directly (NOT via a queued signal — the whole point is
    // to take effect immediately, ahead of anything already queued). When true,
    // processAudio() becomes a no-op for any call already sitting in the event
    // queue, instead of draining the segmenter/blocking-transcribing through a
    // whole backlog. Used so shutdown and ASR-disable don't have to wait for a
    // backlog of queued segments to finish (each includes a blocking transcribe).
    void setCancelPending(bool cancel) { m_cancelPending.store(cancel, std::memory_order_relaxed); }

    // Thread-safe; set once by ~AsrEngine() before it joins this thread. Distinct
    // from setCancelPending() above, which is ALSO raised on every ASR-disable
    // (i.e. whenever the audio tap goes off) and so cannot mean "shutting down".
    // Stages that would otherwise lengthen the join — backend/VAD creation and
    // the ~24 MB speaker ONNX session — check this before starting (#4737).
    void requestShutdown() { m_shuttingDown.store(true, std::memory_order_relaxed); }

public slots:
    void init();                                   // create backend on this thread
    void loadModel(const QString& modelPath);
    void loadSpeakerModel(const QString& modelPath);
    void setSpeakerLabelingEnabled(bool on);
    // Mono float samples at sampleRate; resampled to whisper's 16 kHz on this
    // (worker) thread before segmentation — never on the audio/caller thread.
    void processAudio(const QVector<float>& monoSamples, int sampleRate);
    void setMaxSegmentMs(int ms);
    void setSpeechRms(float rms);
    void setHangoverMs(int ms);
    void setSpeakerThreshold(float t);
    void reset();

signals:
    void loaded();
    void loadFailed(const QString& error);
    void speakerModelLoaded(const QString& modelPath, bool loaded);
    // speaker: 0-based cluster index (A/B/C…), or -1 when labeling is off.
    void segmentText(const QString& text, float confidence, int speaker);
    void processedMs(double ms); // this chunk fully handled (for the backlog meter)
    void errorOccurred(const QString& error);

private:
    // Resample arbitrary-rate mono to 16 kHz mono (returns the input unchanged
    // when already 16 kHz). Builds/rebuilds the r8brain resampler on rate change.
    std::vector<float> toSixteenK(const QVector<float>& monoSamples, int sampleRate);

    AsrBackendFactory m_factory;
    AsrSpeakerEmbedderFactory m_speakerEmbedderFactory;
    std::unique_ptr<IAsrBackend> m_backend;
    AsrSegmenter m_segmenter;
    std::unique_ptr<SileroVad> m_vad;   // built in init() when a model path is set
    std::string m_vadModelPath;         // optional Silero VAD .onnx (empty = energy)
    std::unique_ptr<SpeakerEmbedder> m_embedder; // built by loadSpeakerModel()
    SpeakerClusterer m_clusterer;       // online A/B/C… labeling
    std::string m_speakerModelPath;     // path m_embedder was built from ("" = none)
    bool m_speakerLabelingEnabled = false; // operator intent; embedder presence gates
    std::unique_ptr<Resampler> m_resampler;
    int m_resamplerSrcRate = 0;
    bool m_warnedNoModel = false;
    std::atomic<bool> m_cancelPending{false}; // see setCancelPending()
    std::atomic<bool> m_shuttingDown{false};  // see requestShutdown()
};

// Engine half — main-thread facing. Accepts 16 kHz mono audio, ships it to the
// worker, and re-emits transcription results. Threading obeys the project rule:
// worker communicates only via auto-queued signals; no shared mutable state,
// no work on the audio callback (pushAudio only copies + posts).
class AsrEngine : public QObject {
    Q_OBJECT
public:
    // The only constructor: inject the backend factory. Production code passes
    // whisperAsrBackendFactory(); tests pass a deterministic fake.
    explicit AsrEngine(AsrBackendFactory factory, QObject* parent = nullptr);
    AsrEngine(AsrBackendFactory factory, const AsrSegmenter::Config& segConfig,
              QObject* parent = nullptr,
              AsrSpeakerEmbedderFactory speakerEmbedderFactory = {});
    ~AsrEngine() override;

    // Disabling drops any already-queued backlog (see AsrWorker::setCancelPending)
    // rather than letting the worker keep transcribing it after the UI already
    // says "Disabled". Re-enabling clears the cancel flag so work resumes.
    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

    // Load/switch the model file (async; emits ready() or loadFailed()).
    void setModelPath(const QString& modelPath);
    QString modelPath() const { return m_modelPath; }
    bool isReady() const { return m_ready; }

    // Speaker labeling is independent from the ASR backend. Loading is queued
    // onto the worker so UI toggles never tear down a busy inference thread.
    void setSpeakerModelPath(const QString& modelPath);
    void setSpeakerLabelingEnabled(bool on);
    // Pure operator intent: only setSpeakerLabelingEnabled() writes it, so it
    // stays true after a failed load. Whether labels are actually produced is
    // gated by the worker holding an embedder, which a failed load drops.
    bool isSpeakerLabelingEnabled() const { return m_speakerLabelingEnabled; }

    // Feed mono audio at its native sampleRate (e.g. the 24 kHz RX pipeline).
    // Ignored unless enabled. Cheap — copies and posts to the worker, which
    // resamples to 16 kHz. No work happens on the caller/audio thread.
    void pushAudio(const QVector<float>& monoSamples, int sampleRate);

    // Segmentation tuning (applied on the worker thread):
    //  - decode buffer: max audio (ms) before a decode is forced without silence
    //  - speech RMS: VAD energy threshold (lower = more sensitive)
    //  - silence duration: trailing silence (ms) that closes an utterance
    void setDecodeBufferMs(int ms);
    void setSpeechRms(float rms);
    void setSilenceDurationMs(int ms);
    //  - speaker threshold: cosine match threshold for A/B/C clustering (0..1)
    void setSpeakerThreshold(float threshold);

    void reset();

signals:
    void ready();
    void loadFailed(const QString& error);
    void speakerModelLoaded(const QString& modelPath, bool loaded);
    // speaker: 0-based speaker index (A/B/C…), or -1 when labeling is off.
    void finalText(const QString& text, float confidence, int speaker);
    // Transcription backlog: seconds of received audio not yet handled by the
    // worker (grows when it can't keep up with real time).
    void backlogChanged(double seconds);
    void error(const QString& error);

    // Internal: engine -> worker (queued). Not part of the public contract.
    void requestLoad(const QString& modelPath);
    void requestLoadSpeakerModel(const QString& modelPath);
    void requestSetSpeakerLabelingEnabled(bool on);
    void requestProcess(const QVector<float>& monoSamples, int sampleRate);
    void requestSetMaxSegmentMs(int ms);
    void requestSetSpeechRms(float rms);
    void requestSetHangoverMs(int ms);
    void requestSetSpeakerThreshold(float threshold);
    void requestReset();

private:
    void startThread(AsrBackendFactory factory, const AsrSegmenter::Config& segConfig,
                     AsrSpeakerEmbedderFactory speakerEmbedderFactory);

    void updateBacklog(); // recompute lag = pushed − processed, emit if it moved

    QThread* m_thread = nullptr;
    AsrWorker* m_worker = nullptr;
    bool m_enabled = false;
    bool m_ready = false;
    QString m_modelPath;
    bool m_speakerLabelingEnabled = false;
    double m_pushedMs = 0.0;         // audio handed to the engine (main thread)
    double m_processedMs = 0.0;      // audio the worker reports as handled
    double m_lastBacklogTenths = -1; // last emitted backlog (0.1 s units) — dedup
};

} // namespace AetherSDR
