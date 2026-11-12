#pragma once

#include "asr/AsrSegmenter.h"
#include "asr/IAsrBackend.h"
#include "asr/LivePreview.h"
#include "asr/LivePreviewAssembler.h"
#include "asr/SpeakerClusterer.h"
#include "core/PcmFrame.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <algorithm>
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

// Arbitrary-rate mono to 16 kHz mono (r8brain), rebuilt when the source rate
// changes. AsrWorker and LivePreviewFeeder each own one, built alike, so their
// outputs line up to within the resampler's group delay (a few ms).
class AsrSixteenKConverter {
public:
    AsrSixteenKConverter();
    ~AsrSixteenKConverter();
    // Returns the input unchanged when it is already 16 kHz.
    std::vector<float> convert(const QVector<float>& monoSamples, int sampleRate);
    void reset();

private:
    std::unique_ptr<Resampler> m_resampler;
    int m_srcRate = 0;
};

// Live-preview half of the ASR engine (docs/asr-copy-assist.md, "Live
// preview"): owns the streaming recogniser on a thread of its own. AsrWorker
// blocks for a second or more inside every backend decode; feeding the
// recogniser from there would hold back the audio it hears, and the gray text
// would stall at every segment close. AsrEngine posts each chunk here and to
// the worker, stamped with the same timeline position.
class LivePreviewFeeder : public QObject {
    Q_OBJECT
public:
    explicit LivePreviewFeeder(quint64 generation) : m_generation(generation) {}
    ~LivePreviewFeeder() override;

    // All on the feeder's thread (AsrEngine posts them).
    // An empty factory stops the preview.
    void setFactory(LivePreviewFactory factory);
    // timelineStart: this chunk's position in 16 kHz samples on AsrEngine's
    // timeline, which starts at 0 after every reset.
    void feed(const QVector<float>& monoSamples, int sampleRate, qint64 timelineStart,
              quint64 generation);
    void finalizeThrough(qint64 sample, quint64 generation);
    // A new timeline: drop the session; the next audio starts another.
    void reset(quint64 generation);

signals:
    // Emitted from the recogniser's own threads.
    void previewResult(qint64 startSample, qint64 endSample, const QString& text, bool isFinal,
                       quint64 generation);
    void previewState(int state, const QString& message, quint64 generation);

private:
    LivePreviewFactory m_factory;
    std::unique_ptr<ILivePreview> m_preview;
    AsrSixteenKConverter m_toSixteenK;
    quint64 m_generation = 0;
    bool m_fedAny = false;      // m_fedEnd is valid for this session
    qint64 m_fedEnd = 0;        // where the next contiguous chunk starts
    qint64 m_lastFinalize = 0;
};

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
    void setInputGeneration(quint64 generation) { m_requestedGeneration.store(generation); }

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
    // timelineStart: the chunk's position on AsrEngine's 16 kHz timeline, which
    // live-preview positions share (see m_timelineOffset).
    void processAudio(const QVector<float>& monoSamples, int sampleRate, quint64 generation,
                      const AetherSDR::PcmEpochLease& source, qint64 timelineStart);
    void setMaxSegmentMs(int ms);
    void setSpeechRms(float rms);
    void setHangoverMs(int ms);
    void setOverlapMs(int ms);
    void setSpeakerThreshold(float t);
    // Opt-in (RFC #4818), see IAsrBackend::setContextCarryEnabled.
    void setContextCarryEnabled(bool on);
    void clearContext();   // flush carried context (long gap / Clear button)
    // The engine dropped audio between the previous chunk and the next one
    // (backlog ceiling, #5730): close out the partial segment and the carried
    // text/context so a segment never splices audio from either side of the
    // gap. Narrower than reset() — speaker labels and the resampler survive.
    void markDiscontinuity();
    void reset(quint64 generation);
    // Clear the cancel flag that restartSession() raised. A separate slot, not
    // part of reset(), so this only ever runs for a tap-point switch: the
    // ASR-disable and shutdown paths keep the flag latched as they always did.
    void resumeAfterCancel(quint64 generation);

signals:
    void loaded();
    void loadFailed(const QString& error);
    void speakerModelLoaded(const QString& modelPath, bool loaded);
    // speaker: 0-based cluster index (A/B/C…), or -1 when labeling is off.
    void segmentText(const QString& text, float confidence, int speaker, quint64 generation,
                     const AetherSDR::PcmEpochLease& source, quint64 segmentId);
    // A segment closed but produced no text (empty, failed, all overlap, or no
    // model): its live-preview block is resolved with nothing.
    void segmentDiscarded(quint64 segmentId, quint64 generation);
    // Every closed segment, before its decode starts (live-preview block edge),
    // with its end on AsrEngine's timeline.
    void previewBlockClosed(quint64 segmentId, qint64 endSample, quint64 generation);
    // The segmenter threw away audio too short to count as speech, ending at
    // endSample on AsrEngine's timeline: preview text for it has no segment to
    // replace it.
    void previewSpanDiscarded(qint64 endSample, quint64 generation);
    void processedMs(double ms); // this chunk fully handled (for the backlog meter)
    void errorOccurred(const QString& error);

private:
    // Transcribe closed segments in order and emit their text (overlap de-dup,
    // speaker label). Requires a loaded backend. Shared by processAudio() and
    // markDiscontinuity().
    void decodeSegments(std::vector<AsrSegmenter::ClosedSegment>& segments);
    // Live preview: report the block edges and discarded audio of the feed
    // that just ran, on AsrEngine's timeline.
    void closePreviewBlocks(const std::vector<AsrSegmenter::ClosedSegment>& segments);
    void discardSegments(const std::vector<AsrSegmenter::ClosedSegment>& segments);

    // AsrEngine's timeline minus the audio this worker has accepted, as of the
    // latest chunk. The worker skips stale or cancelled chunks the engine
    // counted, so the two differ; adding this maps a segmenter position onto
    // the timeline the live preview uses. Accepted audio is counted at the
    // exact rate ratio, the way the engine counts, rather than from
    // samplesFed(), so the mapping never depends on how the resampler batches
    // its output.
    qint64 m_timelineOffset = 0;
    double m_accepted16k = 0.0;

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
    QString m_prevSegmentText;          // last decode's text — tail source for overlap de-dup (#4821)
    AsrSixteenKConverter m_toSixteenK;
    bool m_warnedNoModel = false;
    quint64 m_generation = 0;
    std::atomic<quint64> m_requestedGeneration{0};
    PcmEpochLease m_source;
    bool inputCurrent() const;
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

    // Feed mono audio at its native rate; ignored unless enabled. Copies and posts to
    // the worker (which resamples to 16 kHz); no work on the audio thread. Bounded
    // (#5730): past asrBacklogHighWaterMs() chunks are dropped (droppedAudioChanged)
    // until the backlog drains to half, then the worker starts a fresh segment.
    void pushAudio(const QVector<float>& monoSamples, int sampleRate);
    void pushNativePcm(const AetherSDR::PcmFrame& frame);

    // Segmentation tuning (applied on the worker thread):
    //  - decode buffer: max audio (ms) before a decode is forced without silence
    //  - speech RMS: VAD energy threshold (lower = more sensitive)
    //  - silence duration: trailing silence (ms) that closes an utterance
    void setDecodeBufferMs(int ms);
    void setSpeechRms(float rms);
    void setSilenceDurationMs(int ms);
    //  - overlap: boundary-word recovery window (ms) carried across a cap-forced
    //    segment close so a word split at the cut isn't lost (RFC #4821). 0 = off.
    void setOverlapMs(int ms);
    //  - speaker threshold: cosine match threshold for A/B/C clustering (0..1)
    void setSpeakerThreshold(float threshold);

    // Opt-in (RFC #4818), applied live, no engine rebuild:
    //  - context carry: condition each decode on the backend's own previous
    //    confident output, for continuity across segment boundaries (off =
    //    independent decodes, the historical default)
    void setContextCarryEnabled(bool on);
    // Flush any carried decode context (Copy Assist Clear button); the display
    // and the context reset together for a clean fresh start.
    void clearContext();

    void reset();

    // Copy Assist's tap point changed: drop the audio already queued to the
    // worker, because it came from the other chain, then carry on. reset()
    // alone cannot do this — it is a queued signal, so it would sit BEHIND the
    // very backlog it is meant to clear (the same reason setEnabled(false)
    // pairs it with the worker's cancel flag). Ordering here relies on the
    // worker's queue being FIFO: the cancel flag is raised directly, so every
    // already-queued processAudio() no-ops, then reset() runs, then
    // resumeAfterCancel() clears the flag — so the no-op window ends exactly
    // at the backlog boundary and new-chain audio is never dropped.
    void restartSession();

    // Live preview (docs/asr-copy-assist.md): provisional text from a streaming
    // recogniser while the backend waits for its segment to close. Applied
    // live; an empty factory turns it off.
    void setLivePreviewFactory(LivePreviewFactory factory);
    bool isLivePreviewEnabled() const { return m_livePreviewEnabled; }
    // The generation previewText currently carries (livePreviewReset moves it).
    quint64 previewGeneration() const { return m_generation; }

signals:
    void ready();
    void loadFailed(const QString& error);
    void speakerModelLoaded(const QString& modelPath, bool loaded);
    // speaker: 0-based speaker index (A/B/C…), or -1 when labeling is off.
    // segmentId: the segment this text decodes; it resolves that segment's
    // live-preview block (previewText) when a preview is running.
    void finalText(const QString& text, float confidence, int speaker, quint64 segmentId);
    // Live preview (setLivePreviewFactory): provisional text for one segment,
    // replacing any earlier previewText for the same id. This is the preview's
    // stream seam, beside finalText: segment ids restart at 0 with each
    // generation (livePreviewReset), so a consumer away from the engine can
    // tell a stale update from a current one.
    void previewText(quint64 segmentId, const QString& text, quint64 generation);
    // That segment produced no final text: drop its provisional text.
    void previewDiscarded(quint64 segmentId, quint64 generation);
    // LivePreviewState as int, with a message for the status line.
    void livePreviewStatus(int state, const QString& message);
    // reset() restarted the segment-id timeline at 0 under a new generation:
    // drop every provisional block and forget which ids were already resolved.
    void livePreviewReset(quint64 generation);
    // Transcription backlog: seconds of received audio not yet handled by the
    // worker (grows when it can't keep up with real time).
    void backlogChanged(double seconds);
    // Seconds of audio dropped at the backlog ceiling since the last reset()
    // (0.1 s resolution, emitted only when it moves). 0 = keeping up.
    void droppedAudioChanged(double seconds);
    void error(const QString& error);

    // Internal: engine -> worker (queued). Not part of the public contract.
    void requestLoad(const QString& modelPath);
    void requestLoadSpeakerModel(const QString& modelPath);
    void requestSetSpeakerLabelingEnabled(bool on);
    void requestProcess(const QVector<float>& monoSamples, int sampleRate, quint64 generation,
                        const AetherSDR::PcmEpochLease& source, qint64 timelineStart);
    void requestSetMaxSegmentMs(int ms);
    void requestSetSpeechRms(float rms);
    void requestSetHangoverMs(int ms);
    void requestSetOverlapMs(int ms);
    void requestSetSpeakerThreshold(float threshold);
    void requestSetContextCarryEnabled(bool on);
    void requestClearContext();
    void requestMarkDiscontinuity();
    void requestReset(quint64 generation);
    void requestResumeAfterCancel(quint64 generation);

private:
    friend struct AsrEngineTestAccess;
    void startThread(AsrBackendFactory factory, const AsrSegmenter::Config& segConfig,
                     AsrSpeakerEmbedderFactory speakerEmbedderFactory);

    void pushAudioInput(const QVector<float>& monoSamples, int sampleRate, const PcmEpochLease& source);
    quint64 m_generation = 0;

    void updateBacklog(); // recompute lag = pushed − processed, emit if it moved
    void updateDropped(); // emit droppedAudioChanged if the total moved

    QThread* m_thread = nullptr;
    AsrWorker* m_worker = nullptr;
    // Live preview: its own thread, started the first time a preview is
    // turned on and kept until the engine goes (an idle thread costs nothing).
    void startPreviewThread();
    void postToFeeder(std::function<void(LivePreviewFeeder*)> call);
    QThread* m_previewThread = nullptr;
    LivePreviewFeeder* m_feeder = nullptr;
    LivePreviewAssembler m_previewAssembler;
    bool m_livePreviewEnabled = false;
    // Audio handed to the worker since the last reset(), in 16 kHz samples:
    // the timeline that segment edges and preview results share. Dropped
    // chunks (backlog ceiling) are not counted, so it has no gaps.
    double m_timeline16k = 0.0;
    bool m_enabled = false;
    bool m_ready = false;
    QString m_modelPath;
    bool m_speakerLabelingEnabled = false;
    double m_pushedMs = 0.0;         // audio handed to the engine (main thread)
    double m_processedMs = 0.0;      // audio the worker reports as handled
    double m_lastBacklogTenths = -1; // last emitted backlog (0.1 s units) — dedup
    // Audio that was queued at the last reset(): the worker still reports each
    // of those chunks as processed (in order, once each) after the counters
    // were zeroed, so those reports pay this down instead of driving
    // m_processedMs past m_pushedMs — which would read as a negative lag and
    // raise the ceiling by the stale amount (#5730 review).
    double m_staleMs = 0.0;
    // Backlog ceiling (#5730). The decode buffer is mirrored here because the
    // ceiling scales with it; the worker holds the copy the segmenter uses.
    int m_decodeBufferMs = AsrSegmenter::Config{}.maxSegmentMs;
    bool m_dropping = false;         // above the ceiling; pushAudio drops chunks
    double m_droppedMs = 0.0;        // audio dropped since the last reset()
    double m_droppedAtEntryMs = 0.0; // m_droppedMs when the current episode began
    double m_lastDroppedTenths = -1; // last emitted dropped total — dedup
};

// Backlog ceiling for a given decode buffer (#5730): twice the buffer, never
// below 10 s. A decode of one full buffer legitimately shows a backlog of its
// own wall time — twice the buffer leaves room for a slower-than-realtime CPU
// that still catches up in the next silence; the floor keeps a 1 s buffer from
// being cut at every decode. Dropping stops once the backlog has drained to
// half the ceiling or less. Pure so the test can pin the shape.
inline int asrBacklogHighWaterMs(int decodeBufferMs)
{
    constexpr int kFloorMs = 10000;
    return std::max(kFloorMs, 2 * std::max(0, decodeBufferMs));
}

} // namespace AetherSDR
