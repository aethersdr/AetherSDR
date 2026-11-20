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
#include "core/backends/sim/DemoRadioConstants.h"

#include <algorithm>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSet>
#include <QThread>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <utility>

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
    QByteArray lastSpectrum;
    QVector<float> audioTail;   // left channel, last second

    explicit Collector(AetherSDR::SimSignalSource* src)
    {
        connect(src, &AetherSDR::SimSignalSource::audioFrameReady, this,
                [this](const AetherSDR::PcmFrame& frame) {
                    const QByteArray b = frame.legacyStereo24();
                    ++audio;
                    samples += b.size() / qint64(2 * sizeof(float));
                    lastAudio = b;
                    const auto* f = reinterpret_cast<const float*>(b.constData());
                    for (qsizetype i = 0; i + 1 < b.size() / qsizetype(sizeof(float)); i += 2) {
                        audioTail.append(f[i]);
                    }
                    if (audioTail.size() > 24000) {
                        audioTail.remove(0, audioTail.size() - 24000);
                    }
                });
        connect(src, &AetherSDR::SimSignalSource::sliceAudioFrameReady, this,
                [this](int id, const AetherSDR::PcmFrame&) {
                    ++slice;
                    lastSliceId = id;
                });
        connect(src, &AetherSDR::SimSignalSource::spectrumFrameReady, this,
                [this](int pan, quint64, const QByteArray& bins) {
                    ++spectrum;
                    panIdsSeen.insert(pan);
                    lastSpectrum = bins;
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
    QMetaObject::invokeMethod(src, [src] { src->startSession(1); }, Qt::QueuedConnection);
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

    // ── RF-anchored signals are drawn where they are in RF (#7420) ───────
    // The birdie is a carrier at 14.1012 MHz. With the pan centered at 14.100 it
    // is drawn 1200 Hz right of the center whatever the dial and sideband: USB
    // and LSB with the dial off the center, RTTY with the dial on it.
    QMetaObject::invokeMethod(src, [src] {
        src->setScopeStalled(false);
        src->setNoiseEnabled(QStringLiteral("pink"), false);
        src->setPanCenterMhz(14.100);
    }, Qt::QueuedConnection);
    const int kBins = 1024;
    const double hzPerBin = AetherSDR::DemoRadio::kAudioSpanHz / kBins;
    const int birdieBin = static_cast<int>(kBins / 2 + 1200.0 / hzPerBin);
    auto peakBinAfter = [&](auto apply) {
        QMetaObject::invokeMethod(src, apply, Qt::QueuedConnection);
        waitFor([&] { return false; }, 120);           // let the queued change land
        rx.spectrum = 0;
        waitFor([&] { return rx.spectrum > 3; }, 1000);  // a row drawn after it
        const auto* bins = reinterpret_cast<const float*>(rx.lastSpectrum.constData());
        const int n = rx.lastSpectrum.size() / int(sizeof(float));
        return n == kBins ? int(std::max_element(bins, bins + n) - bins) : -1;
    };
    const int usb = peakBinAfter([src] { src->setRttyMode(false); src->setLowerSideband(false); src->setVfoMhz(14.1006); });
    const int lsb = peakBinAfter([src] { src->setLowerSideband(true); src->setVfoMhz(14.1018); });
    const int rtty = peakBinAfter([src] { src->setRttyMode(true); src->setVfoMhz(14.100); });
    std::printf("birdie peak bins: USB %d, LSB %d, RTTY %d (expected %d)\n", usb, lsb, rtty, birdieBin);
    report("USB, dial off the pan center: the birdie is drawn at its RF frequency", usb == birdieBin);
    report("LSB, dial off the pan center: the birdie is drawn at its RF frequency", lsb == birdieBin);
    report("RTTY mode: the birdie is drawn at its RF frequency", rtty == birdieBin);
    const int offCenter = peakBinAfter([src] {
        src->setRttyMode(false); src->setLowerSideband(false); src->setVfoMhz(14.100); src->setPanCenterMhz(14.0995); });
    const int offCenterBin = static_cast<int>(kBins / 2 + 1700.0 / hzPerBin);
    report("pan center away from 14.100: the birdie is drawn at its RF frequency", offCenter == offCenterBin);

    // ANF in RTTY mode carves the birdie where it is drawn, and nothing at its
    // audio pitch (2125 - 1200 = 925 Hz), where it is not. Pink noise keeps
    // every uncarved bin above the floor.
    QMetaObject::invokeMethod(src, [src] {
        src->setPanCenterMhz(14.100); src->setLowerSideband(true); src->setRttyMode(true); src->setVfoMhz(14.100);
        src->setNoiseEnabled(QStringLiteral("pink"), true);
        src->setAnf(true);
    }, Qt::QueuedConnection);
    waitFor([&] { return false; }, 300);
    rx.spectrum = 0;
    waitFor([&] { return rx.spectrum > 3; }, 1000);
    {
        const auto* bins = reinterpret_cast<const float*>(rx.lastSpectrum.constData());
        const int n = rx.lastSpectrum.size() / int(sizeof(float));
        const int pitchBin = static_cast<int>(kBins / 2 + 925.0 / hzPerBin);
        const float lowest = n == kBins ? *std::min_element(bins, bins + n) : 0.0f;
        report("ANF in RTTY mode: nothing is carved at the birdie's audio pitch",
               n == kBins && bins[pitchBin] > lowest);
        report("ANF in RTTY mode: the birdie's notch is carved where it is drawn",
               n == kBins && bins[birdieBin] == lowest);
    }
    QMetaObject::invokeMethod(src, [src] {
        src->setAnf(false);
        src->setNoiseEnabled(QStringLiteral("pink"), false);
    }, Qt::QueuedConnection);
    waitFor([&] { return false; }, 120);

    // ── RTTY stations: which tone is mark (#7420) ─────────────────────────
    // A station keys 1 s of idle mark when switched on. In RTTY mode with the
    // dial on its upper tone, "rtty" (mark on the upper tone) sounds at
    // 2125 Hz and "rtty-rev" (mark on the lower tone) at 2295 Hz.
    auto power = [](const QVector<float>& x, double hz) {
        const double w = 2.0 * 3.141592653589793 * hz / 24000.0, c = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (float v : x) { const double s0 = v + c * s1 - s2; s2 = s1; s1 = s0; }
        return s1 * s1 + s2 * s2 - c * s1 * s2;
    };
    auto idleMarkPowers = [&](const char* station) {
        QMetaObject::invokeMethod(src, [src, station] {
            src->setPanCenterMhz(14.100); src->setLowerSideband(true); src->setRttyMode(true); src->setVfoMhz(14.100);
            src->setNoiseEnabled(QStringLiteral("birdie"), false);
            src->setNoiseEnabled(QStringLiteral("rtty"), false);
            src->setNoiseEnabled(QStringLiteral("rtty-rev"), false);
            src->setNoiseEnabled(QString::fromLatin1(station), true);
        }, Qt::QueuedConnection);
        waitFor([&] { return false; }, 80);
        rx.audioTail.clear();
        waitFor([&] { return false; }, 300);   // well inside the 1 s of idle mark
        return std::pair{power(rx.audioTail, 2125.0), power(rx.audioTail, 2295.0)};
    };
    const auto normal = idleMarkPowers("rtty");
    const auto reversed = idleMarkPowers("rtty-rev");
    report("RTTY normal: idle mark sounds at 2125 Hz in RTTY mode", normal.first > 10.0 * normal.second);
    report("RTTY reversed: idle mark sounds at 2295 Hz in RTTY mode", reversed.second > 10.0 * reversed.first);
    QMetaObject::invokeMethod(src, [src] {
        src->setNoiseEnabled(QStringLiteral("rtty-rev"), false);
        src->setNoiseEnabled(QStringLiteral("birdie"), true);
        src->setRttyMode(false); src->setLowerSideband(false); src->setVfoMhz(14.100); src->setPanCenterMhz(14.100);
    }, Qt::QueuedConnection);

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
