// "ADD PANADAPTER" MUST NOT HOLD THE GUI THREAD, AND MUST NOT HOLD THE I/O ONE.
//
// Hl2Backend::createPanadapter() configured the new receiver's DSP over a
// Qt::BlockingQueuedConnection. Two threads waited on that, not one, and the
// second is the expensive one:
//
//   * the GUI thread, because the call does not return until WDSP has opened a
//     channel and FFTW has planned — the visible half, and the smaller fault;
//
//   * m_ioThread, because that is where Hl2RxDsp LIVES. MetisClient paces EP2
//     from a 2 ms timer on that thread and drains EP6 on it, and iqBlocksReady
//     is a Qt::DirectConnection straight into processIqBlock(). So the open ran
//     ON the thread that feeds the radio: every other receiver's audio stopped
//     for the duration, and so did EP2. docs/HERMES.md §20.8 is explicit that
//     the gateware watchdog answers a gap in EP2 by halting the stream.
//
// #5783 removed exactly this for the pan-bandwidth rebuild and said in its own
// body that createPanadapter() still did it. This is that last site.
//
// ── WHY THE OBVIOUS TEST IS NOT A TEST ───────────────────────────────────
//
// "The panadapter still appears" proves nothing: it passed before the change,
// it passes after it, and it would pass again if someone restored the blocking
// call tomorrow. The assertion has to FAIL when the connection type goes back.
//
// So both cases below work the same way: OCCUPY the thread that must not be
// waited on for a known interval, then measure. Nothing here times WDSP or
// FFTW — the interval is one this test chose, and the margin is against that.
//
//   CASE 1  hold m_ioThread for kHoldMs, then call createPanadapter() and time
//           it. A Qt::BlockingQueuedConnection into an occupied event loop
//           cannot return before that loop is free, so the old code scores
//           >= kHoldMs. The new code posts and returns.
//
//   CASE 2  hold the BUILD thread instead and call createPanadapter(). It must
//           still return at once, and no WDSP channel may be open while that
//           thread is held — which is what proves the open actually goes
//           THERE, rather than the GUI thread having been let go by some
//           other means.
//
// Case 2 is the one that catches a half-fix. A change that merely moved the
// wait from the GUI thread to the I/O thread would pass case 1 and fail case 2,
// because the channel would be open with the build thread still idle.
//
// ── AND ONE ASSERTION POINTING THE OTHER WAY ─────────────────────────────
//
// Case 1 also pins what must NOT become asynchronous. The BUILD moved off the
// GUI thread; the ANNOUNCEMENT did not, and two callers depend on that:
// TciServer diffs m_model->slices() the instant createPanadapter() returns (its
// own comment says "the seam create is SYNCHRONOUS"), and
// MainWindow::createPansSequentially() diffs panadapters() 300 ms later. So
// case 1 requires the pan to be announced while the I/O thread is still held —
// not merely eventually, but before anything on that thread could have run.
//
// ── AND TWO CASES ABOUT IDENTITY, WHICH IS THE OTHER THING THAT MOVED ────
//
// Making the build asynchronous made the COMPLETION arrive at a moment the
// caller does not choose, and a completion has to find the receiver it was
// started for. It resolves by UI NUMBER — and a UI number is REUSED.
// Hl2ReceiverMap::append() hands out the LOWEST FREE one, deliberately (its own
// comment explains that a monotonic counter overran the seam's slice-id space),
// so closing a receiver while its chain builds and opening another puts the
// SAME UI number on a DIFFERENT receiver, with a completion still in flight for
// the first.
//
//   CASE 3  the stale completion FAILED (the chain was closed mid-build, so the
//           swap hop's QPointer was null). Landing it on the reuser tears down a
//           live receiver and tells the operator "the receiver was closed while
//           its chain was building" about a pane they just opened.
//
//   CASE 4  the stale completion SUCCEEDED (the swap won the race against
//           deleteLater). Landing it on the reuser writes the DEAD channel's
//           WDSP id onto it and clears its dspBuildInFlight — while its own
//           build is still running.
//
// BOTH ARE FORCED, NOT HOPED FOR. Nothing below waits a fixed time for a race
// to happen: `ThreadHold` stops a thread on a semaphore and does not return
// until it has actually stopped, and case 4's wait for the first build to land
// polls the I/O thread over BLOCKING invokes precisely because those do not pump
// the GUI event loop — so the completion sits in the GUI queue, undelivered, for
// exactly as long as the test wants it to. A race test that passes because the
// race did not happen is worth nothing; these two cannot pass that way, because
// the interleaving is a consequence of the semaphore and not of the scheduler.
//
// ── AND ONE CASE ABOUT THE OTHER THING AN EARLY ANNOUNCEMENT ALLOWS ──────
//
//   CASE 5  the receiver is announced before its build starts, so it can be
//           SELECTED while the build runs — transmit moved onto it, or the
//           active slice. If the build then FAILS, the erase has to take both
//           roles somewhere that exists. hl2RoleAfterRemove() returns -1 for
//           "the role was the removed receiver" by contract, and storing that
//           leaves rx(m_txDdc) null: no slice owns transmit, and every later
//           key attempt dies in RadioModel's interlock saying so, with nothing
//           from the backend to explain it.
//
// Case 5 is forced the same way cases 3 and 4 are, in both directions at once:
// the build FAILS through Hl2RxDsp::buildChannel()'s own non-positive-rate
// guard rather than a stub, and the roles are ON the doomed receiver because
// the case puts them there through the public API and asserts they landed
// before it releases anything. It also ends on a control — the next build must
// still succeed — so it cannot pass on a fixture that has simply stopped being
// able to build.
//
//   CASE 6  the same forced failure, observed through the S-METER. #5866
//           declares a per-receiver meter in openReceiverDsp(), which succeeds
//           before the build is posted, so a failed build has to withdraw it —
//           a meter left declared renders its last reading forever and goes
//           red nowhere. Asserted through a real MeterModel lookup, with the
//           meter's presence checked before the failure is released.
//
// NO RADIO IS NEEDED. The connect's discovery probe times out on a dead port,
// the session comes up on conservative defaults, and the link edge is injected
// — nothing below asserts anything about the wire.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2Receivers.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisClient.h"
#include "models/MeterModel.h"

#include "TestSettingsProfile.h"
#include "TestDspBuildWait.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QHash>
#include <QSemaphore>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdio>

namespace AetherSDR::hl2 {

// Declared a friend by Hl2Backend. It reaches three things no public API
// exposes and could not: the two threads whose OCCUPANCY is the whole
// experiment, and the link edge that makes a radio-less backend answer
// "connected" — which createPanadapter() refuses without.
struct Hl2PanCreateTestAccess {
    // MetisClient::linkUp is the signal Hl2Backend turns into m_connected.
    //
    // THE BLOCKING INVOKE IS NOT ENOUGH ON ITS OWN, and getting that wrong is
    // what made the first run of this test report "not connected" while
    // createPanadapter() went on to succeed. The blocking call guarantees only
    // that the SIGNAL was emitted on the I/O thread; Hl2Backend's slot for it
    // has GUI-thread affinity, so the connection is queued and m_connected is
    // still false when this returns. Pumping the posted meta-calls is what
    // delivers it — the same pairing Hl2DspReadbackTestAccess::linkDown() uses.
    static void linkUp(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_metis, "linkUp",
                                  Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    }
    // The object that LIVES on the I/O thread. m_ioThread itself does not — a
    // QThread has the affinity of the thread that created it — so this is the
    // handle onto that event loop, exactly as publishIoDspList() says.
    static QObject* wire(Hl2Backend& backend) { return backend.m_metis; }
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
    // THE COMPLETION OBSERVABLE. Hl2Backend records the WDSP channel id in the
    // index map only in finishReceiverDspBuild(), on the success path — so
    // "the last receiver has a channel id" is exactly "its chain finished
    // building", with no timer and no sleep in the assertion.
    static int lastReceiverDspChannel(const Hl2Backend& backend)
    {
        const int ddc = static_cast<int>(backend.m_rx.size()) - 1;
        const Hl2ReceiverIds* ids = backend.m_ids.byDdc(ddc);
        return ids ? ids->dspChannel : -2;
    }
    // The UI number the map just handed out. The identity cases turn on this
    // being REUSED, so they assert it rather than assuming it.
    static int lastReceiverUi(const Hl2Backend& backend)
    {
        const int ddc = static_cast<int>(backend.m_rx.size()) - 1;
        const Hl2ReceiverIds* ids = backend.m_ids.byDdc(ddc);
        return ids ? ids->uiNumber : -1;
    }
    // Resolved the way the COMPLETION resolves — by UI number — so these read
    // what finishReceiverDspBuild() would have written, not what some other
    // index space says.
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
    // The chain object itself, for the ONE thing that cannot be observed from
    // the GUI thread's own state: whether the swap has already happened. It is
    // read over a blocking invoke onto the thread that owns it — see
    // awaitSwapWithoutPumping().
    static Hl2RxDsp* receiverDsp(Hl2Backend& backend, int ddc)
    {
        Hl2Backend::Receiver* r = backend.rx(ddc);
        return r ? r->dsp : nullptr;
    }

    // ── THE TWO ROLES STORED AS DDC INDICES, READ RAW ─────────────────────
    //
    // Straight off the members, with no clamping and no fallback, because
    // "where does transmit point after a failed build" is precisely what case 5
    // asks — and an accessor that tidied -1 into something else would answer a
    // different question and always pass.
    static int txDdc(const Hl2Backend& backend) { return backend.m_txDdc; }
    static int activeDdc(const Hl2Backend& backend) { return backend.m_activeDdc; }

    // Whether a stored role names a receiver that EXISTS — resolved through
    // Hl2Backend's own rx(), so it gets the same answer every production reader
    // of m_txDdc/m_activeDdc gets, rather than a second opinion.
    static bool roleNamesALiveReceiver(Hl2Backend& backend, int role)
    {
        return backend.rx(role) != nullptr;
    }

    // MAKE THE NEXT BUILD FAIL — THROUGH THE CODE'S OWN GUARD, NOT A MOCK.
    //
    // startReceiverDspBuild() takes config.inputSampleRateHz from the rate
    // ledger, and both Hl2RxDsp::configure() and Hl2RxDsp::buildChannel() reject
    // a non-positive input rate on their first line. configure()'s comment names
    // where that comes from in the field: a RadioConnectRequest params override
    // whose "sampleRateHz" is missing or malformed decodes to 0 through
    // QVariant::toInt. So this is the production failure path, entered the
    // production way and carrying the production error string — nothing here
    // substitutes a stub for the thing under test.
    //
    // Returns the previous rate so the caller can put the fixture back.
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

// How long a thread is deliberately occupied for. Long enough that no amount of
// ordinary scheduling noise reaches it, short enough that the test is cheap.
constexpr int kHoldMs = 1000;

// The ceiling on what createPanadapter() itself may spend on the GUI thread.
// It is not a performance budget — the work left there is a std::vector
// push_back, a `new Hl2RxDsp`, six connect() calls and four posts. A third of
// the hold gives that three orders of magnitude of headroom while still being
// unreachable for anything that actually waits on the occupied thread.
constexpr int kReturnBudgetMs = kHoldMs / 3;

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Occupy `target`'s thread for kHoldMs, and do not return until it has actually
// STARTED — otherwise the measurement below could race the hold into place and
// time an unoccupied thread, which is a test that passes for the wrong reason.
void occupyFor(QObject* target, int ms)
{
    QEventLoop started;
    QMetaObject::invokeMethod(target, [&started, ms] {
        QMetaObject::invokeMethod(&started, "quit", Qt::QueuedConnection);
        QThread::msleep(static_cast<unsigned long>(ms));
    }, Qt::QueuedConnection);
    started.exec();
}

// A thread stopped on a SEMAPHORE rather than on a clock, and the difference is
// the whole reason cases 3 and 4 are worth running.
//
// occupyFor() above sleeps for a chosen interval, which is right for a
// MEASUREMENT: it is the yardstick the elapsed time is compared against. It is
// wrong for an ORDERING, because the ordering would then hold only while the
// rest of the test fits inside the interval — on a loaded runner it would stop
// holding, silently, and the case would pass without having reproduced
// anything. This blocks the target thread until release() is called and no
// sooner, so there is no interval to overrun.
//
// The `started` handshake uses QSemaphore::acquire(), NOT a nested QEventLoop —
// and that is load-bearing for case 4, which needs a completion to stay QUEUED
// on this thread. Pumping to learn that another thread has stopped would
// deliver the very event the case is holding back.
class ThreadHold
{
public:
    explicit ThreadHold(QObject* target)
    {
        QMetaObject::invokeMethod(target, [this] {
            m_started.release();
            m_release.acquire();
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
        }
    }
    ~ThreadHold() { release(); }

private:
    QSemaphore m_started;
    QSemaphore m_release;
    bool m_released = false;
};

// Wait until the I/O thread has performed the SWAP for `dsp`, WITHOUT letting
// the GUI thread run its event loop.
//
// finishReceiverDspBuild() is posted to the GUI thread at the end of the swap
// hop, so once the channel id is readable the completion is already queued here
// — and because every wait below is a blocking invoke or a sleep rather than an
// event loop, it stays queued until the caller chooses to spin(). That is what
// makes case 4's interleaving a construction instead of a hope.
//
// Bounded by the same backstop the shared helper uses: an empty FFTW wisdom
// cache genuinely costs tens of seconds on a first open.
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

// Force the pending deleteLater()s on `target`'s thread to run, WITHOUT pumping
// this one.
//
// removePanadapter() retires a chain with deleteLater(), which only posts a
// DeferredDelete event — and a DeferredDelete posted from another thread is not
// guaranteed to be sent by the next turn of the receiving loop. Case 3 needs the
// object actually GONE, because a null QPointer in the swap hop is what makes
// the stale completion the FAILURE variant rather than case 4's success one, so
// it asks the owning thread to drain them and waits for the answer.
//
// A BLOCKING invoke, deliberately: it is what lets this be waited for without
// running an event loop here. Case 3 must not pump between the close and the
// re-add — the backend's connected state is injected rather than fed by a
// radio, and a long pump lets the link time out and makes createPanadapter()
// refuse, which is a property of the fixture and not of the code under test.
void flushDeferredDeletes(QObject* target)
{
    QMetaObject::invokeMethod(target, [] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }, Qt::BlockingQueuedConnection);
}

RadioConnectRequest request()
{
    RadioConnectRequest req;
    req.host = QStringLiteral("127.0.0.1");
    req.port = 1024;   // nothing is on it; the probe times out and defaults apply
    return req;
}

// A connected backend with one receiver running and no radio behind it.
//
// WAIT ON dspSetupFinished, not on the receiver count. m_rx holds one Receiver
// from CONSTRUCTION — its state exists before any radio does (see the
// constructor's note) — so "receiverCount() >= 1" is true before connectRadio()
// is even called and would wait for nothing. dspSetupFinished brackets exactly
// one build, which is what has to be over before the first case starts
// occupying threads: a connect build still running on the I/O thread would be
// indistinguishable from the stall under test.
void bringUp(Hl2Backend& backend)
{
    bool built = false;
    QObject::connect(&backend, &Hl2Backend::dspSetupFinished, &backend,
                     [&built] { built = true; });
    backend.connectRadio(request());
    AetherSDR::test::awaitDspBuild("hl2_pan_create_async_test",
                                   [&built] { return built; });
    check(built, "the connect's DSP build finished");
    // The link edge the wire would have delivered. createPanadapter() refuses
    // before m_connected, and no radio is going to send a first EP6 here.
    Access::linkUp(backend);
    check(backend.isConnected(), "the backend reports connected");
    check(Access::ceiling(backend) > Access::receiverCount(backend),
          "the ceiling leaves room for the receiver this test adds");
}

// ── CASE 1: the GUI thread is not held ───────────────────────────────────
void theGuiThreadIsNotHeld()
{
    TestSettingsProfile profile(
        QStringLiteral("hl2-pan-create-gui"));
    Hl2Backend backend;
    bringUp(backend);

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
    // THE ASSERTION THIS FILE EXISTS FOR. Restore any of the
    // Qt::BlockingQueuedConnection calls this change removed and the measured
    // figure becomes >= kHoldMs, because a blocking invoke cannot outrun the
    // event loop it is waiting on.
    check(elapsedMs < kReturnBudgetMs,
          "createPanadapter() returns without waiting on the occupied I/O thread");

    // AND THE CONTRACT THE CALLERS READ IS UNCHANGED. TciServer diffs
    // m_model->slices() the instant this returns and MainWindow diffs
    // panadapters() 300 ms later; both would break if the announcement had
    // moved to the build's completion. Asserted with the I/O thread STILL
    // occupied, which is the strong form: not merely "eventually", but
    // "before anything on that thread could have run".
    check(announced >= 1,
          "the pan is announced before createPanadapter() returns, with the I/O "
          "thread still held");
    check(Access::receiverCount(backend) == before + 1,
          "the receiver set actually grew");

    // AND THE CHAIN REALLY IS BUILT, eventually. Without this the assertions
    // above would be satisfied by a createPanadapter() that never built one.
    AetherSDR::test::spinUntil(
        [&] { return Access::lastReceiverDspChannel(backend) >= 0; });
    check(Access::lastReceiverDspChannel(backend) >= 0,
          "the new receiver ends up with a real WDSP channel");
}

// ── CASE 2: the build runs on the build thread, not on either of the others ──
void theBuildRunsOnTheBuildThread()
{
    TestSettingsProfile profile(
        QStringLiteral("hl2-pan-create-build"));
    Hl2Backend backend;
    bringUp(backend);

    occupyFor(Access::buildContext(backend), kHoldMs);

    QElapsedTimer clock;
    clock.start();
    const bool admitted = backend.createPanadapter();
    const qint64 elapsedMs = clock.elapsed();
    check(admitted, "createPanadapter() admits the receiver");
    check(elapsedMs < kReturnBudgetMs,
          "createPanadapter() does not wait on the build thread either");

    // Give the GUI thread plenty of turns while the build thread is still held.
    // If the WDSP open were running anywhere but the build thread, it would
    // have completed by now and the channel id would be recorded — the build
    // thread being busy would be irrelevant to it.
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


// ── CASE 3: a stale FAILED completion must not close the receiver that
//            inherited its UI number ─────────────────────────────────────────
//
// THE SEQUENCE, AND WHY IT IS A SEQUENCE AND NOT A RACE. The build thread is
// stopped on a semaphore for the whole of it, so the first receiver's chain
// cannot possibly finish while it is being closed; the close's deleteLater() is
// then WAITED FOR, so the swap hop's QPointer is certainly null and the
// completion is certainly a failure. Only then is the thread released. Nothing
// here depends on how fast anything runs.
void aStaleFailureDoesNotCloseTheReuser()
{
    TestSettingsProfile profile(QStringLiteral("hl2-pan-create-stale-failure"));
    Hl2Backend backend;
    bringUp(backend);

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

    // Observed with a DIRECT connection into a std::atomic, not a queued one into
    // a bool: a queued connection would need this thread to run an event loop to
    // learn the answer, and this case must not run one until the re-add is done.
    std::atomic<bool> firstDspDestroyed{false};
    if (firstDsp) {
        QObject::connect(firstDsp, &QObject::destroyed,
                         [&firstDspDestroyed] { firstDspDestroyed = true; });
    }

    // CLOSE IT WHILE IT IS STILL BUILDING — the ordinary operator move the
    // reviewer names: close a pane, then add one.
    check(backend.removePanadapter(firstPan),
          "the receiver can be closed while its chain is still building");
    // And make the retirement REAL before going on. removePanadapter() only
    // posts the deletion; the swap hop reads a QPointer to the object, so the
    // object being gone is what pins this case to the FAILURE completion rather
    // than to case 4's success one.
    flushDeferredDeletes(Access::wire(backend));
    check(firstDspDestroyed.load(),
          "the closed receiver's chain is destroyed on the I/O thread");

    // AND THE UI NUMBER COMES STRAIGHT BACK, which is the premise the
    // completion's own header missed: Hl2ReceiverMap::append() allocates the
    // LOWEST FREE UI number, deliberately, so a retired one is reissued at once.
    check(backend.createPanadapter(), "a second receiver is admitted after the close");
    const int secondUi = Access::lastReceiverUi(backend);
    check(secondUi == firstUi,
          "the new receiver is handed the SAME UI number the closed one had");

    // Both builds may now run. The closed receiver's completion was posted
    // first, and it carries ok=false.
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

// ── CASE 4: a stale SUCCEEDED completion must not be written onto the
//            receiver that inherited its UI number ──────────────────────────
//
// The other half of the same defect, and the one that leaves no error message
// behind: if the swap wins the race against deleteLater(), the completion is a
// SUCCESS, and landing it on the reuser records a DEAD WDSP channel id against
// a receiver whose own chain does not exist yet — then publishes it.
//
// FORCED, NOT TIMED. The completion is parked in this thread's event queue by
// never running an event loop: awaitSwapWithoutPumping() polls over blocking
// invokes, ThreadHold hands off over a semaphore, and removePanadapter() /
// createPanadapter() contain no event loop of their own. It is then delivered
// by sendPostedEvents(), which involves no interval at all.
void aStaleSuccessIsNotWrittenOntoTheReuser()
{
    TestSettingsProfile profile(QStringLiteral("hl2-pan-create-stale-success"));
    Hl2Backend backend;
    bringUp(backend);

    check(backend.createPanadapter(), "the first added receiver is admitted");
    const int firstUi = Access::lastReceiverUi(backend);
    check(firstUi == 1, "the first added receiver took UI 1");
    AetherSDR::hl2::Hl2RxDsp* const firstDsp = Access::receiverDsp(backend, 1);
    check(firstDsp != nullptr, "it has a chain");

    // LET THE SWAP LAND, BUT NOT THE COMPLETION. After this returns the chain is
    // open on the I/O thread and finishReceiverDspBuild() is queued on THIS
    // thread, undelivered — because nothing above or below pumps until the
    // sendPostedEvents() further down.
    check(firstDsp && awaitSwapWithoutPumping(firstDsp),
          "the first chain's swap completes on the I/O thread");
    check(Access::dspChannelForUi(backend, firstUi) < 0,
          "its completion is still queued here, not yet delivered");

    // Stop the build thread BEFORE the second receiver exists, so the second
    // receiver's own chain cannot be built. That is what makes the assertion
    // below unambiguous: any channel id it reports can only have come from the
    // first receiver's completion.
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

    // DELIVER THE STALE COMPLETION. No interval: the event is already in this
    // object's queue, and this is the same pairing Access::linkUp() uses.
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

// THE METER CATALOGUE AS A CONSUMER HOLDS IT.
//
// RadioModel's own two connections, reproduced rather than referenced so this
// test needs no GUI: meterDefined -> defineMeter, meterRemoved -> removeMeter.
// Every assertion below is a MeterModel::findMeter() -- the same lookup the
// meter list and the S-meter widget go through -- so what is asserted is the
// CONSEQUENCE a consumer can see, not a call. There is no spy on
// withdrawSliceLevelMeter() here, and deliberately so: a spy would still pass
// if the withdrawal named the wrong meter.
//
// disconnected -> clear() is NOT wired, because it must not be: that wholesale
// wipe is RadioModel::onDisconnected()'s backstop, it is what hides this class
// of defect on the ordinary disconnect, and it does not run on the path below.
// Modelling it here would hide the defect from this test too.
class Catalogue {
public:
    MeterModel model;

    explicit Catalogue(Hl2Backend& backend)
    {
        QObject::connect(&backend, &IRadioBackend::meterDefined, &backend,
                         [this](const MeterDef& def) { model.defineMeter(def); });
        QObject::connect(&backend, &IRadioBackend::meterRemoved, &backend,
                         [this](int index) { model.removeMeter(index); });
    }

    // The consumer's own lookup. The explicit sourceIndex is NOT optional:
    // findMeter() treats a negative index as MATCH-ANY and would answer with
    // receiver 0's meter for every receiver, turning a real absence into a
    // false present -- and this case would then pass with the fix reverted.
    int sliceMeter(int uiNumber) const
    {
        return model.findMeter(QStringLiteral("SLC"), QStringLiteral("LEVEL"),
                               uiNumber);
    }
};

// ── CASE 6: a FAILED build must not leave the receiver's S-METER behind ─────
//
// THE INTERACTION THIS CATCHES, AND WHY NEITHER PR SHOWS IT ALONE. #5866 put
// withdrawSliceLevelMeter() on "every path that drops a receiver", one of which
// was createPanadapter()'s INLINE failure path. This PR MOVED that whole path
// into finishReceiverDspBuild(), from a base that predates #5866. Because the
// path moved rather than changed, the merge raises only a textual conflict on
// the shared qCWarning line -- so resolving it by taking either side wholesale
// drops the withdrawal, and nothing marks its absence. Reading either PR on its
// own shows nothing wrong; it took an integration build of both to surface it.
//
// THE METER IS GENUINELY OUTSTANDING AT THAT POINT. openReceiverDsp() declares
// it with defineSliceLevelMeter(ui) as its LAST act and SUCCEEDS -- what fails
// is the build startReceiverDspBuild() posts afterwards -- so a failed async
// build leaves a defined S-meter published for a receiver that does not exist.
// That is a shape nothing goes red for on its own: a meter left declared does
// not throw, does not warn and does not stop rendering. It renders the last
// value it ever received, forever.
//
// FORCED, NOT HOPED FOR, exactly as case 5 is: the build fails through
// Hl2RxDsp::buildChannel()'s own non-positive-rate guard, reached the way a
// malformed params override reaches it in the field. The meter's PRESENCE is
// asserted before the failure is released, so an absence afterwards is a
// withdrawal and not a meter that was never declared -- without that
// precondition this case would pass on a backend that had simply stopped
// defining meters at all. And it ends on the same control case 5 uses.
void aFailedBuildWithdrawsItsSMeter()
{
    TestSettingsProfile profile(QStringLiteral("hl2-pan-create-failed-smeter"));
    Hl2Backend backend;
    Catalogue catalogue(backend);
    bringUp(backend);

    int lifecycleFailures = 0;
    QObject::connect(&backend, &IRadioBackend::sliceLifecycleFailed, &backend,
                     [&lifecycleFailures](const QString&, int, const QString&) {
                         ++lifecycleFailures;
                     });

    const int survivors = Access::receiverCount(backend);

    // Hold the build thread so the receiver is announced -- and its meter
    // declared -- while its chain is provably not built. The precondition below
    // is checked inside that window, on a semaphore rather than a timer.
    ThreadHold hold(Access::buildContext(backend));

    const int goodRate = Access::forceNextBuildToFail(backend);
    check(backend.createPanadapter(), "the receiver is admitted");
    Access::restoreCommittedRate(backend, goodRate);

    const int ui = Access::lastReceiverUi(backend);
    check(ui == 1, "the added receiver took UI 1");

    // ── THE PRECONDITION ──────────────────────────────────────────────────
    //
    // The meter really was declared, and it is THIS receiver's rather than
    // receiver 0's. Without this the assertion after the failure would be
    // satisfied by a meter that never existed.
    check(catalogue.sliceMeter(ui) >= 0,
          "the failing receiver's S-meter is in the catalogue before the "
          "failure lands");
    check(catalogue.sliceMeter(ui) != catalogue.sliceMeter(0),
          "and it is its OWN meter, not receiver 0's answering a match-any "
          "lookup");

    // LET THE FAILURE LAND.
    hold.release();
    AetherSDR::test::spinUntil([&] { return lifecycleFailures > 0; });

    // AND THE BUILD REALLY DID FAIL -- the condition under test, not an
    // incidental.
    check(lifecycleFailures == 1,
          "the failed build is reported once as a create failure");
    check(Access::receiverCount(backend) == survivors,
          "and the failed receiver is erased again");

    // ── THE ASSERTION THIS CASE EXISTS FOR ────────────────────────────────
    //
    // A meter this lookup can still find is a meter a consumer can still see.
    std::fprintf(stderr,
                 "     after the failure: findMeter(SLC, LEVEL, %d) = %d "
                 "(receiver 0's is %d)\n",
                 ui, catalogue.sliceMeter(ui), catalogue.sliceMeter(0));
    check(catalogue.sliceMeter(ui) < 0,
          "no S-meter is left published for the receiver that vanished");

    // AND THE WITHDRAWAL TOOK ONLY ITS OWN. A withdrawal that removed the wrong
    // index would satisfy the assertion above while breaking the receiver that
    // survived, so the survivor's meter is checked too.
    check(catalogue.sliceMeter(0) >= 0,
          "and receiver 0's S-meter is untouched");

    // THE CONTROL, as in case 5: prove the injection was scoped to one build.
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

// ── CASE 5: a FAILED build must not leave transmit or the active slice
//            pointing at nothing ────────────────────────────────────────────
//
// THE DEFECT THIS CHANGE INTRODUCED, AND THE REASON IT IS NEW. Publishing the
// slice BEFORE the build starts is the point of this PR — it is what stops
// TciServer and MainWindow diffing a model the new receiver is not in yet — and
// it also makes that receiver SELECTABLE for the whole length of its build.
// setTxSlice() and setActiveSlice() need only ddcForSlice() and rx(); neither
// reads r->dsp, so both will put a role on a receiver whose chain does not
// exist. Before this PR they could not: the receiver was invisible outside
// Hl2Backend until its chain was open, which is exactly what the old comment on
// the failure path was relying on when it said the `==` case "cannot arise
// here".
//
// WHAT WENT WRONG THEN. finishReceiverDspBuild()'s failure path erases the
// receiver and moved both roles with hl2RoleAfterRemove() alone. That helper
// answers "the role WAS the removed receiver" with -1 BY CONTRACT, so the
// caller can choose a new home. Nothing chose one. rx(-1) is nullptr, so
// transmit owned no slice at all: every later key attempt dies in RadioModel's
// interlock with "No transmit slice is assigned", and nothing down here says
// why — the silent refusal removePanadapter() documents at length and moves
// transmit to DDC 0 to avoid.
//
// FORCED IN BOTH HALVES, WHICH IS WHAT MAKES THIS WORTH RUNNING:
//
//   * the build FAILS because the committed rate is 0 when the Config is
//     snapshotted — the guard on the first line of Hl2RxDsp::buildChannel(),
//     reached the way a malformed params override reaches it. Not a stub, not a
//     timing window, and not something that might not happen.
//   * the roles are ON the failing receiver because this case puts them there
//     through the public API and then ASSERTS they landed, before releasing
//     anything.
//
// A version of this case that reached neither would pass while proving nothing,
// so the two preconditions are checked, the failure itself is checked, and the
// case ends on a control that would catch a fixture which had simply stopped
// being able to build.
void aFailedBuildLeavesTheRolesOnALiveReceiver()
{
    TestSettingsProfile profile(QStringLiteral("hl2-pan-create-failed-roles"));
    Hl2Backend backend;
    bringUp(backend);

    // THE PUBLISHED MODEL, KEPT THE WAY A CLIENT KEEPS IT: last value wins per
    // slice, and a retired slice stops counting. This is not a second copy of
    // m_txDdc — it is what RadioModel's interlock actually reads, so it is the
    // only form in which "transmit has a home" is observable from outside.
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

    // STOP THE BUILD THREAD FIRST, so the receiver added below is announced and
    // can be selected while its chain is provably not built. That window is the
    // one this PR opened; holding it open on a semaphore rather than a timer is
    // what stops this case depending on the scheduler.
    ThreadHold hold(Access::buildContext(backend));

    const int goodRate = Access::forceNextBuildToFail(backend);
    check(backend.createPanadapter(), "the receiver is admitted");
    // PUT THE FIXTURE BACK AT ONCE. The Config was snapshotted inside
    // startReceiverDspBuild() before anything was posted, so this does not
    // rescue the build already in flight — and it leaves the backend otherwise
    // normal, which is what lets the control at the end of this case mean
    // something.
    Access::restoreCommittedRate(backend, goodRate);

    const int ui = Access::lastReceiverUi(backend);
    const int ddc = Access::receiverCount(backend) - 1;
    check(ui == 1, "the added receiver took UI 1");

    // THE PRECONDITION, PUT THERE DELIBERATELY AND THEN CHECKED. This is an
    // operator clicking the pane that has just appeared and pressing it into
    // service as the transmit slice — which createPanadapter()'s announcement
    // note explicitly invites, since the whole reason the pan and slice are
    // emitted early is that callers act on them immediately.
    backend.setTxSlice(ui);
    backend.setActiveSlice(ui);
    check(Access::txDdc(backend) == ddc,
          "transmit really is on the receiver whose build is about to fail");
    check(Access::activeDdc(backend) == ddc,
          "and so is the active slice");

    // LET THE FAILURE LAND.
    hold.release();
    AetherSDR::test::spinUntil([&] { return lifecycleFailures > 0; });

    // AND THE BUILD REALLY DID FAIL. Without this every assertion below would
    // be satisfied by a build that quietly SUCCEEDED and removed nothing: the
    // failure is the condition under test, not an incidental.
    std::fprintf(stderr, "     the build failed with: \"%s\"\n",
                 lastReason.toUtf8().constData());
    check(lifecycleFailures == 1,
          "the failed build is reported once as a create failure");
    check(Access::receiverCount(backend) == survivors,
          "and the failed receiver is erased again");

    // ── THE ASSERTION THIS CASE EXISTS FOR ────────────────────────────────
    //
    // Read off the real members, resolved through the real rx(). With
    // hl2RoleAfterRemove()'s -1 stored, both of these are nullptr and both fail.
    std::fprintf(stderr,
                 "     after the failure: m_txDdc = %d, m_activeDdc = %d, "
                 "%d receiver(s) running\n",
                 Access::txDdc(backend), Access::activeDdc(backend),
                 Access::receiverCount(backend));
    check(Access::roleNamesALiveReceiver(backend, Access::txDdc(backend)),
          "transmit is left on a receiver that EXISTS");
    check(Access::roleNamesALiveReceiver(backend, Access::activeDdc(backend)),
          "the active slice is left on a receiver that EXISTS");

    // AND THE CLIENT WAS TOLD. A role that moves without being republished
    // leaves every surviving slice still saying "not me", which is
    // indistinguishable from having no transmit slice at all — the interlock
    // reads the flag, not the member.
    check(txFlag.values().count(true) == 1,
          "exactly one surviving slice claims transmit in the published model");
    check(activeFlag.values().count(true) == 1,
          "exactly one surviving slice claims to be active");

    // THE CONTROL. Everything above would also pass on a backend that had
    // simply stopped being able to build anything, so prove it can: the
    // injection was scoped to one build and the next one goes through.
    check(backend.createPanadapter(), "a later receiver is still admitted");
    AetherSDR::test::spinUntil(
        [&] { return Access::lastReceiverDspChannel(backend) >= 0; });
    check(Access::lastReceiverDspChannel(backend) >= 0,
          "and its build SUCCEEDS — so the failure above was the injection, not "
          "a fixture that had stopped working");
}

}   // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    theGuiThreadIsNotHeld();
    theBuildRunsOnTheBuildThread();
    aStaleFailureDoesNotCloseTheReuser();
    aStaleSuccessIsNotWrittenOntoTheReuser();
    aFailedBuildLeavesTheRolesOnALiveReceiver();
    aFailedBuildWithdrawsItsSMeter();
    std::fprintf(stderr, "hl2_pan_create_async_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
