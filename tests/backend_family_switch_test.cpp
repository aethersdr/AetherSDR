// IRadioBackend threading contract, rule 5 (IRadioBackend.h, "THREADING AND
// LIFETIME CONTRACT"): teardown is bounded and ordered, and a delivery posted
// by a backend that has since been torn down does not reach the next session.
//
// Cycles every family the factory can build through the PRODUCTION switch
// (RadioModel::rebuildBackendForTest calls the same rebuildBackendForFamily()
// connectToRadio() calls, minus the dial), including a switch away from a LIVE
// simulator whose worker threads are streaming — the case where a
// BlockingQueuedConnection in a destructor turns into a wait cycle. A watchdog
// converts a hang into a failure instead of a CI timeout.
//
// THE LOAD-BEARING CHECK IS THE INJECTED ONE (step 6). Racing the simulator's
// own trailing status line caught the original defect, but only ~1 run in 6 —
// nothing forces the stale event to be queued-but-undrained at the moment of
// the switch. Step 6 forces it: it emits the status from the harvested
// RadioConnection's own thread under a BlockingQueuedConnection, so the
// QMetaCallEvent is provably in the main queue before the switch runs.
// Measured against a build with the setupBackend() generation guards reverted:
// step 5b's race fails 9 runs in 30, step 6 fails 10 in 10.
//
// No socket, no device, no radio: construction, teardown, and the in-process
// synthetic wire only.

#include "TestSettingsProfile.h"
#include "core/RadioConnection.h"
#include "core/RadioDiscovery.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/sim/SimBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QMap>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace AetherSDR;

// A sanitizer build runs this workload — 12 cold family switches that construct
// and join HL2/ANAN worker threads each time — several times slower, and
// sanitizers.yml runs the suite unfiltered. Scale the time budgets rather than
// leave a correct-but-slow run to fail on wall clock; the watchdog below is the
// hard hang detector either way.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#  define AETHER_TEST_SANITIZED 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#    define AETHER_TEST_SANITIZED 1
#  endif
#endif

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

#ifdef AETHER_TEST_SANITIZED
constexpr int kSlowdown = 6;
#else
constexpr int kSlowdown = 1;
#endif

// A teardown that takes longer than this is not "slow"; it is waiting on
// something that is waiting on it.
constexpr int kStepBudgetMs = 5000 * kSlowdown;
constexpr int kConnectDeadlineMs = 3000 * kSlowdown;
constexpr int kWatchdogSeconds = 90 * kSlowdown;

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

bool waitForConnected(RadioModel& model, bool want)
{
    for (int i = 0; i * 50 < kConnectDeadlineMs && model.isConnected() != want; ++i)
        spin(50);
    return model.isConnected() == want;
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

// A slice the way a non-Flex backend announces one over the seam.
SliceDelta seamSlice()
{
    SliceDelta d;
    d.inUse = true;
    d.active = true;
    d.letter = QStringLiteral("A");
    d.panId = QStringLiteral("hl2-0");
    d.frequency = 14.225;
    d.mode = QStringLiteral("USB");
    d.filterLow = 100;
    d.filterHigh = 2700;
    return d;
}

// How many capabilitiesChanged the model emits across ONE connect edge. The
// #4599 defect class is a connection installed per backend whose sender AND
// receiver both outlive it, so it gains a live copy per family switch and this
// count multiplies. The number itself does not matter — only that twelve
// switches do not change it.
int capabilityEmissionsForOneConnect(RadioModel& model, const char* label)
{
    QSignalSpy caps(&model, &RadioModel::capabilitiesChanged);
    model.connectToRadio(demoInfo());
    check(waitForConnected(model, true),
          QStringLiteral("%1: simulator connected").arg(QLatin1String(label)));
    spin(300);
    const int n = caps.count();
    model.disconnectFromRadio();
    check(waitForConnected(model, false),
          QStringLiteral("%1: simulator disconnected").arg(QLatin1String(label)));
    spin(200);
    return n;
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("backend-family-switch"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    std::thread(watchdog).detach();
#ifdef AETHER_TEST_SANITIZED
    std::printf("-- sanitizer build: time budgets scaled %dx\n", kSlowdown);
#endif

    QElapsedTimer destroyTimer;
    {
        RadioModel model;
        QSignalSpy rebuilt(&model, &RadioModel::backendRebuilt);
        QSignalSpy state(&model, &RadioModel::connectionStateChanged);

        // 1. Baseline for the accumulation pin, before any switch has happened.
        std::printf("-- baseline connect edge\n");
        g_phase.store("baseline");
        const int baselineCaps = capabilityEmissionsForOneConnect(model, "baseline");
        std::printf("  baseline: %d capabilitiesChanged on one connect edge\n", baselineCaps);

        // 2. Every family, cold: construct, wire, tear down, twice around.
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

        // 3. #4599's defect class: per-backend wiring must not accumulate across
        //    those twelve switches. A connection installed in setupBackend()
        //    whose sender and receiver are both `this` survives
        //    teardownBackend() and gains one live copy per switch; its symptom
        //    is N capabilitiesChanged per connect edge.
        std::printf("-- accumulation check (#4599 defect class)\n");
        g_phase.store("accumulation");
        const int afterCycleCaps = capabilityEmissionsForOneConnect(model, "accumulation");
        std::printf("  after 12 switches: %d capabilitiesChanged on one connect edge\n",
                    afterCycleCaps);
        check(afterCycleCaps == baselineCaps,
              QStringLiteral("per-connect capabilitiesChanged does not grow with switches (%1 -> %2)")
                  .arg(baselineCaps).arg(afterCycleCaps));

        // 4. Away from a LIVE simulator: threads streaming, then the switch.
        std::printf("-- live simulator -> hl2\n");
        g_phase.store("connect sim");
        model.connectToRadio(demoInfo());
        check(waitForConnected(model, true), "live: simulator connected");
        spin(500);                                    // let audio/spectrum stream
        switchTo(model, QStringLiteral("hl2"), rebuilt);
        spin(200);

        // 5. Back to a live simulator and tear it down through disconnect — the
        //    ordered path. Rule 5 says disconnected() lands exactly once, and
        //    the model must report it exactly once too.
        std::printf("-- live simulator -> disconnect -> flex\n");
        g_phase.store("reconnect sim");
        model.connectToRadio(demoInfo());
        check(waitForConnected(model, true), "live: simulator reconnected");
        g_phase.store("disconnect sim");
        {
            const int before = state.count();
            QElapsedTimer t;
            t.start();
            model.disconnectFromRadio();
            check(waitForConnected(model, false), "live: disconnect reported");
            const qint64 ms = t.elapsed();
            spin(300);                                // anything queued behind it lands
            int disconnects = 0;
            for (int i = before; i < state.count(); ++i)
                if (!state.at(i).at(0).toBool()) ++disconnects;
            check(disconnects == 1,
                  QStringLiteral("live: the clean disconnect reports disconnected EXACTLY once (%1)")
                      .arg(disconnects));
            check(ms < kStepBudgetMs,
                  QStringLiteral("live: disconnect within budget (%1 ms)").arg(ms));
        }
        switchTo(model, QStringLiteral("flex"), rebuilt);

        // 5b. The race that originally found the defect, kept as a secondary
        //     tripwire: the simulator's own trailing "slice 0 client_handle=…"
        //     echo, however the scheduler happens to order it. Catches a
        //     regression ~1 run in 6 by itself, which is why step 6 exists.
        std::printf("-- stale-status race (secondary)\n");
        g_phase.store("race");
        switchTo(model, QStringLiteral("hl2"), rebuilt);
        {
            QSignalSpy added(&model, &RadioModel::sliceAdded);
            QSignalSpy removed(&model, &RadioModel::sliceRemoved);
            model.emitBackendSliceChangedForTest(0, seamSlice());
            spin(300);
            check(removed.count() == 0,
                  QStringLiteral("race: no stale event removed the new slice (%1)").arg(removed.count()));
            check(added.count() == 1,
                  QStringLiteral("race: one seam sliceChanged -> one sliceAdded (%1)").arg(added.count()));
        }

        // 6. THE DETERMINISTIC PIN. Post a trailing "slice 0 client_handle=…"
        //    from the harvested RadioConnection's OWN thread under a blocking
        //    invoke, so the QMetaCallEvent is guaranteed to be sitting in the
        //    main queue when the switch tears that connection down. Without the
        //    generation guard in setupBackend() the next session's handler runs
        //    it, reads a foreign client's slice, and deletes the slice that
        //    session just created.
        //
        //    Placement is load-bearing: the simulator must be LIVE (so the
        //    harvested connection exists and is synthetic), and there must be no
        //    spin() or disconnectFromRadio() between the injection and the
        //    switch — either one drains the event into the old session, where it
        //    is harmless and proves nothing.
        std::printf("-- deterministic stale-status injection\n");
        g_phase.store("inject");
        model.connectToRadio(demoInfo());
        check(waitForConnected(model, true), "inject: simulator connected");
        spin(300);
        {
            auto* sim = dynamic_cast<SimBackend*>(model.backend());
            check(sim != nullptr, "inject: backend is the simulator");
            RadioConnection* conn = sim ? sim->connection() : nullptr;
            check(conn != nullptr, "inject: the simulator vends a RadioConnection");
            check(conn && conn->thread() != QThread::currentThread(),
                  "inject: that connection lives on its own thread");
            if (conn) {
                QMetaObject::invokeMethod(conn, [conn] {
                    QMap<QString, QString> kvs;
                    kvs.insert(QStringLiteral("client_handle"), QStringLiteral("0xDE300001"));
                    kvs.insert(QStringLiteral("RF_frequency"), QStringLiteral("14.100000"));
                    emit conn->statusReceived(QStringLiteral("slice 0"), kvs);
                }, Qt::BlockingQueuedConnection);
            }
        }
        switchTo(model, QStringLiteral("hl2"), rebuilt);   // no spin in between
        {
            QSignalSpy added(&model, &RadioModel::sliceAdded);
            QSignalSpy removed(&model, &RadioModel::sliceRemoved);
            model.emitBackendSliceChangedForTest(0, seamSlice());
            spin(300);
            check(removed.count() == 0,
                  QStringLiteral("inject: the injected stale status did not remove the new slice (%1)")
                      .arg(removed.count()));
            check(added.count() == 1,
                  QStringLiteral("inject: one sliceAdded (%1)").arg(added.count()));
            check(model.slices().size() == 1,
                  QStringLiteral("inject: exactly one slice model exists (%1)").arg(model.slices().size()));
        }

        // 7. Destroy the model with a backend attached (the app-exit path). The
        //    timer is declared OUTSIDE this scope so it outlives ~RadioModel and
        //    can actually be read.
        std::printf("-- destroy with hl2 attached\n");
        g_phase.store("destroy model");
        destroyTimer.start();
    }
    check(destroyTimer.elapsed() < kStepBudgetMs,
          QStringLiteral("destroy with a backend attached is bounded (%1 ms)")
              .arg(destroyTimer.elapsed()));
    g_done.store(true);

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
