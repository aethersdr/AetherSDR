// AsrEngine live-preview plumbing with a fake recogniser: the preview runs on
// its own thread, is fed the same 16 kHz timeline the segmenter's edges are
// reported on, is finalised at every segment close, and neither its audio nor
// its results wait behind a decode. The fake fires results from its own
// thread, as the Apple recogniser does.

#include "asr/AsrEngine.h"
#include "asr/IAsrBackend.h"
#include "asr/LivePreview.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QVector>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace AetherSDR {
// The worker skips chunks the engine already counted (cancelled or stale
// input); this makes it skip one on demand.
struct AsrEngineTestAccess {
    static void setWorkerCancel(AsrEngine& engine, bool cancel)
    {
        engine.m_worker->setCancelPending(cancel);
    }
    static bool awaitWorker(AsrEngine& engine)
    {
        auto done = std::make_shared<QSemaphore>();
        QMetaObject::invokeMethod(engine.m_worker, [done] { done->release(); },
                                  Qt::QueuedConnection);
        return done->tryAcquire(1, 5000);
    }
};
} // namespace AetherSDR

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

constexpr int kRate = 16000; // the ASR rate, so no resampling and exact positions

QVector<float> tone(int ms, int rate = kRate)
{
    QVector<float> v(ms * rate / 1000);
    for (int i = 0; i < v.size(); ++i) {
        v[i] = 0.3f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * i / rate));
    }
    return v;
}

QVector<float> silence(int ms, int rate = kRate)
{
    return QVector<float>(ms * rate / 1000, 0.0f);
}

// What the worker did to the fake, plus every session's callbacks.
struct FakeLog {
    std::mutex mu;
    std::vector<std::pair<qint64, int>> feeds;     // (startSample, count)
    std::vector<qint64> finalizes;
    std::vector<LivePreviewCallbacks> sessions;    // one per created session
    int destroyed = 0;
};

class FakePreview : public ILivePreview {
public:
    explicit FakePreview(std::shared_ptr<FakeLog> log) : m_log(std::move(log)) {}
    ~FakePreview() override
    {
        const std::lock_guard<std::mutex> lock(m_log->mu);
        ++m_log->destroyed;
    }
    void feed(const float*, int count, qint64 startSample) override
    {
        const std::lock_guard<std::mutex> lock(m_log->mu);
        m_log->feeds.emplace_back(startSample, count);
    }
    void finalizeThrough(qint64 sample) override
    {
        const std::lock_guard<std::mutex> lock(m_log->mu);
        m_log->finalizes.push_back(sample);
    }

private:
    std::shared_ptr<FakeLog> m_log;
};

// Segment edges reach the preview through the engine's thread, so they can
// land just after the final text does.
bool awaitFinalize(FakeLog& log);

qint64 fedEnd(FakeLog& log)
{
    const std::lock_guard<std::mutex> lock(log.mu);
    return log.feeds.empty() ? 0 : log.feeds.back().first + log.feeds.back().second;
}

LivePreviewFactory fakeFactory(const std::shared_ptr<FakeLog>& log)
{
    return [log](LivePreviewCallbacks callbacks) -> std::unique_ptr<ILivePreview> {
        {
            const std::lock_guard<std::mutex> lock(log->mu);
            log->sessions.push_back(callbacks);
        }
        return std::make_unique<FakePreview>(log);
    };
}

// Fires a result from a separate thread, like the real recogniser.
void fireFromThread(const LivePreviewCallbacks& cb, qint64 start, qint64 end, const char* text,
                    bool isFinal)
{
    std::thread([cb, start, end, text, isFinal] {
        cb.onResult({start, end, QString::fromUtf8(text), isFinal});
    }).join();
}

class Backend : public IAsrBackend {
public:
    Backend(int delayMs, QString text) : m_delayMs(delayMs), m_text(std::move(text)) {}
    bool load(const QString&, QString*) override { return m_loaded = true; }
    bool isLoaded() const override { return m_loaded; }
    AsrTranscript transcribe(const std::vector<float>&, QString*) override
    {
        QThread::msleep(static_cast<unsigned long>(m_delayMs));
        return {m_text, 0.9f};
    }
    void unload() override { m_loaded = false; }

private:
    int m_delayMs;
    QString m_text;
    bool m_loaded = false;
};

AsrBackendFactory backend(int delayMs, const char* text)
{
    return [delayMs, text] { return std::make_unique<Backend>(delayMs, QString::fromUtf8(text)); };
}

// Pump events until `pred` holds or the timeout passes.
template <typename Pred>
bool waitFor(Pred pred, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return pred();
}

bool awaitFinalize(FakeLog& log)
{
    return waitFor([&] {
        const std::lock_guard<std::mutex> lock(log.mu);
        return !log.finalizes.empty();
    }, 3000);
}

bool readyEngine(AsrEngine& engine)
{
    QSignalSpy ready(&engine, &AsrEngine::ready);
    engine.setModelPath(QStringLiteral("fake"));
    if (!ready.wait(5000)) {
        return false;
    }
    engine.setEnabled(true);
    return true;
}

// One utterance: 150 ms silence, 500 ms tone, 400 ms silence (closes it).
void pushUtterance(AsrEngine& engine)
{
    engine.pushAudio(silence(150), kRate);
    engine.pushAudio(tone(500), kRate);
    engine.pushAudio(silence(400), kRate);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- Timeline and finalisation --------------------------------------
    {
        auto log = std::make_shared<FakeLog>();
        AsrEngine engine(backend(0, "OVER"));
        expect(readyEngine(engine), "engine ready");
        engine.setLivePreviewFactory(fakeFactory(log));
        QSignalSpy finals(&engine, &AsrEngine::finalText);
        pushUtterance(engine);
        expect(finals.wait(5000), "the utterance is decoded");
        awaitFinalize(*log);

        const std::lock_guard<std::mutex> lock(log->mu);
        bool contiguous = !log->feeds.empty() && log->feeds.front().first == 0;
        for (size_t i = 1; i < log->feeds.size(); ++i) {
            contiguous = contiguous
                && log->feeds[i].first == log->feeds[i - 1].first + log->feeds[i - 1].second;
        }
        expect(contiguous, "the preview is fed one contiguous timeline starting at sample 0");
        expect(log->sessions.size() == 1, "one session serves the whole stream");
        expect(!log->finalizes.empty() && log->finalizes.front() > 0
                   && log->finalizes.front() <= (150 + 500 + 400) * kRate / 1000,
               "the preview is finalised at the segment's close");
        expect(!finals.isEmpty() && finals.first().at(3).toULongLong() == 0,
               "finalText names the segment it decodes (id 0)");
    }

    // ---- Results arrive while the worker is blocked in a decode ----------
    {
        auto log = std::make_shared<FakeLog>();
        AsrEngine engine(backend(1500, "SLOW"));
        expect(readyEngine(engine), "slow engine ready");
        engine.setLivePreviewFactory(fakeFactory(log));
        QSignalSpy previews(&engine, &AsrEngine::previewText);
        QSignalSpy finals(&engine, &AsrEngine::finalText);
        pushUtterance(engine);
        const bool closed = waitFor([&] {
            const std::lock_guard<std::mutex> lock(log->mu);
            return !log->finalizes.empty();
        }, 3000);
        expect(closed, "the segment closed and its 1.5 s decode began");

        LivePreviewCallbacks cb;
        qint64 edge = 0;
        {
            const std::lock_guard<std::mutex> lock(log->mu);
            if (!log->sessions.empty()) {
                cb = log->sessions.back();
            }
            edge = log->finalizes.empty() ? 0 : log->finalizes.front();
        }
        if (!cb.onResult) {
            expect(false, "a preview session exists to fire from");
            return 1;
        }
        QElapsedTimer t;
        t.start();
        fireFromThread(cb, edge, edge + 8000, "next over", false);
        expect(previews.wait(600), "preview text arrives during the decode, not after it");
        expect(t.elapsed() < 600 && finals.isEmpty(),
               "it arrived before the decode finished");

        // The audio side of the same promise: what arrives during the decode
        // reaches the recogniser now, not in a burst once the decode ends
        // (the gray text would otherwise stall at every segment close).
        const qint64 before = fedEnd(*log);
        engine.pushAudio(tone(200), kRate);
        const bool fedDuring =
            waitFor([&] { return fedEnd(*log) >= before + 200 * kRate / 1000; }, 400);
        expect(fedDuring && finals.isEmpty(),
               "audio pushed during the decode reaches the preview before the decode ends");
        if (!previews.isEmpty()) {
            expect(previews.last().at(0).toULongLong() == 1
                       && previews.last().at(1).toString() == QStringLiteral("next over"),
                   "text after the close goes to the next segment's block");
        }
        expect(finals.wait(5000), "the slow decode still completes");
    }

    // ---- Resolution, discard, stale results -------------------------------
    {
        auto log = std::make_shared<FakeLog>();
        AsrEngine engine(backend(0, ""));
        expect(readyEngine(engine), "empty-text engine ready");
        engine.setLivePreviewFactory(fakeFactory(log));
        QSignalSpy previews(&engine, &AsrEngine::previewText);
        QSignalSpy discarded(&engine, &AsrEngine::previewDiscarded);
        engine.pushAudio(silence(150), kRate);
        waitFor([&] {
            const std::lock_guard<std::mutex> lock(log->mu);
            return !log->sessions.empty();
        }, 3000);
        LivePreviewCallbacks first;
        {
            const std::lock_guard<std::mutex> lock(log->mu);
            first = log->sessions.back();
        }
        fireFromThread(first, 0, 1600, "maybe words", false);
        expect(previews.wait(2000) && previews.last().at(0).toULongLong() == 0,
               "provisional text before any close goes to block 0");
        engine.pushAudio(tone(500), kRate);
        engine.pushAudio(silence(400), kRate);
        expect(discarded.wait(5000) && discarded.first().at(0).toULongLong() == 0,
               "a segment that decodes to nothing discards its block");
        const int before = previews.count();
        fireFromThread(first, 0, 1600, "late words", true);
        waitFor([] { return false; }, 200);
        expect(previews.count() == before, "a late result for a resolved block is dropped");

        QSignalSpy restarted(&engine, &AsrEngine::livePreviewReset);
        engine.reset();
        expect(restarted.count() == 1, "reset() announces that segment ids restart");
        fireFromThread(first, 0, 1600, "old session", false);
        waitFor([] { return false; }, 200);
        expect(previews.count() == before, "a result from the session before reset() is dropped");
        engine.pushAudio(silence(150), kRate);
        const bool fresh = waitFor([&] {
            const std::lock_guard<std::mutex> lock(log->mu);
            return log->sessions.size() == 2 && log->destroyed == 1;
        }, 3000);
        expect(fresh, "reset() retires the session and the next audio starts a new one");
    }

    // ---- A burst too short to be speech ------------------------------------
    {
        auto log = std::make_shared<FakeLog>();
        AsrEngine engine(backend(0, "OVER"));
        expect(readyEngine(engine), "burst engine ready");
        engine.setLivePreviewFactory(fakeFactory(log));
        QSignalSpy previews(&engine, &AsrEngine::previewText);
        engine.pushAudio(silence(150), kRate);
        waitFor([&] {
            const std::lock_guard<std::mutex> lock(log->mu);
            return !log->sessions.empty();
        }, 3000);
        LivePreviewCallbacks cb;
        {
            const std::lock_guard<std::mutex> lock(log->mu);
            cb = log->sessions.back();
        }
        fireFromThread(cb, 150 * kRate / 1000, 250 * kRate / 1000, "static crash", false);
        expect(previews.wait(2000) && previews.last().at(1).toString() == QStringLiteral("static crash"),
               "the recogniser's text for a burst shows as preview");
        // 100 ms is under minSpeechMs (200): the segmenter discards it, so no
        // final will ever replace that text.
        engine.pushAudio(tone(100), kRate);
        engine.pushAudio(silence(400), kRate);
        const bool cleared = waitFor([&] {
            return previews.count() >= 2 && previews.last().at(0).toULongLong() == 0
                && previews.last().at(1).toString().isEmpty();
        }, 3000);
        expect(cleared, "a burst too short to be speech takes its preview text with it");
        {
            const std::lock_guard<std::mutex> lock(log->mu);
            expect(!log->finalizes.empty()
                       && log->finalizes.back() >= (150 + 100) * kRate / 1000,
                   "the recogniser is finalised where the burst was discarded");
        }
    }

    // ---- A source rate that needs resampling --------------------------------
    {
        constexpr int kNative = 24000;
        auto log = std::make_shared<FakeLog>();
        AsrEngine engine(backend(0, "OVER"));
        expect(readyEngine(engine), "24 kHz engine ready");
        engine.setLivePreviewFactory(fakeFactory(log));
        QSignalSpy finals(&engine, &AsrEngine::finalText);
        // In odd 101-sample chunks (67.33 samples each at 16 kHz), so the
        // engine's rounded positions and the resampler's per-chunk output
        // have to add up.
        constexpr int kChunk = 101;
        QVector<float> all;
        auto push = [&](const QVector<float>& v) { all += v; };
        push(silence(150, kNative));
        push(tone(500, kNative));
        push(silence(400, kNative));
        for (int off = 0; off < all.size(); off += kChunk) {
            engine.pushAudio(all.mid(off, kChunk), kNative);
        }
        expect(finals.wait(5000), "the 24 kHz utterance is decoded");
        awaitFinalize(*log);
        const std::lock_guard<std::mutex> lock(log->mu);
        bool contiguous = !log->feeds.empty() && log->feeds.front().first == 0;
        for (size_t i = 1; i < log->feeds.size(); ++i) {
            contiguous = contiguous
                && log->feeds[i].first == log->feeds[i - 1].first + log->feeds[i - 1].second;
        }
        expect(contiguous, "resampled audio still reaches the preview as one contiguous timeline");
        // The segment closes 300 ms (the hangover) after the tone ends. The
        // resampler delays its output by about 20 ms (measured: 320 samples);
        // the preview's resampler delays its audio alike, so both sides agree.
        const qint64 expected = (150 + 500 + 300 + 20) * kRate / 1000;
        expect(!log->finalizes.empty() && std::llabs(log->finalizes.front() - expected) <= 80,
               "the segment edge lands where the audio says, within 5 ms of the resampler delay");
    }

    // ---- The source rate changes mid-stream ---------------------------------
    {
        // The resampler is rebuilt and starts a fresh rounding phase, so its
        // output no longer lines up sample-for-sample with the engine's
        // positions. The feed must stay contiguous anyway: overlapping or
        // gapped buffer times are not something to hand the recogniser.
        auto log = std::make_shared<FakeLog>();
        AsrEngine engine(backend(0, "OVER"));
        expect(readyEngine(engine), "rate-change engine ready");
        engine.setLivePreviewFactory(fakeFactory(log));
        const QVector<float> a = silence(300, 24000);
        for (int off = 0; off < a.size(); off += 101) {
            engine.pushAudio(a.mid(off, 101), 24000);
        }
        const QVector<float> b = silence(300, 44100);
        for (int off = 0; off < b.size(); off += 101) {
            engine.pushAudio(b.mid(off, 101), 44100);
        }
        waitFor([&] { return fedEnd(*log) >= 590 * kRate / 1000; }, 3000);
        const std::lock_guard<std::mutex> lock(log->mu);
        bool contiguous = !log->feeds.empty();
        for (size_t i = 1; i < log->feeds.size(); ++i) {
            contiguous = contiguous
                && log->feeds[i].first == log->feeds[i - 1].first + log->feeds[i - 1].second;
        }
        expect(contiguous, "a mid-stream rate change keeps the preview's feed contiguous");
    }

    // ---- The worker skips a chunk the preview heard -------------------------
    {
        auto log = std::make_shared<FakeLog>();
        AsrEngine engine(backend(0, "OVER"));
        expect(readyEngine(engine), "skip engine ready");
        engine.setLivePreviewFactory(fakeFactory(log));
        QSignalSpy finals(&engine, &AsrEngine::finalText);
        AsrEngineTestAccess::setWorkerCancel(engine, true);
        engine.pushAudio(silence(700), kRate); // counted and heard, but not segmented
        expect(AsrEngineTestAccess::awaitWorker(engine), "the worker passed over the chunk");
        AsrEngineTestAccess::setWorkerCancel(engine, false);
        pushUtterance(engine);
        expect(finals.wait(5000), "the utterance after the skip is decoded");
        awaitFinalize(*log);
        // The segment closes 300 ms (the hangover) after the tone ends, and
        // the 700 ms the worker skipped still happened on the shared timeline.
        const qint64 expected = (700 + 150 + 500 + 300) * kRate / 1000;
        const std::lock_guard<std::mutex> lock(log->mu);
        expect(!log->finalizes.empty() && std::llabs(log->finalizes.front() - expected) <= 160,
               "segment edges stay on the preview's timeline when the worker skips audio");
    }

    // ---- Turning it off, and an unavailable recogniser ---------------------
    {
        auto log = std::make_shared<FakeLog>();
        AsrEngine engine(backend(0, "OVER"));
        expect(readyEngine(engine), "toggle engine ready");
        engine.setLivePreviewFactory(fakeFactory(log));
        engine.pushAudio(silence(100), kRate);
        waitFor([&] {
            const std::lock_guard<std::mutex> lock(log->mu);
            return !log->feeds.empty();
        }, 3000);
        engine.setLivePreviewFactory({});
        size_t fedBefore = 0;
        waitFor([&] {
            const std::lock_guard<std::mutex> lock(log->mu);
            return log->destroyed == 1;
        }, 3000);
        {
            const std::lock_guard<std::mutex> lock(log->mu);
            fedBefore = log->feeds.size();
        }
        pushUtterance(engine);
        waitFor([] { return false; }, 300);
        {
            const std::lock_guard<std::mutex> lock(log->mu);
            expect(log->destroyed == 1 && log->feeds.size() == fedBefore,
                   "an empty factory stops the session and feeds nothing more");
        }

        QSignalSpy status(&engine, &AsrEngine::livePreviewStatus);
        engine.setLivePreviewFactory([](LivePreviewCallbacks) { return nullptr; });
        pushUtterance(engine);
        expect(status.wait(3000)
                   && status.first().at(0).toInt() == static_cast<int>(LivePreviewState::Failed),
               "an unavailable recogniser reports Failed");
        pushUtterance(engine);
        waitFor([] { return false; }, 300);
        expect(status.count() == 1, "and reports it once, not on every chunk");
    }

    if (g_failures == 0) {
        std::printf("Live preview engine: ALL PASS\n");
        return 0;
    }
    std::printf("Live preview engine: %d FAILURE(S)\n", g_failures);
    return 1;
}
