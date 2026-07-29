// FlexControlManager port-loss recovery (#4574).
//
// The knob had no error path at all: a dropped port stayed "open" as far as Qt
// was concerned, readyRead never fired again, and nothing retried — so the
// control was dead until the process (or, on Windows, the machine) restarted.
//
// These cases cover the parts that are testable without a FlexControl attached:
// the recovery state machine and the close() handle-release. The USB drop
// itself needs hardware; what is asserted here is that a connection the
// operator WANTS keeps being retried, one they cancelled does not, and that
// close() always releases the port rather than early-returning on an errored
// one.

#include "core/FlexControlManager.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // A port name no real device owns, so open() fails the same way on every
    // platform and in CI, where no FlexControl exists.
    const QString kAbsentPort = QStringLiteral("AETHER_TEST_NO_SUCH_PORT");

    {
        // A failed open ARMS the retry rather than giving up.
        //
        // This is THE discriminating assertion: before the fix open() returned
        // false and nothing ever tried again, so isReconnecting() was
        // permanently false and the operator had to reconnect by hand. isOpen()
        // is false either way, which is why the retry state has to be
        // observable for this test to mean anything.
        FlexControlManager fc;
        check(!fc.isReconnecting(), "not retrying before any open attempt");
        check(!fc.open(kAbsentPort), "opening an absent port fails");
        check(!fc.isOpen(), "an absent port is not reported open");
        check(fc.isReconnecting(),
              "a failed open ARMS the retry (was: gave up silently)");

        // Still retrying after several intervals — it must not latch off.
        QTest::qWait(FlexControlManager::kReconnectIntervalMs * 2 + 400);
        check(fc.isReconnecting(), "still retrying while the device is absent");
        check(!fc.isOpen(), "no device, so still not open — but not wedged");
    }

    {
        // close() is the operator saying "stop". It must cancel the retry, or
        // Disconnect would immediately undo itself.
        FlexControlManager fc;
        fc.open(kAbsentPort);
        check(fc.isReconnecting(), "retry armed by the failed open");

        fc.close();
        check(!fc.isReconnecting(),
              "a deliberate close() cancels the retry — Disconnect stays disconnected");

        QSignalSpy spy(&fc, &FlexControlManager::connectionChanged);
        QTest::qWait(FlexControlManager::kReconnectIntervalMs + 400);
        check(!fc.isReconnecting(), "and stays cancelled across an interval");
        check(spy.isEmpty(), "no reconnection happened behind the operator's back");
    }

    {
        // close() on a never-opened port is safe, quiet, and idempotent.
        FlexControlManager fc;
        QSignalSpy spy(&fc, &FlexControlManager::connectionChanged);
        fc.close();
        fc.close();
        check(spy.isEmpty(), "close() on a never-opened port emits nothing");
        check(!fc.isOpen(), "still closed");
        check(!fc.isReconnecting(), "and is not left retrying");
    }

    {
        // A second open() after close() must re-arm: closing cleared the
        // "connection wanted" state, so the retry has to come back with it.
        FlexControlManager fc;
        fc.open(kAbsentPort);
        fc.close();
        check(!fc.isReconnecting(), "cancelled");
        fc.open(kAbsentPort);
        check(fc.isReconnecting(), "a fresh open() re-arms the retry after a close()");
        fc.close();
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "flex_control_recovery_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "flex_control_recovery_test: %d failure(s)\n", g_failures);
    return 1;
}
