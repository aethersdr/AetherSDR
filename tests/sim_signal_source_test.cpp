// SimSignalSource — the demo radio's RX engine on a worker thread (#4878).
//
// The property under test is the fix itself: the generator runs OFF the
// thread that constructed it, keeps its 24 kHz long-run pacing there, and
// every control crosses the boundary queued. The GUI-thread original was the
// only RX producer in the app with GUI affinity, and on software-GL machines
// its 3 ms precise timer plus ~200 allocating emissions/s was a multi-second
// input backlog.

#include "core/backends/sim/NoiseMixer.h"
#include "core/backends/sim/SimSignalSource.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSet>
#include <QThread>

#include <cstdio>
#include <iostream>

using AetherSDR::NoiseMixer;
using AetherSDR::SimSignalSource;

namespace {

int g_failures = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

// Pump the main loop until `predicate` holds or the budget runs out.
bool waitFor(const std::function<bool()>& predicate, int budgetMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < budgetMs) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return predicate();
}

bool frameIsSilent(const QByteArray& stereo)
{
    const auto* f = reinterpret_cast<const float*>(stereo.constData());
    const int n = stereo.size() / int(sizeof(float));
    for (int i = 0; i < n; ++i) {
        if (f[i] != 0.0f) {
            return false;
        }
    }
    return true;
}

// QSignalSpy connects DIRECT, so a cross-thread emitter appends to the spy's
// storage from the worker while the main thread reads it — a data race that
// segfaulted this test's first draft. This collector lives on the main
// thread and receives QUEUED, so every count and payload is touched by one
// thread only.
class Collector : public QObject {
public:
    int audio = 0;
    int slice = 0;
    int spectrum = 0;
    int lastSliceId = -1;
    QSet<int> panIdsSeen;
    qint64 samples = 0;
    QByteArray lastAudio;

    explicit Collector(AetherSDR::SimSignalSource* src)
    {
        connect(src, &AetherSDR::SimSignalSource::audioFrameReady, this,
                [this](const QByteArray& b) {
                    ++audio;
                    samples += b.size() / qint64(2 * sizeof(float));
                    lastAudio = b;
                });
        connect(src, &AetherSDR::SimSignalSource::sliceAudioFrameReady, this,
                [this](int id, const QByteArray&) {
                    ++slice;
                    lastSliceId = id;
                });
        connect(src, &AetherSDR::SimSignalSource::spectrumFrameReady, this,
                [this](int pan, const QByteArray&) {
                    ++spectrum;
                    panIdsSeen.insert(pan);
                });
    }
};

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QThread worker;
    worker.setObjectName("SimSignalSourceTest");
    auto* src = new SimSignalSource;
    src->moveToThread(&worker);
    worker.start();

    report("the source lives on the worker, not the main thread",
           src->thread() == &worker && src->thread() != app.thread());

    Collector rx(src);

    // ── Start, and measure the long-run pacing ───────────────────────────
    QMetaObject::invokeMethod(src, &SimSignalSource::start, Qt::QueuedConnection);
    report("audio frames arrive across the thread boundary",
           waitFor([&] { return rx.audio > 4; }, 2000));

    // Count samples over a measured window: the debt pacing must hold the
    // long-run rate at 24 kHz regardless of the coarse 10 ms timer.
    rx.audio = 0;
    rx.samples = 0;
    QElapsedTimer window;
    window.start();
    waitFor([&] { return window.elapsed() >= 500; }, 1500);
    const qint64 elapsedMs = window.elapsed();
    const double rate = rx.samples * 1000.0 / elapsedMs;
    report("the long-run rate stays at 24 kHz on a coarse timer",
           rate > 24000 * 0.85 && rate < 24000 * 1.15);
    std::printf("       measured %.0f samples/s over %lld ms\n", rate,
                static_cast<long long>(elapsedMs));

    report("per-slice audio rides along", rx.slice > 0 && rx.lastSliceId == 0);
    report("spectrum rows arrive at their cadence",
           rx.spectrum > 0 && rx.spectrum < rx.audio + 1);

    // ── Keyed mutes without stopping the stream ──────────────────────────
    QMetaObject::invokeMethod(src, [src] { src->setKeyed(true); },
                              Qt::QueuedConnection);
    waitFor([&] { return false; }, 60);   // let the queued flip land
    rx.audio = 0;
    report("keyed frames keep flowing",
           waitFor([&] { return rx.audio > 2; }, 1000));
    report("...and are silent (Principle VI)",
           !rx.lastAudio.isEmpty() && frameIsSilent(rx.lastAudio));
    QMetaObject::invokeMethod(src, [src] { src->setKeyed(false); },
                              Qt::QueuedConnection);

    // ── Stallscope freezes the spectrum, audio unaffected ────────────────
    QMetaObject::invokeMethod(src, [src] { src->setScopeStalled(true); },
                              Qt::QueuedConnection);
    waitFor([&] { return false; }, 60);
    rx.spectrum = 0;
    rx.audio = 0;
    waitFor([&] { return rx.audio > 5; }, 1000);
    report("stallscope stops spectrum rows while audio flows",
           rx.spectrum == 0 && rx.audio > 5);

    // ── A queued noise control lands (no crash, still producing) ─────────
    QMetaObject::invokeMethod(src, [src] {
        src->setNoiseEnabled(QStringLiteral("white"), true);
        src->setNoiseLevel(QStringLiteral("white"), -30.0);
        src->setVfoMhz(7.074);
        src->setLowerSideband(true);
    }, Qt::QueuedConnection);
    rx.audio = 0;
    report("controls apply on the worker without disturbing the stream",
           waitFor([&] { return rx.audio > 2; }, 1000));

    // ── Multi-pan: rows address every live pan (#4887 phase 4) ───────────
    QMetaObject::invokeMethod(src, [src] {
        src->setScopeStalled(false);   // stallscope check above left it on
        src->setPanIndices({0, 2});
    }, Qt::QueuedConnection);
    waitFor([&] { return false; }, 60);
    rx.panIdsSeen.clear();
    rx.spectrum = 0;
    waitFor([&] { return rx.spectrum > 6; }, 2000);
    report("spectrum rows address every live pan, and only those",
           rx.panIdsSeen.contains(0) && rx.panIdsSeen.contains(2)
               && rx.panIdsSeen.size() == 2);

    // ── Stop stops ───────────────────────────────────────────────────────
    QMetaObject::invokeMethod(src, &SimSignalSource::stop, Qt::QueuedConnection);
    waitFor([&] { return false; }, 120);   // drain in-flight emissions
    rx.audio = 0;
    waitFor([&] { return false; }, 250);
    report("stop halts production", rx.audio == 0);

    src->deleteLater();
    worker.quit();
    worker.wait(3000);

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
