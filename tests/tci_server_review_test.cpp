#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/TciServer.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonObject>
#include <QTimer>
#include <QWebSocket>

#include <cstdio>

namespace AetherSDR
{

class TciServerReviewTest
{
public:
    static bool deferredAbortIsClientScoped()
    {
        RadioModel model;
        TciServer server(&model);
        QWebSocket client;
        QWebSocket otherClient;

        server.m_routeTransitionInFlight = true;
        server.handleVfoRequest(&client, TciProtocol::VfoRequest{
            0, 1, 14076000
        });
        server.handleTrxRequest(&client, TciProtocol::TrxRequest{
            0, true, QStringLiteral("tci")
        });
        if (!server.m_pendingTrxRequest
            || server.m_pendingTrxRequest->client != &client
            || server.m_pendingRouteCommands.size() != 1) {
            return false;
        }

        server.handleTrxRequest(&client, TciProtocol::TrxRequest{
            0, false, QString()
        });
        if (server.m_pendingTrxRequest
            || !server.m_pendingRouteCommands.isEmpty()) {
            return false;
        }

        server.m_pendingTrxRequest = TciServer::PendingTrxRequest{
            &otherClient, TciProtocol::TrxRequest{
                0, true, QStringLiteral("tci")
            }
        };
        TciServer::PendingRouteCommand otherRoute;
        otherRoute.client = &otherClient;
        otherRoute.vfo = TciProtocol::VfoRequest{0, 1, 7076000};
        server.m_pendingRouteCommands.append(otherRoute);

        server.handleTrxRequest(&client, TciProtocol::TrxRequest{
            0, false, QString()
        });
        return server.m_pendingTrxRequest
            && server.m_pendingTrxRequest->client == &otherClient
            && server.m_pendingRouteCommands.size() == 1
            && server.m_pendingRouteCommands.first().client == &otherClient;
    }

    static bool routeFailureIsObservable()
    {
        RadioModel model;
        TciServer server(&model);
        server.m_routingState.setSplitRequested(true);
        server.reportVfoBRouteFailure(nullptr,
            TciProtocol::VfoRequest{0, 1, 14076000},
            QStringLiteral("capacity test"), true);

        const QJsonObject snapshot = server.routingSnapshot();
        return !server.m_routingState.splitRequested()
            && snapshot.value(QStringLiteral("lastRouteError")).toString()
                == QStringLiteral("capacity test");
    }

    // ── #4161 power / flag broadcast internals ──────────────────────────────

    // Run the event loop for `ms` so a QTimer (the power rate limiter) can fire,
    // without pulling in QtTest.
    static void spin(int ms)
    {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    }

    // Rapid rfPowerChanged collapses to a prompt leading edge plus one trailing
    // send of the settled value; an unchanged value produces nothing.
    static bool powerBroadcastRateLimits()
    {
        RadioModel model;
        QWebSocket sock;
        TciServer server(&model);
        TciServer::ClientState cs;
        cs.socket = &sock;
        server.m_clients.append(cs);

        QStringList drives;
        QObject::connect(&server, &TciServer::tciMessage,
            [&drives](const QString& dir, const QString& msg) {
                if (dir == QLatin1String("tx")
                    && msg.startsWith(QLatin1String("drive:")))
                    drives << msg.trimmed();
            });

        auto& tx = model.transmitModel();
        tx.setRfPower(55);                       // leading edge — sent at once
        if (drives != QStringList{QStringLiteral("drive:0,55;")}) return false;

        tx.setRfPower(56);                       // inside the 100 ms window:
        tx.setRfPower(57);                       // collapsed, nothing on the wire
        tx.setRfPower(58);
        if (drives.size() != 1) return false;

        spin(160);                               // trailing flush of the latest
        if (drives != QStringList{QStringLiteral("drive:0,55;"),
                                  QStringLiteral("drive:0,58;")}) return false;

        tx.setRfPower(58);                       // unchanged → no broadcast
        spin(160);
        return drives.size() == 2;
    }

    // The #4161 mislabel fix: when the TX flag momentarily clears (the
    // band-change slice-recreation gap), drive: keeps the last resolved TX trx
    // from m_lastTxTrx rather than falling back to trx 0.
    static bool driveTrxSurvivesTxFlagClear()
    {
        RadioModel model;
        QWebSocket sock;
        TciServer server(&model);
        TciServer::ClientState cs;
        cs.socket = &sock;
        server.m_clients.append(cs);

        QString err;
        model.automationApplySliceFixture(0, QString(), &err);
        model.automationApplySliceFixture(1, QString(), &err);
        SliceModel* s0 = model.slice(0);
        SliceModel* s1 = model.slice(1);
        if (!s0 || !s1) return false;
        // wireSlice() is driven by MainWindow_Session in the app; call it here.
        server.wireSlice(s0->sliceId(), s0);
        server.wireSlice(s1->sliceId(), s1);
        // TX flag is radio-authoritative — setTxSlice() only sends a command, so
        // drive the status path (as the radio would) to mark slice 1 TX.
        SliceDelta txOn;
        txOn.txSlice = true;
        s1->applyChanges(txOn);                  // → txSliceChanged → caches trx

        QStringList drives;
        QObject::connect(&server, &TciServer::tciMessage,
            [&drives](const QString& dir, const QString& msg) {
                if (dir == QLatin1String("tx")
                    && msg.startsWith(QLatin1String("drive:")))
                    drives << msg.trimmed();
            });

        server.m_drivePending = true;
        server.m_lastDriveSent = -1;
        server.broadcastPower();
        if (drives.size() != 1) return false;
        const QString withTx = drives.first();   // e.g. "drive:1,100;"
        // The TX slice must map to a non-zero trx, or the test can't tell the
        // cache apart from the old trx-0 fallback.
        if (withTx == QStringLiteral("drive:0,100;")) return false;

        drives.clear();
        SliceDelta txOff;
        txOff.txSlice = false;
        s1->applyChanges(txOff);                 // TX flag cleared → txTrxIndex -1
        server.m_drivePending = true;
        server.m_lastDriveSent = -1;
        server.broadcastPower();
        return drives.size() == 1 && drives.first() == withTx;
    }

    // The last-sent cache is forgotten when the last client leaves, so a genuine
    // change back to the remembered value after a reconnect is not suppressed.
    static bool powerCacheResetsWhenClientsLeave()
    {
        RadioModel model;
        TciServer server(&model);
        server.m_lastDriveSent = 55;
        server.m_lastTuneDriveSent = 19;
        server.m_drivePending = true;
        server.m_tuneDrivePending = true;
        server.broadcastPower();                 // no clients → reset + bail
        return server.m_lastDriveSent == -1 && server.m_lastTuneDriveSent == -1;
    }

    // wireSlice seeds each flag relay from current state, then de-dups — a
    // repeat of the same value does not re-announce.
    static bool flagRelaySeedsAndDeDups()
    {
        RadioModel model;
        QWebSocket sock;
        TciServer server(&model);
        TciServer::ClientState cs;
        cs.socket = &sock;
        server.m_clients.append(cs);

        QStringList nb;
        QObject::connect(&server, &TciServer::tciMessage,
            [&nb](const QString& dir, const QString& msg) {
                if (dir == QLatin1String("tx")
                    && msg.startsWith(QLatin1String("rx_nb_enable:")))
                    nb << msg.trimmed();
            });

        QString err;
        model.automationApplySliceFixture(0, QString(), &err);
        SliceModel* s0 = model.slice(0);
        if (!s0) return false;
        server.wireSlice(s0->sliceId(), s0);     // as MainWindow_Session does; seeds
        const bool seeded = !nb.isEmpty()
            && nb.last() == QStringLiteral("rx_nb_enable:0,false;");

        nb.clear();
        s0->setNb(true);                         // real edge → one broadcast
        const bool oneOnChange
            = nb == QStringList{QStringLiteral("rx_nb_enable:0,true;")};

        s0->setNb(true);                         // same value again → de-duped
        return seeded && oneOnChange && nb.size() == 1;
    }
};

} // namespace AetherSDR

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-tci-server-review-test"));
    QCoreApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();

    const bool validProfile = settingsProfile.isValid();
    const bool deferredAbort
        = AetherSDR::TciServerReviewTest::deferredAbortIsClientScoped();
    const bool observableFailure
        = AetherSDR::TciServerReviewTest::routeFailureIsObservable();
    const bool powerRateLimits
        = AetherSDR::TciServerReviewTest::powerBroadcastRateLimits();
    const bool trxCacheHolds
        = AetherSDR::TciServerReviewTest::driveTrxSurvivesTxFlagClear();
    const bool cacheResets
        = AetherSDR::TciServerReviewTest::powerCacheResetsWhenClientsLeave();
    const bool flagSeedsDeDups
        = AetherSDR::TciServerReviewTest::flagRelaySeedsAndDeDups();

    std::printf("%s  isolated settings profile\n",
                validProfile ? "PASS" : "FAIL");
    std::printf("%s  deferred TRX abort is client-scoped\n",
                deferredAbort ? "PASS" : "FAIL");
    std::printf("%s  VFO-B route failure is observable\n",
                observableFailure ? "PASS" : "FAIL");
    std::printf("%s  drive: rate-limits and de-dups\n",
                powerRateLimits ? "PASS" : "FAIL");
    std::printf("%s  drive: trx survives a TX-flag clear\n",
                trxCacheHolds ? "PASS" : "FAIL");
    std::printf("%s  power cache resets when clients leave\n",
                cacheResets ? "PASS" : "FAIL");
    std::printf("%s  flag relay seeds and de-dups\n",
                flagSeedsDeDups ? "PASS" : "FAIL");

    return validProfile && deferredAbort && observableFailure
        && powerRateLimits && trxCacheHolds && cacheResets && flagSeedsDeDups
        ? 0 : 1;
}
