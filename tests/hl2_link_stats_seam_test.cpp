// Hl2Backend::linkStats and its publish cadence, socket-free: link edges and
// counter snapshots are emitted as MetisClient's own signals on the I/O thread,
// and the cadence tick is driven by hand instead of waiting on its timer.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisClient.h"

#include "core/AppSettings.h"

#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>

#include <cstdio>

namespace AetherSDR::hl2 {
struct Hl2HealthBlockTestAccess {
    static void metisEdge(Hl2Backend& b, const char* signal)
    {
        QMetaObject::invokeMethod(b.m_metis, signal, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&b, QEvent::MetaCall);
    }
    static void counters(Hl2Backend& b, const MetisClient::LinkCounters& c)
    {
        MetisClient* metis = b.m_metis;
        QMetaObject::invokeMethod(metis, [metis, c] { emit metis->linkCountersUpdated(c); },
                                  Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&b, QEvent::MetaCall);
    }
    static void tick(Hl2Backend& b) { b.publishLinkStats(); }
    static bool cadenceRunning(const Hl2Backend& b) { return b.m_linkStatsTimer->isActive(); }
};
}  // namespace AetherSDR::hl2

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;
using AetherSDR::hl2::MetisClient;
using Access = AetherSDR::hl2::Hl2HealthBlockTestAccess;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

static IRadioBackend::LinkStats last(const QSignalSpy& spy)
{
    return spy.last().at(0).value<IRadioBackend::LinkStats>();
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-link-stats-seam-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    qRegisterMetaType<IRadioBackend::LinkStats>();
    qRegisterMetaType<MetisClient::LinkCounters>();

    Hl2Backend b;
    QSignalSpy ticks(&b, &IRadioBackend::linkStatsUpdated);

    check(!b.linkStats().reported, "no transport is reported before connect");
    check(!Access::cadenceRunning(b), "no publish cadence before connect");

    Access::metisEdge(b, "linkUp");
    check(b.linkStats().reported, "a connected backend reports its transport");
    check(Access::cadenceRunning(b), "the publish cadence starts on the first EP6");

    MetisClient::LinkCounters c;
    c.rxPackets = 40;
    c.rxBytes = 40 * 1032;
    c.txBytes = 12 * 1032;
    c.txPackets = 12;
    c.drops = 2;
    c.meanGapMs = 3;
    c.maxGapMs = 9;
    c.localEndpoint = QStringLiteral("192.0.2.7:50000");
    Access::counters(b, c);
    Access::tick(b);
    check(!ticks.isEmpty(), "a tick publishes linkStatsUpdated");
    IRadioBackend::LinkStats s = last(ticks);
    check(s.reported && s.alive, "fresh EP6 since the last tick reads alive");
    check(s.rxPackets == 40 && s.rxBytes == 40 * 1032 && s.txBytes == 12 * 1032
              && s.rxPacketsLost == 2,
          "the published counters are the client's");
    check(s.localEndpoint == c.localEndpoint, "the local endpoint is carried through");
    check(s.rttMs < 0, "RTT is reported as not measured, never as 0");
    check(s.gapMs == 3 && s.gapMaxMs == 9 && s.jitterMs == 6,
          "gap, max gap and their spread are carried through");

    // A silent second: counters republished unchanged, or not at all.
    Access::counters(b, c);
    Access::tick(b);
    s = last(ticks);
    check(s.reported && !s.alive, "a tick with no new EP6 reads not alive, but still reported");
    Access::tick(b);
    check(last(ticks).reported && !last(ticks).alive,
          "a tick with no counter publish at all also reads not alive");
    check(b.linkStats().rttMs < 0, "linkStats() keeps RTT unmeasured between ticks");

    Access::metisEdge(b, "linkDown");
    check(!Access::cadenceRunning(b), "link loss stops the publish cadence");
    check(!b.linkStats().reported, "a disconnected backend stops reporting a transport");

    std::fprintf(stderr, "hl2_link_stats_seam_test: %s\n",
                 g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
