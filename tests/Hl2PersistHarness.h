#pragma once

// Shared plumbing for the HL2 persistence tests. One "session" is a real
// Hl2Backend that is restored and connected through the production
// connectRadio(), with no socket: MetisClient::start() runs its session resets
// and then returns through a test stand-in instead of binding a QUdpSocket.
// The link edge is injected through MetisClient's own linkUp signal, so the
// production linkUp handler runs (pushInitialState, the receiver replay). The
// receiver-count probe is skipped by passing boardMaxRx, and no telemetry
// service is attached, so nothing else can open one either. Each session
// checks that: the client holds no socket and, on POSIX, the process holds no
// more socket descriptors than it did before the first backend existed.
//
// Capture, store and load go through the production currentOperatingState()
// and RadioStateMemory, so what is compared is what a restart would find on
// disk.

#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/RadioStateMemory.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2Receivers.h"
#include "core/backends/hl2/MetisClient.h"

#include "TestDspBuildWait.h"

#include <QCoreApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QString>

#include <atomic>
#include <cstdio>
#include <memory>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace AetherSDR::hl2 {

struct MetisClientTestAccess {
    static void installStartStandIn(MetisClient& c, std::shared_ptr<std::atomic<int>> starts)
    {
        c.m_startTransportForTest = [starts](const MetisClient::Params&) {
            ++*starts;
            return true;
        };
    }
    static bool holdsSocket(const MetisClient& c) { return c.m_socket != nullptr; }
};

struct Hl2PersistTestAccess {
    static void installStartStandIn(Hl2Backend& b, std::shared_ptr<std::atomic<int>> starts)
    {
        QMetaObject::invokeMethod(b.m_metis, [metis = b.m_metis, starts] {
            MetisClientTestAccess::installStartStandIn(*metis, starts);
        }, Qt::BlockingQueuedConnection);
    }
    static bool clientHoldsSocket(Hl2Backend& b)
    {
        bool holds = true;
        QMetaObject::invokeMethod(b.m_metis, [metis = b.m_metis, &holds] {
            holds = MetisClientTestAccess::holdsSocket(*metis);
        }, Qt::BlockingQueuedConnection);
        return holds;
    }
    // The first EP6 datagram's edge, as hl2_session_memory_test injects it.
    static void linkUp(Hl2Backend& b)
    {
        QMetaObject::invokeMethod(b.m_metis, "linkUp", Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&b, QEvent::MetaCall);
    }
    static bool buildsSettled(const Hl2Backend& b)
    {
        for (const Hl2Backend::Receiver& r : b.m_rx) {
            if (r.dspBuildInFlight)
                return false;
        }
        return true;
    }
    static int receiverCount(const Hl2Backend& b) { return static_cast<int>(b.m_rx.size()); }
    static QString panIdOf(const Hl2Backend& b, int ddc)
    {
        const Hl2ReceiverIds* ids = b.m_ids.byDdc(ddc);
        return ids ? ids->panId : QString();
    }
    static int sliceIdOf(const Hl2Backend& b, int ddc)
    {
        const Hl2ReceiverIds* ids = b.m_ids.byDdc(ddc);
        return ids ? ids->uiNumber : -1;
    }
};

}  // namespace AetherSDR::hl2

namespace AetherSDR::persist {

inline int g_failures = 0;

inline void check(bool ok, const QString& what)
{
    std::fprintf(stdout, "%s %s\n", ok ? "[ OK ]" : "[FAIL]", qPrintable(what));
    if (!ok)
        ++g_failures;
}

// Socket descriptors this process holds; -1 where it cannot be counted.
inline int openSocketCount()
{
#if defined(Q_OS_UNIX)
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit <= 0 || limit > 65536)
        limit = 65536;
    int count = 0;
    struct stat st;
    for (int fd = 0; fd < limit; ++fd) {
        if (fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode))
            ++count;
    }
    return count;
#else
    return -1;
#endif
}

// Taken in main() before the first backend exists.
inline int g_socketBaseline = -1;

class Session {
public:
    hl2::Hl2Backend backend;
    QString panId;   // receiver A's pan

    using Access = hl2::Hl2PersistTestAccess;

    // `restored` empty means a radio with nothing remembered, which comes up on
    // `startHz`. A non-empty one comes up on what it remembers.
    Session(const QString& serial, const RestoredRadioState& restored, double startHz)
    {
        Access::installStartStandIn(backend, m_starts);
        connect(serial, restored, startHz);
    }
    ~Session() { backend.disconnectRadio(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // restore -> connectRadio() -> DSP build -> start() stand-in -> link-up.
    void connect(const QString& serial, const RestoredRadioState& restored, double startHz)
    {
        const int startsBefore = m_starts->load();
        QSignalSpy finished(&backend, &hl2::Hl2Backend::dspSetupFinished);
        backend.applyRestoredState(restored);
        RadioConnectRequest request;
        request.host = QStringLiteral("192.0.2.1");   // TEST-NET-1; nothing is sent
        request.port = 1024;
        request.serial = serial;
        request.params.insert(QStringLiteral("boardMaxRx"), 4);   // no discovery probe
        if (restored.isEmpty())
            request.params.insert(QStringLiteral("rxFrequencyHz"), startHz);
        backend.connectRadio(request);
        test::awaitDspBuild("hl2_persist", [&] { return !finished.isEmpty(); });
        check(m_starts->load() == startsBefore + 1,
              QStringLiteral("harness: %1's connect reached MetisClient::start()").arg(serial));
        Access::linkUp(backend);
        check(backend.isConnected(), QStringLiteral("harness: %1 is linked up").arg(serial));
        settle();
        panId = Access::panIdOf(backend, 0);
        checkNoSocket(serial);
    }

    // Receivers B.. open asynchronously (the replay and createPanadapter()).
    void settle()
    {
        test::awaitDspBuild("hl2_persist", [&] { return Access::buildsSettled(backend); });
    }

    void checkNoSocket(const QString& who)
    {
        check(!Access::clientHoldsSocket(backend),
              QStringLiteral("no socket: %1's MetisClient holds none").arg(who));
        if (g_socketBaseline >= 0) {
            const int now = openSocketCount();
            check(now == g_socketBaseline,
                  QStringLiteral("no socket: the process holds %1 socket descriptor(s), as "
                                 "before the first backend (%2)")
                      .arg(now).arg(g_socketBaseline));
        }
    }

    int liveLnaDb() const
    {
        return backend.healthSnapshot().values.value(QStringLiteral("lnaGainDb"), 999).toInt();
    }
    // The drive actually held for the band the session is on is what a capture
    // writes under that band's key.
    int liveDriveFor(const QString& band) const { return driveOf(backend.currentOperatingState(), band); }

    static int driveOf(const RestoredRadioState& s, const QString& band)
    {
        return s.extension.value(QStringLiteral("txSetpoints")).toObject()
            .value(QStringLiteral("driveByBand")).toObject().value(band).toInt(-999);
    }
    static int lnaOf(const RestoredRadioState& s, const QString& band)
    {
        return s.extension.value(QStringLiteral("rfGain")).toObject()
            .value(QStringLiteral("lnaDbByBand")).toObject().value(band).toInt(-999);
    }
    static bool hasDrive(const RestoredRadioState& s, const QString& band)
    {
        return s.extension.value(QStringLiteral("txSetpoints")).toObject()
            .value(QStringLiteral("driveByBand")).toObject().contains(band);
    }
    static bool hasLna(const RestoredRadioState& s, const QString& band)
    {
        return s.extension.value(QStringLiteral("rfGain")).toObject()
            .value(QStringLiteral("lnaDbByBand")).toObject().contains(band);
    }
    // Receiver `i`'s entry in the captured receiver list (A is 0).
    static QJsonObject receiverOf(const RestoredRadioState& s, int i)
    {
        return s.extension.value(QStringLiteral("receivers")).toArray().at(i).toObject();
    }

private:
    std::shared_ptr<std::atomic<int>> m_starts = std::make_shared<std::atomic<int>>(0);
};

// store() then load() under the radio's scope: what the next launch reads.
inline RestoredRadioState throughDisk(const RadioSettingsScope& scope,
                                      const RadioCapabilities& caps,
                                      const RestoredRadioState& captured, bool* stored = nullptr)
{
    const bool ok = RadioStateMemory::store(scope, caps, captured);
    if (stored)
        *stored = ok;
    return RadioStateMemory::load(scope, caps);
}

}  // namespace AetherSDR::persist
