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
// NO RADIO IS NEEDED. The connect's discovery probe times out on a dead port,
// the session comes up on conservative defaults, and the link edge is injected
// — nothing below asserts anything about the wire.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisClient.h"

#include "TestSettingsProfile.h"
#include "TestDspBuildWait.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

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

}   // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    theGuiThreadIsNotHeld();
    theBuildRunsOnTheBuildThread();
    std::fprintf(stderr, "hl2_pan_create_async_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
