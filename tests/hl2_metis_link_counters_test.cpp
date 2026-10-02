// MetisClient's transport counters and link edges, socket-free: no start(), no
// bind, no peer. Packets go out through the packet-sink seam, window state is
// injected through MetisClientTestAccess, and timers are fired by hand.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    static void setStreaming(MetisClient& c, bool linkUp)
    {
        c.m_running = true;
        c.m_linkUp = linkUp;
    }
    static void sink(MetisClient& c, qint64 written)
    {
        c.m_packetSinkForTest = [written](const auto&) { return written; };
    }
    static void send(MetisClient& c) { c.sendControlPacket(); }
    static void wakeup(MetisClient& c) { c.accountReceiveWakeup(); }
    static void setWindow(MetisClient& c, int wakeups, qint64 sumUs, qint64 maxUs)
    {
        c.m_linkWindowWakeups = wakeups;
        c.m_linkWindowGapSumUs = sumUs;
        c.m_linkWindowGapMaxUs = maxUs;
    }
    // An invalid window clock means "due", so every call publishes.
    static void publishNow(MetisClient& c)
    {
        c.m_linkWindowClock.invalidate();
        c.publishLinkCountersIfDue();
    }
    static void fireConnectWatchdog(MetisClient& c)
    {
        QMetaObject::invokeMethod(c.m_connectWatchdog, "timeout", Qt::DirectConnection);
    }
};
}  // namespace AetherSDR::hl2

using namespace AetherSDR::hl2;
using Access = MetisClientTestAccess;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

static MetisClient::LinkCounters lastPublished(const QSignalSpy& spy)
{
    return qvariant_cast<MetisClient::LinkCounters>(spy.last().at(0));
}

static void txCounters()
{
    MetisClient c;
    Access::setStreaming(c, true);
    Access::sink(c, static_cast<qint64>(kUsbPacketSize));
    for (int i = 0; i < 3; ++i)
        Access::send(c);
    const auto lc = c.linkCounters();
    check(lc.txPackets == 3, "each EP2 datagram the sink accepted is one txPacket");
    check(lc.txBytes == 3 * kUsbPacketSize, "txBytes sums the bytes actually written");

    // A refused write is not traffic.
    Access::sink(c, -1);
    Access::send(c);
    Access::sink(c, 0);
    Access::send(c);
    check(c.linkCounters().txPackets == 3 && c.linkCounters().txBytes == 3 * kUsbPacketSize,
          "a failed or empty write is not counted");
}

static void stopEmitsLinkDownOnce()
{
    MetisClient c;
    Access::setStreaming(c, true);
    QSignalSpy down(&c, &MetisClient::linkDown);
    c.stop();
    check(!c.isRunning(), "stop() leaves the client not running");
    check(down.count() == 1, "stop() of a live link emits linkDown once");
    c.stop();
    check(down.count() == 1, "a second stop() does not re-emit linkDown");

    MetisClient never;
    Access::setStreaming(never, false);
    QSignalSpy neverDown(&never, &MetisClient::linkDown);
    never.stop();
    check(neverDown.isEmpty(), "stop() before the first EP6 emits no linkDown");
}

static void connectWatchdog()
{
    MetisClient silent;
    Access::setStreaming(silent, false);
    QSignalSpy failed(&silent, &MetisClient::connectFailed);
    QSignalSpy up(&silent, &MetisClient::linkUp);
    Access::fireConnectWatchdog(silent);
    check(failed.count() == 1, "no EP6 by the connect deadline emits connectFailed");
    check(up.isEmpty(), "and never linkUp");

    MetisClient streaming;
    Access::setStreaming(streaming, true);
    QSignalSpy notFailed(&streaming, &MetisClient::connectFailed);
    Access::fireConnectWatchdog(streaming);
    check(notFailed.isEmpty(), "a link already up does not report a failed connect");
}

static void snapshotAndGapRounding()
{
    MetisClient c;
    Access::setStreaming(c, true);
    Access::sink(c, static_cast<qint64>(kUsbPacketSize));
    QSignalSpy pub(&c, &MetisClient::linkCountersUpdated);

    // No wakeups: the gap is "not measured", never 0 ms.
    Access::send(c);
    Access::publishNow(c);
    check(pub.count() == 1, "a due window publishes one snapshot");
    check(lastPublished(pub).meanGapMs == -1 && lastPublished(pub).maxGapMs == -1,
          "a window with no wakeups publishes -1, not 0");
    check(lastPublished(pub).txPackets == 1,
          "the snapshot carries the traffic counted before it");

    // The first wakeup of a session only seeds the clock.
    Access::wakeup(c);
    Access::publishNow(c);
    check(lastPublished(pub).meanGapMs == -1,
          "the first wakeup contributes no gap sample");

    // Round to nearest, not truncate: 1.5 ms is 2 ms and 0.499 ms is 0 ms.
    Access::setWindow(c, 1, 1500, 1500);
    Access::publishNow(c);
    check(lastPublished(pub).meanGapMs == 2 && lastPublished(pub).maxGapMs == 2,
          "a 1500 us gap rounds to 2 ms");
    Access::setWindow(c, 2, 998, 499);
    Access::publishNow(c);
    check(lastPublished(pub).meanGapMs == 0 && lastPublished(pub).maxGapMs == 0,
          "a 499 us gap rounds to 0 ms");
    Access::setWindow(c, 4, 4 * 2600, 7400);
    Access::publishNow(c);
    check(lastPublished(pub).meanGapMs == 3 && lastPublished(pub).maxGapMs == 7,
          "mean is sum over wakeups, max is the window maximum");

    // The window resets at each publish; the cumulative totals do not.
    Access::publishNow(c);
    check(lastPublished(pub).meanGapMs == -1,
          "the gap window starts empty after a publish");
    Access::send(c);
    Access::publishNow(c);
    check(lastPublished(pub).txPackets == 2, "cumulative totals survive the window reset");
    check(c.linkCounters().txPackets == lastPublished(pub).txPackets
              && c.linkCounters().txBytes == lastPublished(pub).txBytes,
          "the accessor and the published snapshot agree");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<MetisClient::LinkCounters>();

    txCounters();
    stopEmitsLinkDownOnce();
    connectWatchdog();
    snapshotAndGapRounding();

    std::fprintf(stderr, "hl2_metis_link_counters_test: %s\n",
                 g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
