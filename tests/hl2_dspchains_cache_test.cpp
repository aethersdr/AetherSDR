// Hl2Backend::dspChains() answers from cache (IRadioBackend thread-contract
// rule 3, #7164): it must not wait on the I/O thread, and a publish gathered
// for a session that has since been withdrawn must not land.
//
// Socket-free. The I/O thread is parked by posting it a lambda that waits on a
// latch; no connectRadio(), no transport, no WDSP RX channel, nothing keyed.

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2TxDsp.h"
#include "core/backends/hl2/MetisClient.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <future>

namespace AetherSDR::hl2 {

struct Hl2DspChainsCacheTestAccess {
    static void configure(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_txDsp, [&backend] {
            backend.m_txDsp->configure(Hl2TxDsp::Config{});
        }, Qt::BlockingQueuedConnection);
    }
    // Holds the I/O thread until `release` is set, or for at most `cap`, so a
    // build that still blocks fails the timing check instead of hanging.
    static void park(Hl2Backend& backend, std::shared_future<void> release,
                     std::chrono::milliseconds cap)
    {
        QMetaObject::invokeMethod(backend.m_metis, [release, cap] {
            (void)release.wait_for(cap);
        }, Qt::QueuedConnection);
    }
    static void postGather(Hl2Backend& backend) { backend.postDspChainsGather(); }
    static void invalidateTx(Hl2Backend& backend) { backend.invalidateTxDspConfiguration(); }
    // The cadence that refreshes the live fields: running, and fired once by hand.
    static bool refreshRunning(const Hl2Backend& backend)
    {
        return backend.m_dspChainsRefreshTimer && backend.m_dspChainsRefreshTimer->isActive();
    }
    static bool refreshTick(Hl2Backend& backend)
    {
        return QMetaObject::invokeMethod(backend.m_dspChainsRefreshTimer, "timeout");
    }
    // Lets every queued I/O call run and delivers what it posted back, WITHOUT
    // asking for a new publish: what lands is what was already in flight.
    static void drain(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_metis, [] {}, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    }
    static void accept(Hl2Backend& backend, const QVariantList& chains,
                       quint64 generation, quint64 epoch)
    {
        backend.acceptDspChains(chains, generation, epoch);
    }
    static quint64 generation(const Hl2Backend& backend) { return backend.m_connectGeneration; }
    static quint64 epoch(const Hl2Backend& backend) { return backend.m_dspChainsEpoch; }
};

}  // namespace AetherSDR::hl2

using AetherSDR::hl2::Hl2Backend;
using Access = AetherSDR::hl2::Hl2DspChainsCacheTestAccess;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

static bool txConfigured(const QVariantList& chains)
{
    for (const QVariant& v : chains) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("chain")).toString() == QLatin1String("hl2-tx")) {
            return m.value(QStringLiteral("level")).toString()
                   != QLatin1String("not-configured");
        }
    }
    return false;
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-dspchains-cache-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");
    std::printf("\n  dspChains() answers from cache\n\n");

    constexpr auto kParkCap = std::chrono::milliseconds(2000);
    constexpr qint64 kPromptMs = 250;

    // 1. A parked I/O thread does not hold the read, and the read is the last
    //    published value rather than an empty "could not ask".
    {
        Hl2Backend backend;
        Access::configure(backend);
        backend.settleDspChainsForTest();
        const QVariantList published = backend.dspChains();
        check(txConfigured(published), "the configured modulator is published");

        std::promise<void> latch;
        Access::park(backend, latch.get_future().share(), kParkCap);
        QElapsedTimer clock;
        clock.start();
        const QVariantList read = backend.dspChains();
        const qint64 tookMs = clock.elapsed();
        latch.set_value();
        std::printf("    read took %lld ms with the I/O thread parked\n",
                    static_cast<long long>(tookMs));
        check(tookMs < kPromptMs, "dspChains() returns while the I/O thread is busy");
        check(read == published, "and returns the last published value, unchanged");
        Access::drain(backend);
    }

    // 2. Disconnect while a gather is in flight: the gather ran BEFORE the
    //    queued invalidation, so it reports the old session as configured. It
    //    must not land over the withdrawal (generation and epoch both moved).
    {
        Hl2Backend backend;
        Access::configure(backend);
        backend.settleDspChainsForTest();
        check(txConfigured(backend.dspChains()), "configured before the disconnect");

        std::promise<void> latch;
        Access::park(backend, latch.get_future().share(), kParkCap);
        Access::postGather(backend);            // queued behind the park
        backend.disconnectRadio();               // invalidation queued behind the gather
        check(!txConfigured(backend.dspChains()), "disconnect withdraws the cache at once");
        latch.set_value();
        Access::drain(backend);
        check(!txConfigured(backend.dspChains()),
              "a gather from before the disconnect does not land after it");
        backend.settleDspChainsForTest();
        check(!txConfigured(backend.dspChains()), "and the next publish agrees");
    }

    // 3. The same race on a withdrawal that does NOT start a new connection
    //    (receiver teardown and connect failure share this step): only the
    //    epoch moves, so it alone must reject the stale gather.
    {
        Hl2Backend backend;
        Access::configure(backend);
        backend.settleDspChainsForTest();
        const quint64 generationBefore = Access::generation(backend);

        std::promise<void> latch;
        Access::park(backend, latch.get_future().share(), kParkCap);
        Access::postGather(backend);
        Access::invalidateTx(backend);
        check(Access::generation(backend) == generationBefore,
              "the withdrawal leaves the connect generation alone");
        check(!txConfigured(backend.dspChains()), "the withdrawal clears the cache at once");
        latch.set_value();
        Access::drain(backend);
        check(!txConfigured(backend.dspChains()),
              "a gather from before the withdrawal does not land after it");
    }

    // 4. A publish stamped with the previous connect generation is dropped even
    //    when the epoch matches; one with the current stamps lands.
    {
        Hl2Backend backend;
        const QVariantList configured = {QVariantMap{
            {QStringLiteral("chain"), QStringLiteral("hl2-tx")},
            {QStringLiteral("level"), QStringLiteral("dsp-config")}}};
        backend.disconnectRadio();   // moves the connect generation to >= 1
        Access::drain(backend);
        const quint64 gen = Access::generation(backend);
        const quint64 epoch = Access::epoch(backend);
        Access::accept(backend, configured, gen - 1, epoch);
        check(!txConfigured(backend.dspChains()),
              "a publish from the previous connect generation is dropped");
        Access::accept(backend, configured, gen, epoch);
        check(txConfigured(backend.dspChains()), "a publish with current stamps lands");
    }

    // 5. The refresh cadence re-gathers with no verb behind it, which is what
    //    keeps the live fields (notch count, blanker, modulator counters) fresh.
    {
        Hl2Backend backend;
        check(!txConfigured(backend.dspChains()), "fresh backend reports TX not-configured");
        Access::configure(backend);   // the modulator moves; nothing publishes
        Access::drain(backend);
        check(!txConfigured(backend.dspChains()),
              "an unpublished change is not visible: the read is the cache");
        check(Access::refreshRunning(backend), "the refresh cadence runs while unconnected");
        check(Access::refreshTick(backend), "a refresh tick fires");
        Access::drain(backend);
        check(txConfigured(backend.dspChains()), "one refresh tick publishes it");
    }

    std::printf("\n  %s (%d failure%s)\n\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
