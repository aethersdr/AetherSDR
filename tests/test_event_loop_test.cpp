// TestEventLoop.h is test infrastructure that makes correctness claims, so it
// carries its own proof. The load-bearing claim is that these helpers observe
// a CROSS-THREAD event, which is exactly what the iteration-count idiom in
// #4693 failed to do — so every case here arms its trigger on a worker thread.
//
// The negative case matters most: brokenSpin() is the #4693 idiom preserved
// verbatim. If it ever stops failing, this file's other assertions stop meaning
// anything, because the trap would no longer be a trap.

#include "TestEventLoop.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdio>

namespace {

int g_failures = 0;

void check(bool ok, const char* what, const QString& detail = QString())
{
    std::fprintf(stderr, "%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", what,
                 qPrintable(detail));
    if (!ok)
        ++g_failures;
}

// Fires on a worker thread after a delay: both a signal and a plain atomic, so
// the predicate form is exercised against something that posts NO event to the
// main thread (the case a bare nested loop would sleep straight through).
//
// The signal is QTimer::timeout from a single-shot timer living on the worker,
// rather than a hand-declared one — no test in this suite declares Q_OBJECT in
// a .cpp, and borrowing a real cross-thread signal avoids being the first.
class Trigger : public QObject
{
public:
    std::atomic_bool flag{false};

    // Owned by the worker thread; QSignalSpy connects to its timeout across
    // threads, which is exactly the delivery path under test.
    QTimer* armAfter(int ms)
    {
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(ms);
        QObject::connect(timer, &QTimer::timeout, this, [this] { flag.store(true); });
        return timer;
    }
};

// The #4693 idiom, kept verbatim as the control.
template <typename Predicate>
bool brokenSpin(Predicate predicate)
{
    for (int i = 0; i < 200 && !predicate(); ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

struct Rig {
    QThread thread;
    Trigger trigger;

    Rig()
    {
        trigger.moveToThread(&thread);
        thread.start();
    }
    ~Rig()
    {
        // Stop and destroy the timers on the thread that owns them, BEFORE the
        // thread stops. Letting ~Trigger() run on the main thread instead would
        // delete worker-affinity QObjects with live timers from the wrong
        // thread — Qt warns ("Timers cannot be stopped from another thread") and
        // the teardown is genuinely unsafe. This file is the template the next
        // threaded test will copy, so it should model the correct shape.
        QMetaObject::invokeMethod(&trigger, [this] {
            for (QObject* child : trigger.children())
                if (auto* t = qobject_cast<QTimer*>(child))
                    t->stop();
            qDeleteAll(trigger.children());
        }, Qt::BlockingQueuedConnection);

        thread.quit();
        thread.wait();
    }

    // Creates the timer ON THE WORKER (blocking, so it exists before we return)
    // and hands it back so a QSignalSpy can attach before anything starts it.
    // Worker affinity is the point: the emission has to cross threads to be the
    // delivery path these helpers exist to observe.
    QTimer* armAfter(int ms)
    {
        QTimer* timer = nullptr;
        QMetaObject::invokeMethod(
            &trigger, [this, ms] { return trigger.armAfter(ms); },
            Qt::BlockingQueuedConnection, &timer);
        return timer;
    }

    void start(QTimer* timer)
    {
        QMetaObject::invokeMethod(timer, [timer] { timer->start(); });
    }

    // Arm and start in one step, for the cases that don't need a spy attached
    // in between.
    void fireAfter(int ms) { start(armAfter(ms)); }
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- waitFor() observes a worker-thread atomic ------------------------
    {
        Rig rig;
        rig.fireAfter(120);
        QElapsedTimer t;
        t.start();
        const bool caught =
            AetherTest::waitFor([&rig] { return rig.trigger.flag.load(); }, 5000);
        const qint64 elapsed = t.elapsed();
        check(caught, "waitFor: catches a flag set on a worker thread");
        // Returned promptly rather than sitting out the timeout: proves it woke
        // on the change, not on the deadline.
        check(elapsed < 1000, "waitFor: returns when the predicate holds, not at timeout",
              QStringLiteral("%1 ms").arg(elapsed));
    }

    // ---- ...where the broken idiom does not --------------------------------
    // This is the assertion that keeps the header honest. Same trigger, same
    // delay, the idiom #4693 replaced.
    {
        Rig rig;
        rig.fireAfter(120);
        QElapsedTimer t;
        t.start();
        const bool caught = brokenSpin([&rig] { return rig.trigger.flag.load(); });
        const qint64 elapsed = t.elapsed();
        check(!caught,
              "control: the iteration-count spin MISSES it (this is the #4693 bug)",
              QStringLiteral("caught=%1 after %2 ms").arg(caught).arg(elapsed));
        check(elapsed < 100,
              "control: ...and exits early rather than waiting its stated budget",
              QStringLiteral("%1 ms").arg(elapsed));
    }

    // ---- waitFor() reports failure on a predicate that never holds ---------
    // A wait helper that returns true on timeout would turn every future
    // timeout into a silent pass.
    {
        QElapsedTimer t;
        t.start();
        const bool caught = AetherTest::waitFor([] { return false; }, 300);
        const qint64 elapsed = t.elapsed();
        check(!caught, "waitFor: returns false when the predicate never holds");
        check(elapsed >= 250,
              "waitFor: ...after actually waiting out the timeout",
              QStringLiteral("%1 ms").arg(elapsed));
    }

    // ---- waitFor() is immediate when already satisfied ---------------------
    {
        QElapsedTimer t;
        t.start();
        const bool caught = AetherTest::waitFor([] { return true; }, 5000);
        check(caught && t.elapsed() < 50,
              "waitFor: an already-true predicate returns without waiting",
              QStringLiteral("%1 ms").arg(t.elapsed()));
    }

    // ---- waitForSignal() on a queued cross-thread signal -------------------
    {
        Rig rig;
        QTimer* timer = rig.armAfter(120);
        QSignalSpy spy(timer, &QTimer::timeout);
        check(spy.isValid(), "waitForSignal: the spy is connectable");
        rig.start(timer);
        check(AetherTest::waitForSignal(spy),
              "waitForSignal: catches a queued cross-thread emission");
    }

    // ---- waitForSignal() on a signal already delivered ---------------------
    // QSignalSpy::wait() alone waits for the NEXT emission and would block
    // until timeout here, reporting failure for something that did happen.
    {
        Rig rig;
        QTimer* timer = rig.armAfter(0);
        QSignalSpy spy(timer, &QTimer::timeout);
        rig.start(timer);
        check(AetherTest::waitForSignal(spy), "waitForSignal: first emission arrives");

        QElapsedTimer t;
        t.start();
        const bool again = AetherTest::waitForSignal(spy, 1, 2000);
        check(again && t.elapsed() < 100,
              "waitForSignal: an ALREADY-delivered emission returns immediately",
              QStringLiteral("%1 ms").arg(t.elapsed()));
    }

    // ---- waitForSignal() counts ------------------------------------------
    {
        // One repeating timer on the worker, so two emissions reach the same spy.
        Rig rig;
        QTimer* timer = rig.armAfter(40);
        QMetaObject::invokeMethod(timer, [timer] { timer->setSingleShot(false); },
                                  Qt::BlockingQueuedConnection);
        QSignalSpy spy(timer, &QTimer::timeout);
        rig.start(timer);
        check(AetherTest::waitForSignal(spy, 2), "waitForSignal: waits for the Nth emission");
        check(spy.count() >= 2, "waitForSignal: ...and the spy really holds them",
              QStringLiteral("count=%1").arg(spy.count()));
    }

    // ---- pumpFor() burns the whole window ---------------------------------
    // The property that makes it usable for watchdog and animation tests: no
    // early exit, so an assertion after it is genuinely past the window.
    {
        QElapsedTimer t;
        t.start();
        AetherTest::pumpFor(300);
        const qint64 elapsed = t.elapsed();
        check(elapsed >= 290, "pumpFor: waits the full window (no early exit)",
              QStringLiteral("%1 ms").arg(elapsed));
        check(elapsed < 900, "pumpFor: ...and does not overshoot badly",
              QStringLiteral("%1 ms").arg(elapsed));
    }

    // ---- pumpFor() actually delivers events -------------------------------
    // A sleep would satisfy the duration assertion above while breaking every
    // test that relies on a timer firing inside the window.
    {
        std::atomic_int ticks{0};
        QTimer timer;
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, [&ticks] { ++ticks; });
        timer.start();
        AetherTest::pumpFor(200);
        timer.stop();
        check(ticks.load() >= 3,
              "pumpFor: main-thread timers run during the window",
              QStringLiteral("ticks=%1").arg(ticks.load()));
    }

    if (g_failures == 0)
        std::fprintf(stderr, "\ntest_event_loop_test: all checks passed\n");
    else
        std::fprintf(stderr, "\ntest_event_loop_test: %d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

