// Offline unit test for AsrEngine (RFC #4333, Phase 3). Uses a deterministic
// fake backend injected via the factory, so the engine's worker-thread
// orchestration — async load, audio -> segment -> transcribe -> finalText,
// enabled-gating, and load-failure — is verified without any model or whisper.

#include "asr/AsrEngine.h"
#include "asr/IAsrBackend.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QThread>
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
