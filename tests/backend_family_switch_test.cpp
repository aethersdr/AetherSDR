// IRadioBackend threading contract, rule 5 (IRadioBackend.h, "THREADING AND
// LIFETIME CONTRACT"): teardown is bounded and ordered, and the model's
// per-backend wiring does not accumulate across family switches (#4599).
//
// Cycles every family the factory can build through the PRODUCTION switch
// (RadioModel::rebuildBackendForTest runs the same drop/teardown/setup/
// announce sequence connectToRadio() does, minus the dial), including one
// switch away from a LIVE simulator whose worker threads are streaming —
// the case where a BlockingQueuedConnection in a destructor turns into a
// wait cycle. A watchdog thread converts a hang into a failure instead of
// a CI timeout.
//
// No socket, no device, no radio: construction and teardown only.

#include "TestSettingsProfile.h"
#include "core/RadioDiscovery.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/sim/SimBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace AetherSDR;

namespace {
int g_failures = 0;
void check(bool ok, const QString& what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    g_failures += !ok;
}

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// A teardown that takes longer than this is not "slow"; it is waiting on
// something that is waiting on it.
constexpr int kStepBudgetMs = 5000;
constexpr int kWatchdogSeconds = 90;

std::atomic<bool> g_done{false};
std::atomic<const char*> g_phase{"startup"};

void watchdog()
{
    for (int i = 0; i < kWatchdogSeconds * 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (g_done.load()) return;
    }
    std::printf("[FAIL] watchdog: test hung for %d s during '%s' — teardown wait cycle\n",
                kWatchdogSeconds, g_phase.load());
    std::fflush(stdout);
    std::_Exit(2);
}

RadioInfo demoInfo()
{
    RadioInfo i;
    i.name    = QStringLiteral("FLEX-6700");
    i.model   = SimBackend::demoModelName();
    i.serial  = SimBackend::demoSerial();
    i.family  = SimBackend::familyName();
    i.address = QHostAddress(QHostAddress::LocalHost);   // synthetic; never dialed
    i.port    = 4992;
    return i;
}

// One production switch, timed.
bool switchTo(RadioModel& model, const QString& family, QSignalSpy& rebuilt)
{
    g_phase.store("switch");
    const int before = rebuilt.count();
    QElapsedTimer t;
    t.start();
    const bool built = model.rebuildBackendForTest(family);
    const qint64 ms = t.elapsed();
    if (!built) {
        std::printf("  %s: not built into this binary, skipped\n", qPrintable(family));
        return false;
    }
    check(model.family() == family, QStringLiteral("-> %1: model reports the family").arg(family));
    check(model.backend() != nullptr, QStringLiteral("-> %1: backend present").arg(family));
    check(rebuilt.count() == before + 1,
          QStringLiteral("-> %1: backendRebuilt announced exactly once").arg(family));
    check(ms < kStepBudgetMs,
          QStringLiteral("-> %1: teardown+setup within budget (%2 ms)").arg(family).arg(ms));
    check(model.slices().isEmpty(), QStringLiteral("-> %1: no slice models carried across").arg(family));
    check(model.panadapters().isEmpty(), QStringLiteral("-> %1: no pan models carried across").arg(family));
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("backend-family-switch"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    std::thread(watchdog).detach();

    {
        RadioModel model;
        QSignalSpy rebuilt(&model, &RadioModel::backendRebuilt);
        QSignalSpy state(&model, &RadioModel::connectionStateChanged);

        // 1. Every family, cold: construct, wire, tear down, twice around.
        const QStringList cycle = {
            QStringLiteral("sim"),  QStringLiteral("hl2"),  QStringLiteral("anan"),
            QStringLiteral("icom"), QStringLiteral("rtl"),  QStringLiteral("flex"),
            QStringLiteral("hl2"),  QStringLiteral("sim"),  QStringLiteral("icom"),
            QStringLiteral("flex"), QStringLiteral("anan"), QStringLiteral("flex"),
        };
        std::printf("-- cold cycle\n");
        for (const QString& family : cycle) {
            switchTo(model, family, rebuilt);
        }

        // 2. Away from a LIVE simulator: threads streaming, then the switch.
        std::printf("-- live simulator -> hl2\n");
        g_phase.store("connect sim");
        model.connectToRadio(demoInfo());
        for (int i = 0; i < 60 && !model.isConnected(); ++i) spin(50);
        check(model.isConnected(), "live: simulator connected");
        spin(500);                                    // let audio/spectrum stream
        const int stateBefore = state.count();
        switchTo(model, QStringLiteral("hl2"), rebuilt);
        spin(200);
        int disconnects = 0;
        for (int i = stateBefore; i < state.count(); ++i) {
            if (!state.at(i).at(0).toBool()) ++disconnects;
        }
        check(disconnects <= 1,
              QStringLiteral("live: at most one disconnected report on switch (%1)").arg(disconnects));

        // 3. Back to a live simulator and tear it down through disconnect, then
        //    switch — the ordered path (rule 5: disconnected() once, then gone).
        std::printf("-- live simulator -> disconnect -> flex\n");
        g_phase.store("reconnect sim");
        model.connectToRadio(demoInfo());
        for (int i = 0; i < 60 && !model.isConnected(); ++i) spin(50);
        check(model.isConnected(), "live: simulator reconnected");
        g_phase.store("disconnect sim");
        {
            QElapsedTimer t;
            t.start();
            model.disconnectFromRadio();
            for (int i = 0; i < 60 && model.isConnected(); ++i) spin(50);
            check(!model.isConnected(), "live: disconnect reported");
            check(t.elapsed() < kStepBudgetMs,
                  QStringLiteral("live: disconnect within budget (%1 ms)").arg(t.elapsed()));
        }
        switchTo(model, QStringLiteral("flex"), rebuilt);

        // 4. #4599: after all of the above, one backend emission produces ONE
        //    model reaction. A connection whose sender and receiver both
        //    outlive the backend would have gained a copy per switch.
        std::printf("-- duplicate-wiring check\n");
        g_phase.store("dup check");
        switchTo(model, QStringLiteral("hl2"), rebuilt);   // non-Flex: seam slices materialise
        QSignalSpy added(&model, &RadioModel::sliceAdded);
        SliceDelta d;
        d.inUse = true;
        d.active = true;
        d.letter = QStringLiteral("A");
        d.panId = QStringLiteral("hl2-0");
        d.frequency = 14.225;
        d.mode = QStringLiteral("USB");
        d.filterLow = 100;
        d.filterHigh = 2700;
        QSignalSpy removed(&model, &RadioModel::sliceRemoved);
        model.emitBackendSliceChangedForTest(0, d);
        spin(300);   // long enough for anything the previous session left queued
        // The previous session's synthetic connection posted a trailing
        // "slice 0 client_handle=…" status that, before the rule-5 guard on
        // the connection handlers, landed here and removed the NEW session's
        // slice as a foreign client's. 2 in 12 runs; now pinned.
        check(removed.count() == 0,
              QStringLiteral("no stale event from the previous session removed the new slice (%1)")
                  .arg(removed.count()));
        check(added.count() == 1,
              QStringLiteral("one seam sliceChanged -> exactly one sliceAdded (%1)").arg(added.count()));
        check(model.slices().size() == 1,
              QStringLiteral("exactly one slice model exists (%1)").arg(model.slices().size()));

        // 5. Destroy the model with a backend attached (the app-exit path).
        g_phase.store("destroy model");
        QElapsedTimer t;
        t.start();
        // scope end destroys `model`
        std::printf("-- destroy with hl2 attached\n");
        (void)t;
    }
    g_done.store(true);

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
