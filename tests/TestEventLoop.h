#pragma once

// Waiting on the event loop in a test, done once.
//
// Four test files had grown their own copy of this function under three names
// (waitUntil, spinUntil, waitFor), and #4693 was a fifth spelling that got the
// semantics wrong. This header is the one correct version, so the next test
// that needs to wait for something inherits it rather than re-deriving it.
//
// ---------------------------------------------------------------------------
// The trap this exists to prevent
// ---------------------------------------------------------------------------
//
// QCoreApplication::processEvents(flags, ms) does NOT wait for ms. The
// implementation strips WaitForMoreEvents, so on an empty queue it returns
// immediately; that argument is a ceiling on how long it will keep processing
// if there IS work, not a floor on how long it blocks. A loop bounded by an
// ITERATION COUNT therefore waits approximately zero:
//
//     for (int i = 0; i < 200 && spy.count() == 0; ++i)          // BROKEN
//         QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
//
// Measured on Qt 6.10.3 against a worker-thread signal fired after N ms: that
// loop exited after 0 ms and MISSED the signal at N = 5, 50, 200, 1000 and
// 3000. It reads as a 2-second budget and is a zero-second one. It passes only
// when the emission happens to be queued already, which is why it failed
// intermittently on a loaded CI box and never on an idle desktop (#4693).
//
// A loop bounded by a DEADLINE does not have this problem, even without a
// yield: the wall-clock condition keeps re-entering processEvents() until the
// deadline expires, so it does eventually observe the flag. Measured on the
// same probe, a deadline loop with no yield caught the signal at every N tried
// and missed 0/25 under 14-way CPU contention. It busy-spins where waitFor()
// blocks, but it is not incorrect — the existing deadline-bounded helpers in
// this suite were never broken, and only the iteration-count spelling was.
//
// ---------------------------------------------------------------------------
// Which one to use
// ---------------------------------------------------------------------------
//
//   waitFor(predicate)  — waiting for a CONDITION to become true. Blocks on a
//                         real nested event loop; returns as soon as the
//                         predicate holds. Prefer this.
//
//   waitForSignal(spy)  — waiting for a SIGNAL to arrive. Same thing, phrased
//                         for a QSignalSpy, and it handles the already-emitted
//                         case that QSignalSpy::wait() alone would miss.
//
//   pumpFor(ms)         — deliberately burning a wall-clock window while the
//                         event loop runs, because the thing under test IS the
//                         passage of time (a watchdog firing, an animation
//                         settling, a rate measured over an interval). This is
//                         not a condition wait and must not be replaced by one.
//
// A test that reaches for pumpFor() when it means waitFor() is slow and
// timing-fragile; one that reaches for waitFor() when it means pumpFor() is
// asserting on a window it never actually waited out.
//
// ---------------------------------------------------------------------------
// Two things that bite when MIGRATING an existing spin to waitFor()
// ---------------------------------------------------------------------------
//
// 1. No trailing drain. Several of the hand-rolled spins this replaced ran an
//    unconditional processEvents() AFTER their predicate held. waitFor() returns
//    the instant the predicate is observed — and returns having processed
//    nothing at all when it is already true on entry. A call site that waits on
//    one condition and then asserts on state that settles an event hop later
//    silently loses that hop. This is not theoretical: tci_automation_test lost
//    4 runs in 360 under load to it, against 0/360 for its pre-migration self,
//    on an assertion about state one signal downstream of the awaited condition
//    (it did not reproduce in a further 400 runs, so the rate is low — but the
//    mechanism is not in doubt). Both affected call sites keep an explicit
//    drain; see the comment on tci_automation_test's spinUntil(). Prefer waiting
//    on the condition you actually assert on — the drain is compatibility with
//    the old spins, not a design to copy into new tests.
//
// 2. deleteLater() now runs. A nested QEventLoop and a top-level processEvents()
//    spin do not treat DeferredDelete alike: Qt's allowDeferredDelete includes
//    (!eventLevel && loopLevel > 0), so an object deleteLater()'d at loop level
//    zero — where these tests issue it — is collected INSIDE waitFor(), where
//    the old spin never collected it at all. Measured on Qt 6.11.1: COLLECTED
//    under the nested loop, STRANDED under the spin. That is an improvement, but
//    it changes object lifetime: something that used to survive a wait is now
//    destroyed during it, so a raw pointer held across a waitFor() can dangle
//    where it previously did not.

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

#include <algorithm>

// QSignalSpy lives in Qt6::Test, which several consumers of this header do not
// link (tci_automation_test and ax25_link_timing_test are Qt6::Core only).
// waitFor()/pumpFor() need nothing beyond Core, so the spy overload is compiled
// only where the type is actually available rather than forcing every caller to
// take a Qt Test dependency for it.
#if __has_include(<QSignalSpy>)
#  include <QSignalSpy>
#  define AETHER_TEST_HAVE_SIGNAL_SPY 1
#endif

namespace AetherTest {

// Run the event loop until `predicate` returns true, or `timeoutMs` elapses.
// Returns the predicate's final value, so `if (!waitFor(...)) fail()` reads
// correctly and a timeout is never silently mistaken for success.
//
// The nested QEventLoop is what makes this a wait rather than a poll: it blocks
// the calling thread instead of burning CPU on an empty queue. Latency is one
// tick (10 ms), not zero — the predicate is evaluated on the tick rather than
// on delivery, because a predicate can be satisfied by something that posts no
// event to this thread at all.
template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs = 5000)
{
    if (predicate())
        return true;

    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        QEventLoop loop;
        // The tick is load-bearing, not a safety net: a worker thread setting an
        // atomic (or a timer owned elsewhere) wakes nothing here, so a bare
        // exec() would sleep until the deadline. Mutation-checked — remove the
        // periodic re-check and the helper sits out the full timeout instead of
        // returning when the predicate holds.
        QTimer tick;
        tick.setInterval(
            static_cast<int>(std::min<qint64>(10, deadline.remainingTime() + 1)));
        QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
            if (predicate() || deadline.hasExpired())
                loop.quit();
        });
        tick.start();
        loop.exec(QEventLoop::AllEvents);

        if (predicate())
            return true;
    }
    return predicate();
}

#ifdef AETHER_TEST_HAVE_SIGNAL_SPY
// Wait for `spy` to hold at least `count` emissions.
//
// count() is checked BEFORE waiting because QSignalSpy::wait() waits for the
// NEXT emission — on a signal that has already been delivered it would block
// until the timeout and then report failure for something that did happen.
//
// Available only where the target links Qt6::Test; see the include guard above.
inline bool waitForSignal(QSignalSpy& spy, int count = 1, int timeoutMs = 5000)
{
    return waitFor([&spy, count] { return spy.count() >= count; }, timeoutMs);
}
#endif

// Pump the event loop for a fixed wall-clock window.
//
// For tests whose subject IS elapsed time — a watchdog that must fire after
// 500 ms, an animation that must settle, a rate averaged over an interval.
// Unlike waitFor() this has no early exit: it is supposed to take the full
// duration. Do not "optimise" it into a condition wait.
inline void pumpFor(int milliseconds)
{
    QDeadlineTimer deadline(milliseconds);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(
            QEventLoop::AllEvents,
            static_cast<int>(std::min<qint64>(10, deadline.remainingTime() + 1)));
        // Yield rather than busy-spinning a whole core for the window. The
        // contract here is "burn wall-clock time while events flow", and on an
        // empty queue processEvents() returns instantly — without this the loop
        // pegs a core, which on a loaded CI box steals the scheduling the code
        // under test may itself be waiting for.
        if (!deadline.hasExpired())
            QThread::msleep(1);
    }
    // A final drain so events posted by the last timer to fire inside the
    // window are delivered before the caller asserts on their effect.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

} // namespace AetherTest
