// Socket-free initial-build timing, receiver identity and failure rollback.
// Receiver/DSP setup and link state are injected without connectRadio(),
// discovery or MetisClient::start(). Thread holds force each tested ordering.
// See docs/HERMES.md §22.4 for the production contract.

#include "core/backends/SliceDelta.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2Receivers.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisClient.h"
#include "models/MeterModel.h"

#include "TestSettingsProfile.h"
#include "TestDspBuildWait.h"
#include "SeamThreadAffinityProbe.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSemaphore>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cmath>
#include <vector>
#include <cstdio>
#include <tuple>

namespace AetherSDR::hl2 {

struct Hl2PanCreateTestAccess {
    static bool rebuildReceivers(Hl2Backend& backend, int count)
    {
        backend.m_boardMaxRx = Hl2Backend::kAssumedBoardMaxRx;
        backend.buildReceivers(count);
        for (const Hl2ReceiverIds& ids : backend.m_ids.all()) {
            Hl2Backend::Receiver* const receiver = backend.rx(ids.ddcIndex);
            if (!receiver || !receiver->dsp) {
                return false;
            }
            Hl2RxDsp::Config config;
            config.inputSampleRateHz = backend.m_rateLedger.committed();
            std::tie(config.filterLowHz, config.filterHighHz) =
                backend.dspFilterHz(*receiver);
            bool configured = false;
            int channelId = -1;
            QMetaObject::invokeMethod(receiver->dsp, [receiver, config, &configured, &channelId] {
                configured = receiver->dsp->configure(config);
                channelId = receiver->dsp->wdspChannelId();
            }, Qt::BlockingQueuedConnection);
            if (!configured) {
                return false;
            }
            receiver->configuredRateHz = config.inputSampleRateHz;
            Hl2ReceiverIds* const current = backend.m_ids.mutableByDdc(ids.ddcIndex);
            current->dspChannel = channelId;
            current->analyzerId = current->uiNumber;
        }
        backend.publishIoDsps();
        return true;
    }

    static bool transportIdle(Hl2Backend& backend)
    {
        bool idle = false;
        QMetaObject::invokeMethod(backend.m_metis, [&backend, &idle] {
            idle = !backend.m_metis->isRunning();
            for (const QObject* child : backend.m_metis->children()) {
                idle = idle && !child->inherits("QUdpSocket");
            }
        }, Qt::BlockingQueuedConnection);
        return idle;
    }

    static void linkUp(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_metis, "linkUp",
                                  Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    }
    static QObject* wire(Hl2Backend& backend) { return backend.m_metis; }
    static QString sliceMeterName(int uiNumber) { return Hl2Backend::sliceMeterName(uiNumber); }
    static QObject* buildContext(Hl2Backend& backend)
    {
        return backend.m_dspBuildContext;
    }
    static int receiverCount(const Hl2Backend& backend)
    {
        return static_cast<int>(backend.m_rx.size());
    }
    static int ceiling(const Hl2Backend& backend)
    {
        return backend.receiverCeiling();
    }
    static int lastReceiverDspChannel(const Hl2Backend& backend)
    {
        const int ddc = static_cast<int>(backend.m_rx.size()) - 1;
        const Hl2ReceiverIds* ids = backend.m_ids.byDdc(ddc);
        return ids ? ids->dspChannel : -2;
    }
    static int lastReceiverUi(const Hl2Backend& backend)
    {
        const int ddc = static_cast<int>(backend.m_rx.size()) - 1;
        const Hl2ReceiverIds* ids = backend.m_ids.byDdc(ddc);
        return ids ? ids->uiNumber : -1;
    }
    static int dspChannelForUi(const Hl2Backend& backend, int uiNumber)
    {
        const Hl2ReceiverIds* ids = backend.m_ids.byUi(uiNumber);
        return ids ? ids->dspChannel : -2;
    }
    static bool buildInFlightForUi(const Hl2Backend& backend, int uiNumber)
    {
        const Hl2ReceiverIds* ids = backend.m_ids.byUi(uiNumber);
        const Hl2Backend::Receiver* r = ids ? backend.rx(ids->ddcIndex) : nullptr;
        return r && r->dspBuildInFlight;
    }
    static Hl2RxDsp* receiverDsp(Hl2Backend& backend, int ddc)
    {
        Hl2Backend::Receiver* r = backend.rx(ddc);
        return r ? r->dsp : nullptr;
    }

    static int txDdc(const Hl2Backend& backend) { return backend.m_txDdc; }
    static int activeDdc(const Hl2Backend& backend) { return backend.m_activeDdc; }

    static bool roleNamesALiveReceiver(Hl2Backend& backend, int role)
    {
        return backend.rx(role) != nullptr;
    }

    static int forceNextBuildToFail(Hl2Backend& backend)
    {
        const int previous = backend.m_rateLedger.committed();
        backend.m_rateLedger.commit(0);
        return previous;
    }
    static void restoreCommittedRate(Hl2Backend& backend, int rateHz)
    {
        backend.m_rateLedger.commit(rateHz);
    }
};

}   // namespace AetherSDR::hl2

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;
using Access = AetherSDR::hl2::Hl2PanCreateTestAccess;

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", condition ? " OK " : "FAIL", what);
    if (!condition)
        ++g_failures;
}

constexpr int kHoldMs = 1000;

constexpr int kReturnBudgetMs = kHoldMs / 3;

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

void occupyFor(QObject* target, int ms)
{
    QEventLoop started;
    QMetaObject::invokeMethod(target, [&started, ms] {
        QMetaObject::invokeMethod(&started, "quit", Qt::QueuedConnection);
        QThread::msleep(static_cast<unsigned long>(ms));
    }, Qt::QueuedConnection);
    started.exec();
}

class ThreadHold
{
public:
    explicit ThreadHold(QObject* target)
    {
        QMetaObject::invokeMethod(target, [this] {
            m_started.release();
            m_release.acquire();
            m_finished.release();
        }, Qt::QueuedConnection);
        m_started.acquire();   // returns only once the thread is actually stopped
    }
    ThreadHold(const ThreadHold&) = delete;
    ThreadHold& operator=(const ThreadHold&) = delete;
    void release()
    {
        if (!m_released) {
            m_released = true;
            m_release.release();
            m_finished.acquire();
        }
    }
    ~ThreadHold() { release(); }

private:
    QSemaphore m_started;
    QSemaphore m_release;
    QSemaphore m_finished;
    bool m_released = false;
};

bool awaitSwapWithoutPumping(AetherSDR::hl2::Hl2RxDsp* dsp)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < AetherSDR::test::kDspBuildTimeoutMs) {
        int id = -1;
        QMetaObject::invokeMethod(dsp, [dsp, &id] { id = dsp->wdspChannelId(); },
                                  Qt::BlockingQueuedConnection);
        if (id >= 0)
            return true;
        QThread::msleep(5);
    }
    return false;
}

void flushDeferredDeletes(QObject* target)
{
    QMetaObject::invokeMethod(target, [] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }, Qt::BlockingQueuedConnection);
}

bool bringUp(Hl2Backend& backend)
{
    if (!Access::rebuildReceivers(backend, 1)) {
        check(false, "the injected receiver setup succeeds");
        return false;
    }
    Access::linkUp(backend);
    check(backend.isConnected(), "the injected link edge reaches the backend");
    check(Access::ceiling(backend) > Access::receiverCount(backend),
          "the ceiling leaves room for the receiver this test adds");
    check(Access::transportIdle(backend), "setup opens no transport socket");
    return backend.isConnected();
}

// A receiver seeded from the first while that one is in DIGU/DIGL copies its
// mode and its AGC off, so it needs the held AGC too or it can never leave
// off (#5629).
void aPanSeededInADataModeCarriesTheHeldAgc()
{
    Hl2Backend backend;
    if (!bringUp(backend)) {
        return;
    }
    QHash<int, QString> agc;
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                     [&agc](int id, const SliceDelta& d) {
                         if (d.agcMode) agc[id] = *d.agcMode;
                     });
    backend.setSliceAgc(0, QStringLiteral("slow"), 65);
    backend.setSliceMode(0, QStringLiteral("DIGU"));
    check(backend.createPanadapter(), "a receiver is admitted beside a DIGU one");
    const int ui = Access::lastReceiverUi(backend);
    check(agc.value(ui) == QStringLiteral("off"),
          "a receiver seeded from a DIGU one opens with AGC off");
    backend.setSliceMode(ui, QStringLiteral("USB"));
    check(agc.value(ui) == QStringLiteral("slow"),
          "and returns to the first receiver's held AGC on leaving DIGU");
    check(agc.value(0) == QStringLiteral("off"),
          "the first receiver stays in its data mode's off");
    AetherSDR::test::spinUntil(
        [&] { return Access::lastReceiverDspChannel(backend) >= 0; });
}

void theGuiThreadIsNotHeld()
{
    Hl2Backend backend;
    if (!bringUp(backend)) {
        return;
    }

    int announced = 0;
    QObject::connect(&backend, &IRadioBackend::panCenterBandwidthChanged, &backend,
                     [&announced](const QString&, double, double) { ++announced; });

    const int before = Access::receiverCount(backend);

    occupyFor(Access::wire(backend), kHoldMs);

    QElapsedTimer clock;
    clock.start();
    const bool admitted = backend.createPanadapter();
    const qint64 elapsedMs = clock.elapsed();

    std::fprintf(stderr,
                 "     createPanadapter() returned in %lld ms with the I/O "
                 "thread occupied for %d ms\n",
                 static_cast<long long>(elapsedMs), kHoldMs);

    check(admitted, "createPanadapter() admits the receiver");
    check(elapsedMs < kReturnBudgetMs,
          "createPanadapter() returns without waiting on the occupied I/O thread");

    check(announced >= 1,
          "the pan is announced before createPanadapter() returns, with the I/O "
          "thread still held");
    check(Access::receiverCount(backend) == before + 1,
          "the receiver set actually grew");

    AetherSDR::test::spinUntil(
        [&] { return Access::lastReceiverDspChannel(backend) >= 0; });
    check(Access::lastReceiverDspChannel(backend) >= 0,
          "the new receiver ends up with a real WDSP channel");
}

void theBuildRunsOnTheBuildThread()
{
    Hl2Backend backend;
    if (!bringUp(backend)) {
        return;
    }

    occupyFor(Access::buildContext(backend), kHoldMs);

    QElapsedTimer clock;
    clock.start();
    const bool admitted = backend.createPanadapter();
    const qint64 elapsedMs = clock.elapsed();
    check(admitted, "createPanadapter() admits the receiver");
    check(elapsedMs < kReturnBudgetMs,
          "createPanadapter() does not wait on the build thread either");

    spin(kHoldMs / 2);
    const int midChannel = Access::lastReceiverDspChannel(backend);
    std::fprintf(stderr,
                 "     WDSP channel id is %d after %d ms with the build thread "
                 "still occupied\n",
                 midChannel, kHoldMs / 2);
    check(midChannel < 0,
          "no WDSP channel is open while the build thread is occupied — the "
          "open is queued behind it, so it is running THERE");

    AetherSDR::test::spinUntil(
        [&] { return Access::lastReceiverDspChannel(backend) >= 0; });
    check(Access::lastReceiverDspChannel(backend) >= 0,
          "and the channel is open once the build thread is free");
}

void aStaleFailureDoesNotCloseTheReuser()
{
    Hl2Backend backend;
    if (!bringUp(backend)) {
        return;
    }

    QStringList pans;
    QObject::connect(&backend, &IRadioBackend::panCenterBandwidthChanged, &backend,
                     [&pans](const QString& id, double, double) {
                         if (!pans.contains(id))
                             pans << id;
                     });
    QObject::connect(&backend, &IRadioBackend::panRemoved, &backend,
                     [&pans](const QString& id) { pans.removeAll(id); });
    int lifecycleFailures = 0;
    QString lastReason;
    QObject::connect(&backend, &IRadioBackend::sliceLifecycleFailed, &backend,
                     [&lifecycleFailures, &lastReason](const QString&, int,
                                                       const QString& why) {
                         ++lifecycleFailures;
                         lastReason = why;
                     });

    ThreadHold hold(Access::buildContext(backend));

    check(backend.createPanadapter(), "the first added receiver is admitted");
    const int firstUi = Access::lastReceiverUi(backend);
    check(firstUi == 1, "the first added receiver took UI 1");
    const QString firstPan = AetherSDR::hl2::hl2PanId(firstUi);
    AetherSDR::hl2::Hl2RxDsp* const firstDsp = Access::receiverDsp(backend, 1);
    check(firstDsp != nullptr, "it has a chain, queued behind the stopped build thread");

    std::atomic<bool> firstDspDestroyed{false};
    if (firstDsp) {
        QObject::connect(firstDsp, &QObject::destroyed,
                         [&firstDspDestroyed] { firstDspDestroyed = true; });
    }

    check(backend.removePanadapter(firstPan),
          "the receiver can be closed while its chain is still building");
    flushDeferredDeletes(Access::wire(backend));
    check(firstDspDestroyed.load(),
          "the closed receiver's chain is destroyed on the I/O thread");

    check(backend.createPanadapter(), "a second receiver is admitted after the close");
    const int secondUi = Access::lastReceiverUi(backend);
    check(secondUi == firstUi,
          "the new receiver is handed the SAME UI number the closed one had");

    hold.release();
    AetherSDR::test::spinUntil([&] {
        return Access::receiverCount(backend) < 2
            || Access::lastReceiverDspChannel(backend) >= 0;
    });

    if (lifecycleFailures > 0) {
        std::fprintf(stderr, "     the operator was told: \"%s\"\n",
                     lastReason.toUtf8().constData());
    }
    check(lifecycleFailures == 0,
          "no create failure is reported against a receiver that never failed");
    check(pans.contains(firstPan),
          "the pan the operator opened a moment ago is still there");
    check(Access::receiverCount(backend) == 2,
          "the re-added receiver survives the closed receiver's completion");

    AetherSDR::test::spinUntil(
        [&] { return Access::lastReceiverDspChannel(backend) >= 0; });
    check(Access::lastReceiverDspChannel(backend) >= 0,
          "and it goes on to get its own WDSP channel");
}

void aStaleSuccessIsNotWrittenOntoTheReuser()
{
    Hl2Backend backend;
    if (!bringUp(backend)) {
        return;
    }

    check(backend.createPanadapter(), "the first added receiver is admitted");
    const int firstUi = Access::lastReceiverUi(backend);
    check(firstUi == 1, "the first added receiver took UI 1");
    AetherSDR::hl2::Hl2RxDsp* const firstDsp = Access::receiverDsp(backend, 1);
    check(firstDsp != nullptr, "it has a chain");

    check(firstDsp && awaitSwapWithoutPumping(firstDsp),
          "the first chain's swap completes on the I/O thread");
    check(Access::dspChannelForUi(backend, firstUi) < 0,
          "its completion is still queued here, not yet delivered");

    ThreadHold hold(Access::buildContext(backend));

    check(backend.removePanadapter(AetherSDR::hl2::hl2PanId(firstUi)),
          "the first added receiver closes");
    check(backend.createPanadapter(), "a second receiver is admitted");
    const int secondUi = Access::lastReceiverUi(backend);
    check(secondUi == firstUi, "and is handed the same UI number");
    check(Access::dspChannelForUi(backend, secondUi) < 0,
          "the new receiver has no WDSP channel of its own — its build is stopped");
    check(Access::buildInFlightForUi(backend, secondUi),
          "and its own build is in flight");

    QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);

    check(Access::dspChannelForUi(backend, secondUi) < 0,
          "the dead chain's WDSP channel id is NOT written onto the receiver "
          "that reused its UI number");
    check(Access::buildInFlightForUi(backend, secondUi),
          "and the stale completion does not clear the in-flight flag that keeps "
          "finishRateChange() off a chain still being built");

    hold.release();
    AetherSDR::test::spinUntil(
        [&] { return Access::dspChannelForUi(backend, secondUi) >= 0; });
    check(Access::dspChannelForUi(backend, secondUi) >= 0,
          "the new receiver's own build still completes normally");
    check(!Access::buildInFlightForUi(backend, secondUi),
          "and clears the in-flight flag itself");
    check(Access::receiverCount(backend) == 2, "both receivers are still running");
}

// Exercise the production reconnect reconstruction, retaining the added UI number.
void aRetiredBuildDoesNotCompleteAgainstReconstructedReceivers(bool alreadyInstalled)
{
    Hl2Backend backend;
    if (!bringUp(backend)) {
        return;
    }
    int lifecycleFailures = 0;
    QObject::connect(&backend, &IRadioBackend::sliceLifecycleFailed, &backend,
                     [&lifecycleFailures](const QString&, int, const QString&) {
                         ++lifecycleFailures;
                     });

    if (alreadyInstalled) {
        check(backend.createPanadapter(), "the pre-reconnect receiver is admitted");
        check(awaitSwapWithoutPumping(Access::receiverDsp(backend, 1)),
              "the old DSP installs before its GUI completion is delivered");
    }
    ThreadHold hold(Access::buildContext(backend));
    if (!alreadyInstalled) {
        check(backend.createPanadapter(), "the pre-reconnect build is queued");
    }
    check(Access::buildInFlightForUi(backend, 1), "the old build identity is pending");
    check(Access::rebuildReceivers(backend, 2),
          "the production reconstruction configures two replacement receivers");
    flushDeferredDeletes(Access::wire(backend));
    const int replacementChannel = Access::dspChannelForUi(backend, 1);
    check(replacementChannel >= 0, "the replacement has its own WDSP channel");
    check(!Access::buildInFlightForUi(backend, 1),
          "reconstruction discards the retiring DSP's build identity");

    hold.release();
    // Drain the build, swap and GUI completion in that order without a timed sleep.
    QMetaObject::invokeMethod(Access::buildContext(backend), [] {}, Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(Access::wire(backend), [] {}, Qt::BlockingQueuedConnection);
    QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);

    check(lifecycleFailures == 0, "a retired build cannot fail the replacement");
    check(Access::receiverCount(backend) == 2, "both reconstructed receivers survive");
    check(Access::dspChannelForUi(backend, 1) == replacementChannel,
          "a retired completion cannot overwrite the replacement channel");
    check(Access::transportIdle(backend), "reconstruction remains socket-free");
}

class Catalogue {
public:
    MeterModel model;

    explicit Catalogue(Hl2Backend& backend)
    {
        QObject::connect(&backend, &IRadioBackend::meterDefined, &model,
                         [this](const MeterDef& def) { model.defineMeter(def); });
        QObject::connect(&backend, &IRadioBackend::meterRemoved, &model,
                         [this](int index) { model.removeMeter(index); });
    }

    int sliceMeter(int uiNumber) const
    {
        return model.findMeter(QStringLiteral("SLC"), QStringLiteral("LEVEL"),
                               uiNumber);
    }
};

void aFailedBuildWithdrawsItsSMeter()
{
    Hl2Backend backend;
    Catalogue catalogue(backend);
    if (!bringUp(backend)) {
        return;
    }

    int lifecycleFailures = 0;
    QObject::connect(&backend, &IRadioBackend::sliceLifecycleFailed, &backend,
                     [&lifecycleFailures](const QString&, int, const QString&) {
                         ++lifecycleFailures;
                     });

    const int survivors = Access::receiverCount(backend);

    ThreadHold hold(Access::buildContext(backend));

    const int goodRate = Access::forceNextBuildToFail(backend);
    check(backend.createPanadapter(), "the receiver is admitted");
    Access::restoreCommittedRate(backend, goodRate);

    const int ui = Access::lastReceiverUi(backend);
    check(ui == 1, "the added receiver took UI 1");

    check(catalogue.sliceMeter(ui) >= 0,
          "the failing receiver's S-meter is in the catalogue before the "
          "failure lands");
    check(catalogue.sliceMeter(ui) != catalogue.sliceMeter(0),
          "and it is its OWN meter, not receiver 0's answering a match-any "
          "lookup");

    hold.release();
    AetherSDR::test::spinUntil([&] { return lifecycleFailures > 0; });

    check(lifecycleFailures == 1,
          "the failed build is reported once as a create failure");
    check(Access::receiverCount(backend) == survivors,
          "and the failed receiver is erased again");

    std::fprintf(stderr,
                 "     after the failure: findMeter(SLC, LEVEL, %d) = %d "
                 "(receiver 0's is %d)\n",
                 ui, catalogue.sliceMeter(ui), catalogue.sliceMeter(0));
    check(catalogue.sliceMeter(ui) < 0,
          "no S-meter is left published for the receiver that vanished");

    check(catalogue.sliceMeter(0) >= 0,
          "and receiver 0's S-meter is untouched");

    check(backend.createPanadapter(), "a later receiver is still admitted");
    AetherSDR::test::spinUntil(
        [&] { return Access::lastReceiverDspChannel(backend) >= 0; });
    check(Access::lastReceiverDspChannel(backend) >= 0,
          "and its build SUCCEEDS -- so the failure above was the injection, "
          "not a fixture that had stopped working");
    check(catalogue.sliceMeter(Access::lastReceiverUi(backend)) >= 0,
          "and that later receiver DOES get an S-meter -- so the absence above "
          "was a withdrawal, not a backend that had stopped declaring them");
}

void aFailedBuildLeavesTheRolesOnALiveReceiver()
{
    Hl2Backend backend;
    if (!bringUp(backend)) {
        return;
    }

    QHash<int, bool> txFlag;
    QHash<int, bool> activeFlag;
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                     [&txFlag, &activeFlag](int id, const SliceDelta& d) {
                         if (d.txSlice)
                             txFlag[id] = *d.txSlice;
                         if (d.active)
                             activeFlag[id] = *d.active;
                     });
    QObject::connect(&backend, &IRadioBackend::sliceRemoved, &backend,
                     [&txFlag, &activeFlag](int id) {
                         txFlag.remove(id);
                         activeFlag.remove(id);
                     });

    int lifecycleFailures = 0;
    QString lastReason;
    QObject::connect(&backend, &IRadioBackend::sliceLifecycleFailed, &backend,
                     [&lifecycleFailures, &lastReason](const QString&, int,
                                                       const QString& why) {
                         ++lifecycleFailures;
                         lastReason = why;
                     });

    const int survivors = Access::receiverCount(backend);
    check(survivors >= 1, "there is a surviving receiver for the roles to fall back to");

    ThreadHold hold(Access::buildContext(backend));

    const int goodRate = Access::forceNextBuildToFail(backend);
    check(backend.createPanadapter(), "the receiver is admitted");
    Access::restoreCommittedRate(backend, goodRate);

    const int ui = Access::lastReceiverUi(backend);
    const int ddc = Access::receiverCount(backend) - 1;
    check(ui == 1, "the added receiver took UI 1");

    backend.setTxSlice(ui);
    backend.setActiveSlice(ui);
    check(Access::txDdc(backend) == ddc,
          "transmit really is on the receiver whose build is about to fail");
    check(Access::activeDdc(backend) == ddc,
          "and so is the active slice");

    hold.release();
    AetherSDR::test::spinUntil([&] { return lifecycleFailures > 0; });

    std::fprintf(stderr, "     the build failed with: \"%s\"\n",
                 lastReason.toUtf8().constData());
    check(lifecycleFailures == 1,
          "the failed build is reported once as a create failure");
    check(Access::receiverCount(backend) == survivors,
          "and the failed receiver is erased again");

    std::fprintf(stderr,
                 "     after the failure: m_txDdc = %d, m_activeDdc = %d, "
                 "%d receiver(s) running\n",
                 Access::txDdc(backend), Access::activeDdc(backend),
                 Access::receiverCount(backend));
    check(Access::roleNamesALiveReceiver(backend, Access::txDdc(backend)),
          "transmit is left on a receiver that EXISTS");
    check(Access::roleNamesALiveReceiver(backend, Access::activeDdc(backend)),
          "the active slice is left on a receiver that EXISTS");

    check(txFlag.values().count(true) == 1,
          "exactly one surviving slice claims transmit in the published model");
    check(activeFlag.values().count(true) == 1,
          "exactly one surviving slice claims to be active");

    check(backend.createPanadapter(), "a later receiver is still admitted");
    AetherSDR::test::spinUntil(
        [&] { return Access::lastReceiverDspChannel(backend) >= 0; });
    check(Access::lastReceiverDspChannel(backend) >= 0,
          "and its build SUCCEEDS — so the failure above was the injection, not "
          "a fixture that had stopped working");
}

}   // namespace

// Swallows the retired chain's DeferredDelete so it is still alive when its
// queued frame is delivered: the producer-identity term, not the QPointer,
// must then reject it. Lives on the chain's thread, as an event filter must.
class KeepAlive : public QObject
{
public:
    bool eventFilter(QObject*, QEvent* event) override
    {
        return event->type() == QEvent::DeferredDelete;
    }
};

// A frame or meter sample its DSP queued before the close must not publish
// under the recycled UI number; the reuser's own must.
void aStaleSpectrumFrameDoesNotPublishOnTheReuser(bool retiredChainAlive)
{
    std::fprintf(stderr, "-- stale spectrum frame and meter sample, retired chain %s at delivery\n",
                 retiredChainAlive ? "still alive" : "deleted");
    Hl2Backend backend;
    AetherSDR::test::SeamThreadAffinityProbe seam(&backend);   // after backend: torn down first
    AetherSDR::test::attachAllSeamSignals(seam);
    if (!bringUp(backend)) {
        return;
    }

    int framesOnUi1 = 0;
    QObject::connect(&backend, &IRadioBackend::spectrumFrameReady, &backend,
                     [&framesOnUi1](int ui, const QByteArray&) {
                         if (ui == 1)
                             ++framesOnUi1;
                     });
    const QString ui1Meter = Access::sliceMeterName(1);
    QList<double> metersOnUi1;
    QObject::connect(&backend, &IRadioBackend::meterUpdate, &backend,
                     [&metersOnUi1, ui1Meter](const QString& id, double value) {
                         if (id == ui1Meter)
                             metersOnUi1 << value;
                     });

    check(backend.createPanadapter(), "a receiver is added");
    check(Access::lastReceiverUi(backend) == 1, "it takes UI 1");
    AetherSDR::test::spinUntil(
        [&] { return Access::dspChannelForUi(backend, 1) >= 0; });
    AetherSDR::hl2::Hl2RxDsp* const oldDsp = Access::receiverDsp(backend, 1);
    check(oldDsp != nullptr, "its chain is built");
    if (!oldDsp) {
        return;
    }

    const std::vector<float> bins(16, -100.0f);
    const auto emitFrom = [&bins](AetherSDR::hl2::Hl2RxDsp* dsp) {
        QMetaObject::invokeMethod(dsp, [dsp, &bins] { emit dsp->spectrumReady(bins); },
                                  Qt::BlockingQueuedConnection);
    };

    // A fresh smoother publishes its first reading at once, so one sample is
    // enough to see a meter on either side of the reopen.
    constexpr float kOldDbfs = -50.0f;
    constexpr float kReuserDbfs = -90.0f;
    const auto meterFrom = [](AetherSDR::hl2::Hl2RxDsp* dsp, float dbfs) {
        QMetaObject::invokeMethod(dsp, [dsp, dbfs] { emit dsp->meterUpdate(dbfs); },
                                  Qt::BlockingQueuedConnection);
    };

    emitFrom(oldDsp);
    meterFrom(oldDsp, kOldDbfs);
    QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    check(framesOnUi1 == 1, "a frame from the current chain publishes on UI 1");
    check(metersOnUi1.size() == 1, "a meter sample from the current chain publishes on UI 1");
    const double oldDbm = metersOnUi1.value(0);

    QPointer<AetherSDR::hl2::Hl2RxDsp> oldGuard(oldDsp);
    KeepAlive* keepAlive = nullptr;
    if (retiredChainAlive) {
        keepAlive = new KeepAlive;
        keepAlive->moveToThread(oldDsp->thread());
        QMetaObject::invokeMethod(oldDsp, [oldDsp, keepAlive] {
            oldDsp->installEventFilter(keepAlive);
        }, Qt::BlockingQueuedConnection);
    }

    // Queued on the GUI thread and not delivered until after the reopen.
    emitFrom(oldDsp);
    meterFrom(oldDsp, kOldDbfs);
    check(backend.removePanadapter(AetherSDR::hl2::hl2PanId(1)), "UI 1 is closed");
    check(backend.createPanadapter(), "a receiver is added again");
    check(Access::lastReceiverUi(backend) == 1, "the reuser is handed UI 1 again");
    flushDeferredDeletes(Access::wire(backend));
    check(oldGuard.isNull() != retiredChainAlive,
          retiredChainAlive ? "the retired chain is still alive at delivery"
                            : "the retired chain is deleted before delivery");
    QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    check(framesOnUi1 == 1,
          "the closed chain's queued frame does not publish on the reused UI 1");
    check(metersOnUi1.size() == 1,
          "the closed chain's queued meter sample does not publish on the reused UI 1");
    if (keepAlive) {
        QMetaObject::invokeMethod(keepAlive, [keepAlive, oldGuard] {
            delete oldGuard.data();   // on its own thread, which owns its WDSP channel
            delete keepAlive;
        }, Qt::BlockingQueuedConnection);
    }

    AetherSDR::test::spinUntil(
        [&] { return Access::dspChannelForUi(backend, 1) >= 0; });
    AetherSDR::hl2::Hl2RxDsp* const newDsp = Access::receiverDsp(backend, 1);
    check(newDsp != nullptr && newDsp != oldDsp, "the reuser has its own chain");
    const int beforeReuserFrame = framesOnUi1;
    const qsizetype beforeReuserMeter = metersOnUi1.size();
    if (newDsp) {
        emitFrom(newDsp);
        meterFrom(newDsp, kReuserDbfs);
        QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    }
    check(framesOnUi1 == beforeReuserFrame + 1, "the reuser's own frame publishes on UI 1");
    // Exactly its own reading: had the stale sample reached this smoother, the
    // reading would be throttled or blended with it.
    const double expectedDbm = oldDbm + (kReuserDbfs - kOldDbfs);
    check(metersOnUi1.size() == beforeReuserMeter + 1
              && std::abs(metersOnUi1.last() - expectedDbm) < 1e-6,
          "the reuser's own meter sample publishes on UI 1, unblended with the stale one");

    const QStringList violations = seam.violations();
    for (const QString& v : violations) {
        std::fprintf(stderr, "     %s\n", v.toUtf8().constData());
    }
    check(violations.isEmpty(), "every seam signal is emitted on the backend's thread");
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-pan-create-async"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot isolate test settings\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    theGuiThreadIsNotHeld();
    aPanSeededInADataModeCarriesTheHeldAgc();
    theBuildRunsOnTheBuildThread();
    aStaleFailureDoesNotCloseTheReuser();
    aStaleSuccessIsNotWrittenOntoTheReuser();
    aRetiredBuildDoesNotCompleteAgainstReconstructedReceivers(false);
    aRetiredBuildDoesNotCompleteAgainstReconstructedReceivers(true);
    aFailedBuildLeavesTheRolesOnALiveReceiver();
    aFailedBuildWithdrawsItsSMeter();
    aStaleSpectrumFrameDoesNotPublishOnTheReuser(false);
    aStaleSpectrumFrameDoesNotPublishOnTheReuser(true);
    std::fprintf(stderr, "hl2_pan_create_async_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
