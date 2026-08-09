// Offline unit test for AsrEngine (RFC #4333, Phase 3). Uses a deterministic
// fake backend injected via the factory, so the engine's worker-thread
// orchestration — async load, audio -> segment -> transcribe -> finalText,
// enabled-gating, and load-failure — is verified without any model or whisper.

#include "asr/AsrEngine.h"
#include "asr/IAsrBackend.h"
#include "asr/SpeakerEmbedder.h"
#include "gui/CopyAssistController.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <cmath>
#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

// Deterministic fake: load() succeeds unless constructed to fail; transcribe()
// returns a fixed phrase for any non-empty utterance.
class FakeBackend : public IAsrBackend {
public:
    explicit FakeBackend(bool loadOk) : m_loadOk(loadOk) {}
    bool load(const QString&, QString* error) override
    {
        if (!m_loadOk) {
            if (error != nullptr) {
                *error = QStringLiteral("fake load failure");
            }
            return false;
        }
        m_loaded = true;
        return true;
    }
    bool isLoaded() const override { return m_loaded; }
    AsrTranscript transcribe(const std::vector<float>& pcm, QString*) override
    {
        if (pcm.empty()) {
            return {};
        }
        return AsrTranscript{QStringLiteral("OVER"), 0.9f};
    }
    void unload() override { m_loaded = false; }

private:
    bool m_loadOk;
    bool m_loaded = false;
};

AsrBackendFactory factory(bool loadOk)
{
    return [loadOk] { return std::unique_ptr<IAsrBackend>(new FakeBackend(loadOk)); };
}

// A scripted line equal to this reports a decode FAILURE instead of text — the
// error out-param path, which is distinct from an empty decode (RFC #4821).
const QString kScriptedFailure = QStringLiteral("!fail");

// Returns a scripted phrase per successive transcribe() call (the last phrase
// repeats once the script runs out). Lets the segment-overlap de-dup test drive
// deterministic boundary words across consecutive segments (RFC #4821).
class ScriptedBackend : public IAsrBackend {
public:
    explicit ScriptedBackend(std::vector<QString> lines) : m_lines(std::move(lines)) {}
    bool load(const QString&, QString*) override
    {
        m_loaded = true;
        return true;
    }
    bool isLoaded() const override { return m_loaded; }
    AsrTranscript transcribe(const std::vector<float>& pcm, QString* error) override
    {
        if (pcm.empty() || m_lines.empty()) {
            return {};
        }
        const QString text = m_next < m_lines.size() ? m_lines[m_next] : m_lines.back();
        ++m_next;
        if (text == kScriptedFailure) {
            if (error != nullptr) {
                *error = QStringLiteral("scripted decode failure");
            }
            return {};
        }
        return AsrTranscript{text, 0.9f};
    }
    void unload() override { m_loaded = false; }

private:
    std::vector<QString> m_lines;
    std::size_t m_next = 0;
    bool m_loaded = false;
};

AsrBackendFactory scriptedFactory(std::vector<QString> lines)
{
    return [lines] { return std::unique_ptr<IAsrBackend>(new ScriptedBackend(lines)); };
}

// Backend whose transcribe() blocks for a fixed delay — stands in for a real
// whisper decode or remote HTTP round-trip, so tests can build an actual
// backlog of queued (not-yet-dequeued) processAudio() calls and verify
// shutdown/disable don't transcribe all of it (see AsrEngine::~AsrEngine and
// AsrEngine::setEnabled).
class SlowBackend : public IAsrBackend {
public:
    explicit SlowBackend(int delayMs) : m_delayMs(delayMs) {}
    bool load(const QString&, QString*) override
    {
        m_loaded = true;
        return true;
    }
    bool isLoaded() const override { return m_loaded; }
    AsrTranscript transcribe(const std::vector<float>& pcm, QString*) override
    {
        QThread::msleep(m_delayMs);
        if (pcm.empty()) {
            return {};
        }
        return AsrTranscript{QStringLiteral("OVER"), 0.9f};
    }
    void unload() override { m_loaded = false; }

private:
    int m_delayMs;
    bool m_loaded = false;
};

AsrBackendFactory slowFactory(int delayMs)
{
    return [delayMs] { return std::unique_ptr<IAsrBackend>(new SlowBackend(delayMs)); };
}

// Deterministic stand-in for building the ~24 MB ECAPA session: the sleep is
// what the caller must not wait on. `succeed` picks which completion branch of
// AsrWorker::loadSpeakerModel() runs. The success branch hands back a default-
// constructed SpeakerEmbedder — enough to prove the embedder is installed, the
// clusterer reset and speakerModelLoaded(path, true) emitted, but NOT that a
// label comes out the other end: embed() needs a real ONNX model for that, and
// SpeakerEmbedder is concrete (no seam to fake one). Label emission stays
// covered by the ONNX-gated fixtures, not here.
AsrSpeakerEmbedderFactory speakerFactory(bool succeed, int delayMs)
{
    return [succeed, delayMs](const QString&) {
        QThread::msleep(delayMs);
        return succeed ? std::make_unique<SpeakerEmbedder>() : std::unique_ptr<SpeakerEmbedder>{};
    };
}

// Generate at the real RX pipeline rate (24 kHz) so the engine must resample
// 24k -> 16k on its worker before segmenting.
constexpr int kSrcRate = 24000;

QVector<float> tone(int ms, float amp = 0.3f)
{
    QVector<float> v;
    const int n = ms * kSrcRate / 1000;
    for (int i = 0; i < n; ++i) {
        v.push_back(amp * static_cast<float>(std::sin(2.0 * M_PI * 440.0 * i / kSrcRate)));
    }
    return v;
}

QVector<float> silence(int ms)
{
    return QVector<float>(ms * kSrcRate / 1000, 0.0f);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- Engine replacement discards only per-engine speaker state --------
    {
        SpeakerLoadLifecycle lifecycle;
        lifecycle.begin(QStringLiteral("/old-engine.onnx"));
        expect(lifecycle.isPending(QStringLiteral("/old-engine.onnx")),
               "matching speaker load is recognized as already pending");
        expect(!lifecycle.isPending(QStringLiteral("/different-model.onnx")),
               "a different speaker model is not deduplicated");
        lifecycle.begin(QStringLiteral("/custom-speaker.onnx"));
        expect(lifecycle.isPending(QStringLiteral("/custom-speaker.onnx")),
               "custom speaker off/on recognizes the in-flight path for deduplication");
        lifecycle.resetForEngineReplacement();
        expect(!lifecycle.isPending(), "engine replacement clears pending speaker load");
        expect(!lifecycle.isLoaded(QStringLiteral("/old-engine.onnx")),
               "engine replacement does not carry a stale loaded model");

        lifecycle.begin(QStringLiteral("/replayed-intent.onnx"));
        expect(!lifecycle.complete(QStringLiteral("/old-engine.onnx"), true),
               "late old-engine completion cannot settle replayed intent");
        expect(lifecycle.isPending(), "late old-engine completion leaves replay pending");
        expect(lifecycle.complete(QStringLiteral("/replayed-intent.onnx"), true),
               "replayed speaker intent settles on the replacement engine");
        expect(lifecycle.isLoaded(QStringLiteral("/replayed-intent.onnx")),
               "replacement engine caches its own loaded speaker model");

        lifecycle.begin(QStringLiteral("/failed-replacement.onnx"));
        expect(lifecycle.complete(QStringLiteral("/failed-replacement.onnx"), false),
               "failed replacement settles its pending load");
        expect(!lifecycle.isLoaded(QStringLiteral("/replayed-intent.onnx")),
               "failed replacement clears stale cached speaker state");
    }

    // ---- Async load emits ready() -----------------------------------------
    {
        AsrEngine engine(factory(true));
        QSignalSpy readySpy(&engine, &AsrEngine::ready);
        QSignalSpy failSpy(&engine, &AsrEngine::loadFailed);
        engine.setModelPath(QStringLiteral("/does/not/matter"));
        expect(readySpy.wait(5000), "setModelPath -> ready() emitted");
        expect(failSpy.isEmpty(), "no loadFailed on a good backend");
        expect(engine.isReady(), "engine reports ready");

        // ---- Audio -> segment -> transcribe -> finalText -------------------
        QSignalSpy textSpy(&engine, &AsrEngine::finalText);
        engine.setEnabled(true);
        engine.pushAudio(silence(150), kSrcRate);
        engine.pushAudio(tone(500), kSrcRate);
        engine.pushAudio(silence(400), kSrcRate); // trailing silence closes the utterance
        expect(textSpy.wait(5000), "utterance transcribed -> finalText() emitted");
        if (!textSpy.isEmpty()) {
            const auto args = textSpy.first();
            expect(args.at(0).toString() == QStringLiteral("OVER"),
                   "finalText carries the backend's transcription");
            expect(std::abs(args.at(1).toFloat() - 0.9f) < 1e-4f,
                   "finalText carries the backend's confidence");
        }

        // ---- Disabled engine ignores audio --------------------------------
        engine.reset();
        engine.setEnabled(false);
        const int before = textSpy.count();
        engine.pushAudio(tone(500), kSrcRate);
        engine.pushAudio(silence(400), kSrcRate);
        QSignalSpy idle(&engine, &AsrEngine::finalText);
        idle.wait(400); // give the worker a chance; expect nothing
        expect(textSpy.count() == before, "disabled engine emits no finalText");
    }

    // ---- Segment overlap: boundary-word de-dup (RFC #4821) -----------------
    // A small decode-buffer cap force-closes continuous speech into consecutive
    // segments; with overlap on, each continuation re-transcribes the previous
    // segment's tail word, which the worker must strip. The scripted backend
    // makes that duplicated boundary word deterministic (…charlie | charlie…).
    {
        const std::vector<QString> lines = {
            QStringLiteral("alpha bravo charlie"),
            QStringLiteral("charlie delta echo"),
            QStringLiteral("echo foxtrot golf"),
            QStringLiteral("golf hotel india"),
            QStringLiteral("india juliet kilo"),
            QStringLiteral("kilo lima mike"),
        };
        AsrEngine engine(scriptedFactory(lines));
        QSignalSpy readySpy(&engine, &AsrEngine::ready);
        engine.setModelPath(QStringLiteral("/does/not/matter"));
        expect(readySpy.wait(5000), "overlap dedup: engine ready");

        engine.setEnabled(true);
        engine.setDecodeBufferMs(300); // small cap → continuous speech force-closes
        engine.setOverlapMs(100);      // carry 100 ms across the cut

        QSignalSpy textSpy(&engine, &AsrEngine::finalText);
        engine.pushAudio(tone(1000), kSrcRate);   // continuous → repeated cap closes
        engine.pushAudio(silence(400), kSrcRate);  // hangover closes the tail
        expect(textSpy.wait(5000), "overlap dedup: first finalText emitted");
        while (textSpy.wait(500)) {
            // drain the remaining queued segment transcriptions
        }
        expect(textSpy.count() >= 2, "overlap dedup: continuous speech split into >=2 segments");
        if (textSpy.count() >= 2) {
            expect(textSpy.at(0).at(0).toString() == QStringLiteral("alpha bravo charlie"),
                   "overlap dedup: first segment is emitted whole");
            expect(textSpy.at(1).at(0).toString() == QStringLiteral("delta echo"),
                   "overlap dedup: continuation drops the duplicated boundary word");
        }
    }

    // ---- Segment overlap: an empty continuation clears the de-dup tail --------
    // If a continuation decodes empty (marginal SNR), the tail it would have been
    // compared against is gone; the NEXT continuation must de-dup against *that*
    // (now empty) reference, not the segment before it — otherwise a leading word
    // that coincidentally matches the older tail is wrongly stripped. Here the
    // 2nd decode is empty and the 3rd begins with "charlie", which also ends the
    // 1st: it must survive, not be de-dup'd away.
    {
        const std::vector<QString> lines = {
            QStringLiteral("alpha bravo charlie"),
            QString(),                              // empty decode (skipped, clears tail)
            QStringLiteral("charlie hotel india"),  // leading "charlie" must NOT be stripped
            QStringLiteral("india juliet kilo"),
            QStringLiteral("kilo lima mike"),
        };
        AsrEngine engine(scriptedFactory(lines));
        QSignalSpy readySpy(&engine, &AsrEngine::ready);
        engine.setModelPath(QStringLiteral("/does/not/matter"));
        expect(readySpy.wait(5000), "overlap empty-continuation: engine ready");

        engine.setEnabled(true);
        engine.setDecodeBufferMs(300);
        engine.setOverlapMs(100);

        QSignalSpy textSpy(&engine, &AsrEngine::finalText);
        engine.pushAudio(tone(1000), kSrcRate);
        engine.pushAudio(silence(400), kSrcRate);
        expect(textSpy.wait(5000), "overlap empty-continuation: first finalText emitted");
        while (textSpy.wait(500)) {
            // drain
        }
        expect(textSpy.count() >= 2, "overlap empty-continuation: >=2 non-empty emissions");
        if (textSpy.count() >= 2) {
            expect(textSpy.at(0).at(0).toString() == QStringLiteral("alpha bravo charlie"),
                   "overlap empty-continuation: first segment whole");
            expect(textSpy.at(1).at(0).toString() == QStringLiteral("charlie hotel india"),
                   "overlap empty-continuation: word after an empty decode is not stripped");
        }
    }

    // ---- Segment overlap: a FAILED continuation clears the de-dup tail ------
    // The error out-param path, not the empty-text one: a backend that reports a
    // decode failure also leaves no tail, so the reference must be dropped for
    // exactly the same reason. Without the clear, the 3rd segment would de-dup
    // against the 1st — two segments back — and lose a genuine leading word.
    // Mutation check: delete the m_prevSegmentText.clear() on the error path and
    // this case emits "hotel india" instead.
    {
        const std::vector<QString> lines = {
            QStringLiteral("alpha bravo charlie"),
            kScriptedFailure,                       // decode error (clears the tail)
            QStringLiteral("charlie hotel india"),  // leading "charlie" must NOT be stripped
            QStringLiteral("india juliet kilo"),
            QStringLiteral("kilo lima mike"),
        };
        AsrEngine engine(scriptedFactory(lines));
        QSignalSpy readySpy(&engine, &AsrEngine::ready);
        engine.setModelPath(QStringLiteral("/does/not/matter"));
        expect(readySpy.wait(5000), "overlap failed-continuation: engine ready");

        engine.setEnabled(true);
        engine.setDecodeBufferMs(300);
        engine.setOverlapMs(100);

        QSignalSpy errSpy(&engine, &AsrEngine::error);
        QSignalSpy textSpy(&engine, &AsrEngine::finalText);
        engine.pushAudio(tone(1000), kSrcRate);
        engine.pushAudio(silence(400), kSrcRate);
        expect(textSpy.wait(5000), "overlap failed-continuation: first finalText emitted");
        while (textSpy.wait(500)) {
            // drain
        }
        expect(errSpy.count() >= 1, "overlap failed-continuation: the decode failure is reported");
        expect(textSpy.count() >= 2, "overlap failed-continuation: >=2 non-empty emissions");
        if (textSpy.count() >= 2) {
            expect(textSpy.at(0).at(0).toString() == QStringLiteral("alpha bravo charlie"),
                   "overlap failed-continuation: first segment whole");
            expect(textSpy.at(1).at(0).toString() == QStringLiteral("charlie hotel india"),
                   "overlap failed-continuation: word after a failed decode is not stripped");
        }
    }

    // ---- Segment overlap: a punctuation-only token is not a boundary match ---
    // normWord() strips edge punctuation, so a token that is ALL punctuation
    // normalizes to "". Two such tokens must not compare equal — "" == "" is not
    // a repeated word, and treating it as one silently eats the continuation's
    // real first token. Mutation check: drop the isEmpty() guards from the
    // comparison and this case emits "delta echo".
    {
        const std::vector<QString> lines = {
            QStringLiteral("alpha bravo ..."),
            QStringLiteral("... delta echo"), // the "..." must survive, not match
            QStringLiteral("echo foxtrot golf"),
            QStringLiteral("golf hotel india"),
        };
        AsrEngine engine(scriptedFactory(lines));
        QSignalSpy readySpy(&engine, &AsrEngine::ready);
        engine.setModelPath(QStringLiteral("/does/not/matter"));
        expect(readySpy.wait(5000), "overlap punctuation: engine ready");

        engine.setEnabled(true);
        engine.setDecodeBufferMs(300);
        engine.setOverlapMs(100);

        QSignalSpy textSpy(&engine, &AsrEngine::finalText);
        engine.pushAudio(tone(1000), kSrcRate);
        engine.pushAudio(silence(400), kSrcRate);
        expect(textSpy.wait(5000), "overlap punctuation: first finalText emitted");
        while (textSpy.wait(500)) {
            // drain
        }
        expect(textSpy.count() >= 2, "overlap punctuation: >=2 emissions");
        if (textSpy.count() >= 2) {
            expect(textSpy.at(1).at(0).toString() == QStringLiteral("... delta echo"),
                   "overlap punctuation: a punctuation-only token is not a boundary match");
        }
    }

    // ---- Segment overlap: the strip is bounded by the carried window ---------
    // The de-dup must not strip the longest match it can find — only about as
    // many words as the carried audio could actually hold (overlapWordBudget).
    // Here the boundary is a genuine triple repeat ("one one one" | "one one one
    // two") but only 100 ms was carried, so exactly ONE word may go; the other
    // two are real speech the operator said. Mutation check: remove
    // overlapWordBudget from the cap (or pass a live/large overlapMs instead of
    // the value captured at close time) and this case emits "two".
    {
        const std::vector<QString> lines = {
            QStringLiteral("bravo one one one"),
            QStringLiteral("one one one two"), // only the first "one" is overlap
            QStringLiteral("two three four"),
            QStringLiteral("four five six"),
        };
        AsrEngine engine(scriptedFactory(lines));
        QSignalSpy readySpy(&engine, &AsrEngine::ready);
        engine.setModelPath(QStringLiteral("/does/not/matter"));
        expect(readySpy.wait(5000), "overlap budget: engine ready");

        engine.setEnabled(true);
        engine.setDecodeBufferMs(300);
        engine.setOverlapMs(100); // 100 ms -> a one-word budget

        QSignalSpy textSpy(&engine, &AsrEngine::finalText);
        engine.pushAudio(tone(1000), kSrcRate);
        engine.pushAudio(silence(400), kSrcRate);
        expect(textSpy.wait(5000), "overlap budget: first finalText emitted");
        while (textSpy.wait(500)) {
            // drain
        }
        expect(textSpy.count() >= 2, "overlap budget: >=2 emissions");
        if (textSpy.count() >= 2) {
            expect(textSpy.at(1).at(0).toString() == QStringLiteral("one one two"),
                   "overlap budget: the strip is bounded by the carried window, "
                   "not the longest match");
        }
    }

    // ---- Destructor returns promptly with a queued backlog ----------------
    // Note: Qt's own QThread::quit() already stops the worker from starting
    // any FURTHER queued processAudio() calls once the current one returns —
    // confirmed by temporarily disabling AsrWorker's cancel check, which still
    // left this passing (only the one already-in-flight call ran). This test
    // is a regression guard on that overall property (whichever mechanism
    // provides it) — shutdown must be bounded by ~one in-flight call, not by
    // however many segments happen to be backlogged.
    {
        constexpr int kDelayMs = 300;      // stand-in for a slow transcribe()
        constexpr int kBacklogCount = 5;   // would need ~1500 ms if all ran
        auto* engine = new AsrEngine(slowFactory(kDelayMs));
        QSignalSpy readySpy(engine, &AsrEngine::ready);
        engine->setModelPath(QStringLiteral("/does/not/matter"));
        expect(readySpy.wait(5000), "shutdown test: engine ready");
        engine->setEnabled(true);

        // Queue several distinct utterances rapidly so multiple requestProcess
        // calls back up behind the (slow) one currently being transcribed.
        for (int i = 0; i < kBacklogCount; ++i) {
            engine->pushAudio(tone(500), kSrcRate);
            engine->pushAudio(silence(400), kSrcRate);
        }
        QThread::msleep(50); // let the worker start on the first item

        QElapsedTimer timer;
        timer.start();
        delete engine; // must not block until the whole backlog finishes
        expect(timer.elapsed() < kDelayMs * 2,
               "destructor returns promptly despite a queued backlog");
    }

    // ---- Speaker preparation stays off the caller thread ------------------
    {
        constexpr int kDelayMs = 300;
        AsrEngine engine(factory(true), AsrSegmenter::Config{}, nullptr,
                         speakerFactory(false, kDelayMs));
        QSignalSpy readySpy(&engine, &AsrEngine::ready);
        engine.setModelPath(QStringLiteral("/does/not/matter"));
        expect(readySpy.wait(5000), "speaker-load test: engine ready");

        QSignalSpy speakerSpy(&engine, &AsrEngine::speakerModelLoaded);
        QElapsedTimer timer;
        timer.start();
        engine.setSpeakerModelPath(QStringLiteral("/slow-speaker.onnx"));
        // Half the loader delay, not a small absolute bound: the property is
        // "did not wait for the load", and a loaded runner must make this test
        // slower rather than red. The blocking (pre-fix) shape costs >= kDelayMs.
        expect(timer.elapsed() < kDelayMs / 2,
               "setSpeakerModelPath returns without waiting for speaker preparation");

        engine.setSpeakerLabelingEnabled(true);
        engine.setSpeakerLabelingEnabled(false); // latest intent wins behind the slow load
        expect(!engine.isSpeakerLabelingEnabled(),
               "speaker labeling disable takes effect immediately on the caller thread");

        bool eventLoopTicked = false;
        QTimer::singleShot(30, &app, [&eventLoopTicked] { eventLoopTicked = true; });
        expect(speakerSpy.wait(5000), "slow speaker preparation completes asynchronously");
        expect(eventLoopTicked,
               "main event loop remains responsive while speaker preparation is in flight");
        if (!speakerSpy.isEmpty()) {
            const QList<QVariant> args = speakerSpy.first();
            expect(args.at(0).toString() == QStringLiteral("/slow-speaker.onnx"),
                   "speaker completion identifies the requested model path");
            expect(!args.at(1).toBool(), "failed injected speaker loader is reported");
        }

        speakerSpy.clear();
        engine.setSpeakerLabelingEnabled(true);
        engine.setSpeakerModelPath(QStringLiteral("/failed-replacement.onnx"));
        expect(speakerSpy.wait(5000), "failed speaker replacement completes");
        // The flag is operator intent and nothing but setSpeakerLabelingEnabled()
        // writes it, so a failed load leaves it alone; the worker dropping its
        // embedder is what stops labels. Owning that distinction is what lets the
        // controller decide whether to un-check the box.
        expect(engine.isSpeakerLabelingEnabled(),
               "a failed speaker load does not silently rewrite the operator's intent");
    }

    // ---- A successful speaker load arms labeling and keeps intent ---------
    {
        constexpr int kDelayMs = 50;
        AsrEngine engine(factory(true), AsrSegmenter::Config{}, nullptr,
                         speakerFactory(true, kDelayMs));
        QSignalSpy readySpy(&engine, &AsrEngine::ready);
        engine.setModelPath(QStringLiteral("/does/not/matter"));
        expect(readySpy.wait(5000), "speaker-success test: engine ready");

        QSignalSpy speakerSpy(&engine, &AsrEngine::speakerModelLoaded);
        engine.setSpeakerLabelingEnabled(true);
        engine.setSpeakerModelPath(QStringLiteral("/good-speaker.onnx"));
        expect(speakerSpy.wait(5000), "successful speaker preparation completes");
        if (!speakerSpy.isEmpty()) {
            const QList<QVariant> args = speakerSpy.first();
            expect(args.at(0).toString() == QStringLiteral("/good-speaker.onnx"),
                   "successful completion identifies the requested model path");
            expect(args.at(1).toBool(), "successful speaker load is reported as loaded");
        }
        expect(engine.isSpeakerLabelingEnabled(),
               "a successful speaker load leaves labeling intent on");

        // Replacing a good model with another good one must not disturb intent
        // either — the cached embedder is swapped, not the operator's choice.
        speakerSpy.clear();
        engine.setSpeakerModelPath(QStringLiteral("/second-speaker.onnx"));
        expect(speakerSpy.wait(5000), "speaker model replacement completes");
        expect(engine.isSpeakerLabelingEnabled(),
               "replacing a loaded speaker model keeps labeling intent on");

        // Toggling off keeps the loaded embedder (that is the whole point of
        // #4737's fix) — only the intent flips, with no engine teardown.
        engine.setSpeakerLabelingEnabled(false);
        expect(!engine.isSpeakerLabelingEnabled(),
               "toggling labeling off flips intent without a reload");
    }

    // ---- Shutdown skips speaker loads that have not started ---------------
    // A backend/GPU/language change destroys the engine, and ~AsrEngine() joins
    // the worker. A speaker session build cannot be aborted once running, so the
    // bound that matters is "one in-flight stage", not "every queued stage".
    {
        constexpr int kDelayMs = 300;
        auto* engine = new AsrEngine(factory(true), AsrSegmenter::Config{}, nullptr,
                                     speakerFactory(false, kDelayMs));
        QSignalSpy readySpy(engine, &AsrEngine::ready);
        engine->setModelPath(QStringLiteral("/does/not/matter"));
        expect(readySpy.wait(5000), "speaker-shutdown test: engine ready");

        engine->setSpeakerModelPath(QStringLiteral("/first.onnx"));
        engine->setSpeakerModelPath(QStringLiteral("/second.onnx"));
        engine->setSpeakerModelPath(QStringLiteral("/third.onnx"));
        QThread::msleep(50); // let the worker start on the first

        QElapsedTimer timer;
        timer.start();
        delete engine; // must wait out one load, not all three (~900 ms)
        expect(timer.elapsed() < kDelayMs * 2,
               "destructor skips speaker loads that have not started");
    }

    // ---- Disabling drops a queued backlog instead of transcribing it ------
    {
        constexpr int kDelayMs = 300;
        constexpr int kBacklogCount = 5;
        AsrEngine engine(slowFactory(kDelayMs));
        QSignalSpy readySpy(&engine, &AsrEngine::ready);
        engine.setModelPath(QStringLiteral("/does/not/matter"));
        expect(readySpy.wait(5000), "disable test: engine ready");
        engine.setEnabled(true);

        QSignalSpy textSpy(&engine, &AsrEngine::finalText);
        for (int i = 0; i < kBacklogCount; ++i) {
            engine.pushAudio(tone(500), kSrcRate);
            engine.pushAudio(silence(400), kSrcRate);
        }
        QThread::msleep(50); // let the worker start on the first item
        engine.setEnabled(false); // should drop everything still queued

        // Long enough that, without the fix, most/all of the backlog would
        // have finished transcribing by now.
        QThread::msleep(kBacklogCount * kDelayMs + 200);
        QCoreApplication::processEvents(); // deliver any queued finalText

        // At most one item (whichever was already in-flight when disabled)
        // may complete — the rest of the backlog must be dropped.
        expect(textSpy.count() <= 1,
               "disabling drops the backlog instead of transcribing all of it");
    }

    // ---- Load failure surfaces loadFailed(), not ready() ------------------
    {
        AsrEngine engine(factory(false));
        QSignalSpy readySpy(&engine, &AsrEngine::ready);
        QSignalSpy failSpy(&engine, &AsrEngine::loadFailed);
        engine.setModelPath(QStringLiteral("/bad"));
        expect(failSpy.wait(5000), "failing backend -> loadFailed() emitted");
        expect(readySpy.isEmpty(), "no ready() on load failure");
        expect(!engine.isReady(), "engine not ready after load failure");
    }

    std::printf(g_failures == 0 ? "\nASR engine: ALL PASS\n"
                                : "\nASR engine: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
