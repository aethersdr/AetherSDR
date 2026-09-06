#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/RadioDiscovery.h"
#include "core/TciServer.h"
#include "core/TciTrxMap.h"
#include "core/backends/sim/SimBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QAbstractSocket>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <cstring>
#include <cstdio>

namespace AetherSDR
{

namespace
{

QStringList* g_logSink = nullptr;

void captureLogHandler(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    if (g_logSink) {
        *g_logSink << msg;
    }
}

// RadioModel has no outbound-command signal, and the pan↔channel binding this
// file now asserts goes out through RadioModel::sendCommand(), not DaxIqModel.
// It is logged at debug on aether.protocol, so capture that for the lifetime of
// a test rather than leaving the bind unasserted — without it, four channels
// fighting over one panadapter looks identical to four working streams.
class ScopedCommandLog
{
public:
    ScopedCommandLog()
    {
        g_logSink = &m_lines;
        m_previous = qInstallMessageHandler(captureLogHandler);
        QLoggingCategory::setFilterRules(QStringLiteral("aether.protocol.debug=true"));
    }
    ~ScopedCommandLog()
    {
        QLoggingCategory::setFilterRules(QString());
        qInstallMessageHandler(m_previous);
        g_logSink = nullptr;
    }
    ScopedCommandLog(const ScopedCommandLog&) = delete;
    ScopedCommandLog& operator=(const ScopedCommandLog&) = delete;

    bool contains(const QString& fragment) const
    {
        for (const QString& line : m_lines) {
            if (line.contains(fragment)) {
                return true;
            }
        }
        return false;
    }
    void clear() { m_lines.clear(); }

private:
    QStringList      m_lines;
    QtMessageHandler m_previous{nullptr};
};

// Collect everything logged while `body` runs. aether.cat defaults to
// QtWarningMsg, so the info-level route line needs the rule as well as the
// handler.
template <typename Body>
QStringList captureCatLog(Body body)
{
    QStringList captured;
    g_logSink = &captured;
    const QtMessageHandler previous = qInstallMessageHandler(captureLogHandler);
    QLoggingCategory::setFilterRules(QStringLiteral("aether.cat.info=true"));

    body();

    QLoggingCategory::setFilterRules(QString());
    qInstallMessageHandler(previous);
    g_logSink = nullptr;
    return captured;
}

QString findLine(const QStringList& lines, const char* prefix)
{
    for (const QString& line : lines) {
        if (line.startsWith(QLatin1String(prefix))) {
            return line;
        }
    }
    return QString();
}

} // namespace

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

    // #4547: two WSJT-X instances both address trx 0, so the wire request
    // cannot tell them apart. The receiver each declared in audio_start does.
    static bool pttBindsToTheDeclaredAudioReceiver()
    {
        RadioModel model;
        TciServer server(&model);
        QWebSocket wsjtxA;
        QWebSocket wsjtxB;
        QWebSocket controlOnly;

        TciServer::ClientState a;
        a.socket = &wsjtxA;
        a.audioEnabled = true;
        a.audioReceiver = 0;
        TciServer::ClientState b;
        b.socket = &wsjtxB;
        b.audioEnabled = true;
        b.audioReceiver = 1;
        TciServer::ClientState c;      // Stream Deck: control only, no audio
        c.socket = &controlOnly;
        c.audioReceiver = -1;
        server.m_clients.append(a);
        server.m_clients.append(b);
        server.m_clients.append(c);

        // Both instances put trx 0 on the wire; B is operating receiver 1.
        return server.effectiveTrx(&wsjtxA, 0) == 0
            && server.effectiveTrx(&wsjtxB, 0) == 1
            // ...but ONLY trx 0 is redirected. trx 0 is the ambiguous default
            // every WSJT-X instance sends whatever receiver it operates, so it
            // carries no intent to override. A non-zero trx is a deliberate
            // address: B declared receiver 1 and is honoured when it asks for
            // trx 0, and still honoured when it explicitly asks for trx 2.
            // Overriding that would key a slice the client never asked for —
            // #4547's own defect class, re-entered through its fix.
            && server.effectiveTrx(&wsjtxB, 2) == 2
            && server.effectiveTrx(&wsjtxA, 1) == 1
            // A client that declared none keeps whatever it addresses.
            && server.effectiveTrx(&controlOnly, 0) == 0
            && server.effectiveTrx(&controlOnly, 2) == 2
            // An unknown socket falls back to the wire index rather than
            // guessing receiver 0.
            && server.effectiveTrx(nullptr, 3) == 3;
    }

    // The #4547 / #4567 seam. sliceForTrx() resolves through the stable trx
    // map; the PTT path must resolve through the SAME map, not TciProtocol's
    // positional statics. If it did not, a band-stack recreate would key
    // whichever slice happened to sit at the requested index while every other
    // command followed the binding — and nothing would catch it: the #4547
    // tests build no map, the #4567 tests never key.
    static bool pttResolvesThroughTheStableMap()
    {
        RadioModel model;
        TciServer server(&model);

        QString err;
        model.automationApplySliceFixture(0, QString(), &err);
        model.automationApplySliceFixture(1, QString(), &err);
        SliceModel* s0 = model.slice(0);
        SliceModel* s1 = model.slice(1);
        if (!s0 || !s1) return false;

        // Bind so the map deliberately DISAGREES with list position: slice id 1
        // holds receiver 0. That is the state a mid-session recreate produces,
        // and it is the only state in which a positional resolver is visibly
        // wrong. (Clear first — the sliceAdded handler already bound these in
        // creation order.)
        server.m_trxMap.clear();
        if (server.m_trxMap.acquire(1) != 0 || server.m_trxMap.acquire(0) != 1) {
            return false;
        }

        // Receiver 0 is slice id 1, even though slices()[0] is slice id 0.
        return server.sliceForTrxStrict(0) == s1
            && server.sliceForTrxStrict(1) == s0
            // Guard: if this ever equals the positional answer the seam has
            // regressed, whatever the first two assertions happen to say.
            && server.sliceForTrxStrict(0) != s0
            // And the strict variant still agrees with the lax one wherever
            // the lax one is not guessing.
            && server.sliceForTrxStrict(0) == server.sliceForTrx(0);
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

    static bool disconnectSnapshotIsPayloadFreeAndUsesUnsetError()
    {
        RadioModel model;
        TciServer server(&model);
        QWebSocket socket;
        TciServer::ClientState client;
        client.socket = &socket;

        const QJsonObject snapshot = server.disconnectSnapshot(client, &socket);
        return !snapshot.contains(QStringLiteral("closeReason"))
            && snapshot.value(QStringLiteral("socketError")).toInt() == -1
            && snapshot.value(QStringLiteral("socketErrorString")).toString().isEmpty()
            && snapshot.value(QStringLiteral("lastSocketErrorAgeMs")).toInt() == -1;
    }

    static bool outboundTextAccountingIsCentralized()
    {
        RadioModel model;
        TciServer server(&model);
        QWebSocket socket;
        TciServer::ClientState client;
        client.socket = &socket;
        server.m_clients.append(client);

        server.sendClientText(
            &socket, QStringLiteral("rx_channel_sensors:0,0,-73.0;"));
        return server.m_clients.first().lastTextTxAtMs >= 0
            && server.m_clients.first().lastTxCommand
                == QStringLiteral("rx_channel_sensors");
    }

    // ── #4547 PTT routing diagnostic ────────────────────────────────────────

    // Two slices, slice 1 holding transmit, and a routing cache still pointing
    // at slice 0 from when slice 0 was TX. This is the exact state behind the
    // fldigi capture on #4547: the client addresses the slice that genuinely
    // has TX, branch 1 of resolvePttSlice is therefore skipped, and the stale
    // cache drags transmit back to slice 0 — wrong band, wrong antenna.
    static bool pttRouteFixture(RadioModel& model, TciServer& server)
    {
        QString error;
        if (!model.automationApplySliceFixture(0, QString(), &error)
            || !model.automationApplySliceFixture(1, QString(), &error)) {
            std::fprintf(stderr, "ptt route fixtures failed: %s\n",
                         error.toUtf8().constData());
            return false;
        }
        SliceModel* s1 = model.slice(1);
        if (!s1) {
            return false;
        }
        // The TX flag is radio-authoritative — drive the status path the way
        // the radio would, rather than calling setTxSlice() (which only sends).
        SliceDelta txOn;
        txOn.txSlice = true;
        s1->applyChanges(txOn);

        // The stale route: bound while slice 0 held transmit, never refreshed.
        server.m_routingState.bindCreatedRoute(1, 0);
        return true;
    }

    // The diagnostic's whole reason to exist is showing the cached route
    // DISAGREEING with the live TX assignment. Pinned because the sampling
    // order that makes it possible is load-bearing and invisible:
    // resolvePttSlice() writes the live assignment through to the cache on its
    // external-TX branch, so reading cachedTx AFTER the resolve reports the
    // cache agreeing with liveTx on every path — the line stays plausible and
    // says nothing. Move those three reads below the resolve and this fails.
    static bool pttRouteLogExposesStaleCache()
    {
        RadioModel model;
        TciServer server(&model);
        QWebSocket client;
        if (!pttRouteFixture(model, server)) {
            return false;
        }

        const QStringList log = captureCatLog([&] {
            server.handleTrxRequest(&client, TciProtocol::TrxRequest{
                1, true, QStringLiteral("tci")
            });
        });

        const QString route = findLine(log, "TCI PTT route:");
        if (route.isEmpty()) {
            std::fprintf(stderr, "no TCI PTT route line was logged\n");
            return false;
        }
        // Slice ids carry their receiver number, because the wire speaks trx
        // and a reader correlating the two must not have to guess (#4567).
        //
        // The resolved slice here is the LIVE transmitter, not the stale cache:
        // #4547 made the live TX slice outrank a cached route, so the scenario
        // this fixture builds — a route bound while slice 0 held transmit,
        // never refreshed after the operator moved transmit to slice 1 — no
        // longer drags transmit back onto slice 0. The diagnostic's own job is
        // unchanged and is still what this pins: cachedTx must be sampled
        // BEFORE the resolve, so it can still show the disagreement (0 vs 1)
        // that the resolve is about to write away.
        const char* const required[] = {
            "trx=1",
            "rxSlice=1(trx1)",
            "-> txSlice=1(trx1)",
            "(the requested slice)",
            "liveTx=1(trx1)",       // transmit is really on slice 1...
            "cachedTx=0(trx0)",     // ...and the cache still said slice 0
            "owner=tci-created",
        };
        for (const char* needle : required) {
            if (!route.contains(QLatin1String(needle))) {
                std::fprintf(stderr, "route line missing %s\n  got: %s\n",
                             needle, route.toUtf8().constData());
                return false;
            }
        }
        // And transmit does NOT move. This is the #4547 secondary defect in its
        // literal form: before the fix this logged "transmit will move from
        // slice 1(trx1) to slice 0(trx0)" and then keyed slice 0's band and
        // antenna with no operator action.
        return findLine(log, "TCI PTT: transmit will move from slice").isEmpty();
    }

    // The other half of the same defect, and the one that pins the sampling
    // ORDER. Here the client addresses a slice that is NOT the transmitter, so
    // resolvePttSlice() takes its external-TX branch — and that branch writes
    // the live assignment through to the cache (m_txSliceId, m_rxSliceId,
    // m_owner) before returning. Read cachedTx AFTER the resolve and it always
    // equals liveTx: the line still prints, still looks reasonable, and has
    // quietly stopped reporting the disagreement it exists for. That is what
    // 60b78967 shipped and 0610df79 fixed. Move the three reads below the
    // resolve and this test fails; the one above it does not.
    static bool pttRouteLogSamplesCacheBeforeResolve()
    {
        RadioModel model;
        TciServer server(&model);
        QWebSocket client;
        if (!pttRouteFixture(model, server)) {
            return false;
        }
        SliceModel* s1 = model.slice(1);
        if (!s1) {
            return false;
        }
        // Inhibit the slice that holds transmit. The route line is logged
        // before this guard, so the decision is still fully recorded — and the
        // request stops here instead of running on into a real key-down.
        model.setPanTransmitInhibited(s1->panId(), true,
                                      QStringLiteral("inhibited for test"));

        // Split is what makes a TX route answer this request at all since
        // #4547: a bare PTT now keys the slice it names, and the external-TX
        // branch — the one that writes the live assignment through to the cache
        // and so is the only branch that can erase the disagreement this test
        // exists to catch — is reached only when a route actually applies.
        server.m_routingState.setSplitRequested(true);

        // trx 0 addresses slice 0, which is not the TX slice: with a route
        // applying, the requested receiver is resolved away onto slice 1.
        const QStringList log = captureCatLog([&] {
            server.handleTrxRequest(&client, TciProtocol::TrxRequest{
                0, true, QStringLiteral("tci")
            });
        });

        const QString route = findLine(log, "TCI PTT route:");
        if (route.isEmpty()) {
            std::fprintf(stderr, "no TCI PTT route line was logged\n");
            return false;
        }
        const char* const required[] = {
            "trx=0",
            "rxSlice=0(trx0)",
            "-> txSlice=1(trx1)",
            "(NOT the requested slice)",
            "liveTx=1(trx1)",
            "cachedTx=0(trx0)",     // post-resolve this reads 1(trx1)
            "cachedRx=1(trx1)",     // post-resolve this reads 0(trx0)
            "owner=tci-created",    // post-resolve this reads external
        };
        for (const char* needle : required) {
            if (!route.contains(QLatin1String(needle))) {
                std::fprintf(stderr, "route line missing %s\n  got: %s\n",
                             needle, route.toUtf8().constData());
                return false;
            }
        }
        // The drop reason is stated, and names the slice in both numberings.
        return !findLine(log, "TCI PTT: slice 1(trx1) is transmit-inhibited - "
                              "inhibited for test")
                    .isEmpty();
    }

    // source= is client-supplied and printed with .noquote(), so without
    // sanitising, any TCI client could emit a newline and forge a second
    // "TCI PTT route:" line into the log a reporter attaches to an issue.
    // Drop the simplified() call and this fails.
    static bool pttRouteLogSanitizesClientSource()
    {
        RadioModel model;
        TciServer server(&model);
        QWebSocket client;
        if (!pttRouteFixture(model, server)) {
            return false;
        }

        const QStringList log = captureCatLog([&] {
            server.handleTrxRequest(&client, TciProtocol::TrxRequest{
                1, true,
                QStringLiteral("dax\nTCI PTT route: trx=9 rxSlice=9(trx9)")
            });
        });

        int routeLines = 0;
        for (const QString& line : log) {
            if (line.contains(QLatin1String("TCI PTT route:"))) {
                ++routeLines;
            }
        }
        if (routeLines != 1) {
            std::fprintf(stderr, "expected 1 route line, got %d\n", routeLines);
            return false;
        }
        const QString route = findLine(log, "TCI PTT route:");
        if (route.contains(QLatin1Char('\n'))) {
            std::fprintf(stderr, "route line carries an embedded newline: %s\n",
                         route.toUtf8().constData());
            return false;
        }
        // Neutralised in place and bounded, not silently dropped — the field
        // is still evidence about which client sent the request. source= is
        // last on the line, so what follows it is exactly the rendered field.
        const QString source = route.section(QLatin1String(" source="), 1);
        if (!source.startsWith(QLatin1String("dax TCI PTT route:"))) {
            std::fprintf(stderr, "source not neutralised in place: '%s'\n",
                         source.toUtf8().constData());
            return false;
        }
        if (source.size() > 32) {
            std::fprintf(stderr, "source not bounded: %lld chars\n",
                         static_cast<long long>(source.size()));
            return false;
        }
        return true;
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

    // wireSlice schedules a de-duping change handler plus a *deferred* settled
    // seed, both sharing one baseline. Nothing goes on the wire synchronously;
    // after the settle window the seed announces the current value once, and a
    // repeat edge does not re-announce.
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
        server.wireSlice(s0->sliceId(), s0);     // as MainWindow_Session does
        if (!nb.isEmpty()) return false;         // seed is deferred, not sync
        spin(450);                               // let the settled seed fire
        const bool seeded
            = nb == QStringList{QStringLiteral("rx_nb_enable:0,false;")};

        nb.clear();
        s0->setNb(true);                         // real edge → one broadcast
        const bool oneOnChange
            = nb == QStringList{QStringLiteral("rx_nb_enable:0,true;")};

        s0->setNb(true);                         // same value again → de-duped
        return seeded && oneOnChange && nb.size() == 1;
    }

    // The #4161 transient fix: on a band-change re-wire the seed reads the
    // *settled* flag, not the recreated slice's pre-settle state, so a client
    // sees exactly one correct edge — never a stale blip then a correction.
    // Model it: wire a slice holding a pre-settle value, flip it inside the
    // settle window (as the radio's restore does), and require the client to
    // see only the final value, once. An immediate seed would emit the blip.
    static bool flagSeedReadsSettledNotTransient()
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
        s0->setNb(true);                         // recreated slice's pre-settle
        nb.clear();
        server.wireSlice(s0->sliceId(), s0);     // re-wire; seed deferred
        spin(120);                               // still inside the window
        s0->setNb(false);                        // radio restores settled value
        // Handler announces the settled edge exactly once...
        if (nb != QStringList{QStringLiteral("rx_nb_enable:0,false;")})
            return false;
        spin(400);                               // ...and the deferred seed de-dups.
        return nb.size() == 1;
    }

    // A TX slice *closed* (removed with no recreation) must not leave
    // m_lastTxTrx pointing at a dead index. The deferred cleanup resets it once
    // the settle window passes with no live slice carrying that trx; a
    // same-id recreation (band change) inside the window must not reset it.
    static bool lastTxTrxResetsOnClose()
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
        server.wireSlice(s0->sliceId(), s0);
        server.wireSlice(s1->sliceId(), s1);
        SliceDelta txOn;
        txOn.txSlice = true;
        s1->applyChanges(txOn);                  // caches m_lastTxTrx = trx(s1)
        const int cachedTrx = server.m_lastTxTrx;
        if (cachedTrx == 0) return false;        // need non-zero to detect a reset

        model.automationRemoveSliceFixture(1, &err);   // close it for good
        // Immediately after removal the cache is untouched (band-change grace).
        if (server.m_lastTxTrx != cachedTrx) return false;
        spin(600);                               // past the 500 ms cleanup
        return server.m_lastTxTrx == 0;
    }

    // #4160 — GUI focus must reach TCI, and a slice must seed focus from
    // *current* state rather than waiting for a future activeChanged edge.
    // RadioModel applies active=1 (emitting activeChanged) BEFORE it emits
    // sliceAdded, and MainWindow wires the slice from sliceAdded — so the edge
    // has already fired by the time TciServer::wireSlice() runs. wireSlice()
    // therefore seeds from isActive() as well as connecting for future edges.
    // That seed was previously exercised only on hardware (see PR #4323
    // test-plan note); this covers it in-process by mirroring MainWindow's
    // wireTciSlice() call over the disconnected slice-fixture seam.
    static bool activeSliceSeedsFromCurrentState()
    {
        RadioModel model;
        TciServer server(&model);

        // Mirror MainWindow_Session::wireTciSlice(): trx is the contiguous
        // index within the owned slice list.
        auto wire = [&](SliceModel* s) {
            server.wireSlice(model.slices().indexOf(s), s);
        };

        QString error;
        if (!model.automationApplySliceFixture(0, QStringLiteral("A"), &error)) {
            std::fprintf(stderr, "fixture A failed: %s\n",
                         error.toUtf8().constData());
            return false;
        }
        wire(model.slices().value(0));
        // No client is connected, yet focus is tracked already: the seed runs
        // unconditionally so the *next* client's init burst is correct.
        if (server.m_activeTrx != 0
            || server.m_activeLetter != QStringLiteral("A")) {
            std::fprintf(stderr,
                         "seed after fixture A: got trx=%d letter=%s\n",
                         server.m_activeTrx,
                         server.m_activeLetter.toUtf8().constData());
            return false;
        }

        // Adding a second slice moves GUI focus (single-active semantics). TCI
        // follows by slice identity; trx is positional, so B resolves to trx 1.
        if (!model.automationApplySliceFixture(1, QStringLiteral("B"), &error)) {
            std::fprintf(stderr, "fixture B failed: %s\n",
                         error.toUtf8().constData());
            return false;
        }
        wire(model.slices().value(1));
        if (server.m_activeTrx != 1
            || server.m_activeLetter != QStringLiteral("B")) {
            std::fprintf(stderr,
                         "focus switch to B: got trx=%d letter=%s\n",
                         server.m_activeTrx,
                         server.m_activeLetter.toUtf8().constData());
            return false;
        }

        // The seed handoff the accept path performs (TciServer connection
        // setup) must make a newly connected client's protocol report current
        // focus, not a stale scan — the exact bug #4160 is about.
        TciProtocol seeded(&model, &server.m_routingState);
        seeded.setActiveSlice(server.m_activeTrx, server.m_activeLetter);
        return seeded.handleCommand(QStringLiteral("active_slice"))
            == QStringLiteral("active_slice:1,B;");
    }

    // #4160 established this hook when trx was positional (removal renumbered
    // every later slice). Under #4567's stable bindings removal no longer
    // renumbers ANYTHING — the focused slice keeps its trx — but the hook
    // still matters: removing the FOCUSED slice must clear the tracked trx
    // rather than leave it naming a dead slice. Both halves covered here.
    static bool activeSliceFollowsSliceRemoval()
    {
        RadioModel model;
        TciServer server(&model);
        auto wire = [&](SliceModel* s) {
            server.wireSlice(model.slices().indexOf(s), s);
        };

        QString error;
        if (!model.automationApplySliceFixture(0, QStringLiteral("A"), &error)
            || !model.automationApplySliceFixture(1, QStringLiteral("B"),
                                                  &error)) {
            std::fprintf(stderr, "removal fixtures failed: %s\n",
                         error.toUtf8().constData());
            return false;
        }
        wire(model.slices().value(0));
        wire(model.slices().value(1));
        if (server.m_activeTrx != 1
            || server.m_activeLetter != QStringLiteral("B")) {
            std::fprintf(stderr,
                         "removal setup: expected focus B at trx 1, got trx=%d letter=%s\n",
                         server.m_activeTrx,
                         server.m_activeLetter.toUtf8().constData());
            return false;
        }

        // Remove the EARLIER, unfocused slice. Focus stays with B — and under
        // #4567's stable bindings B KEEPS trx 1 (the pre-#4567 positional
        // policy renumbered it to 0 here; that silent renumber-under-a-live-
        // client is the defect #4567 fixed).
        if (!model.automationRemoveSliceFixture(0, &error)) {
            std::fprintf(stderr, "remove slice 0 failed: %s\n",
                         error.toUtf8().constData());
            return false;
        }
        if (server.m_activeTrx != 1
            || server.m_activeLetter != QStringLiteral("B")) {
            std::fprintf(stderr,
                         "after removing trx 0: expected focus B to KEEP trx 1 (#4567), "
                         "got trx=%d letter=%s\n",
                         server.m_activeTrx,
                         server.m_activeLetter.toUtf8().constData());
            return false;
        }

        // Remove the FOCUSED slice: nothing holds focus until the radio picks a
        // new slice, so the tracked trx clears rather than naming a slice that
        // no longer exists.
        if (!model.automationRemoveSliceFixture(1, &error)) {
            std::fprintf(stderr, "remove slice 1 failed: %s\n",
                         error.toUtf8().constData());
            return false;
        }
        if (server.m_activeTrx != -1) {
            std::fprintf(stderr,
                         "after removing the focused slice: expected cleared focus, got trx=%d\n",
                         server.m_activeTrx);
            return false;
        }

        // A client connecting in that window must be told nothing rather than
        // be handed a scanned guess that was never broadcast.
        TciProtocol seeded(&model, &server.m_routingState);
        seeded.setActiveSlice(server.m_activeTrx, server.m_activeLetter);
        return seeded.handleCommand(QStringLiteral("active_slice")).isEmpty();
    }

    // #4567 — the captured defect, end to end on the model: a band-stack
    // switch destroys and recreates its slice (same Flex slice id) while a
    // second slice is open. The recreate re-enters RadioModel's list at the
    // tail, so positional numbering handed the surviving slice receiver 0 —
    // silently re-routing a live client. With stable bindings the recreated
    // slice reclaims its number (the release is deferred past the settle
    // window, and no event loop runs here, so the binding is held exactly as
    // it is on hardware) and the survivor never moves.
    static bool trxStableAcrossSliceRecreate()
    {
        RadioModel model;
        TciServer server(&model);

        QString error;
        if (!model.automationApplySliceFixture(0, QStringLiteral("A"), &error)
            || !model.automationApplySliceFixture(1, QStringLiteral("B"),
                                                  &error)) {
            std::fprintf(stderr, "recreate fixtures failed: %s\n",
                         error.toUtf8().constData());
            return false;
        }
        SliceModel* b = model.slice(1);
        if (server.m_trxMap.trxForSlice(&model, model.slice(0)) != 0
            || server.m_trxMap.trxForSlice(&model, b) != 1) {
            std::fprintf(stderr, "recreate setup: expected A=0 B=1\n");
            return false;
        }

        // The band switch: destroy A, recreate it with the same slice id.
        // The recreated slice is APPENDED — list order is now [B, newA].
        if (!model.automationRemoveSliceFixture(0, &error)
            || !model.automationApplySliceFixture(0, QStringLiteral("A"),
                                                  &error)) {
            std::fprintf(stderr, "recreate cycle failed: %s\n",
                         error.toUtf8().constData());
            return false;
        }
        SliceModel* newA = model.slice(0);
        if (!newA || model.slices().indexOf(newA) != 1) {
            std::fprintf(stderr,
                         "recreate precondition lost: expected the recreated "
                         "slice at the list tail\n");
            return false;
        }

        // The regression: positional numbering answered B=0 / newA=1 here.
        if (server.m_trxMap.trxForSlice(&model, newA) != 0) {
            std::fprintf(stderr, "recreated slice did not reclaim trx 0\n");
            return false;
        }
        if (server.m_trxMap.trxForSlice(&model, b) != 1) {
            std::fprintf(stderr, "surviving slice lost trx 1\n");
            return false;
        }
        return server.m_trxMap.sliceForTrx(&model, 0) == newA
            && server.m_trxMap.sliceForTrx(&model, 1) == b;
    }

    // #4577 review — the reconnect hole: stageSessionModelsForReconnect()
    // reclaims the previous session's slices without sliceAdded (RadioModel's
    // !reclaimed guard) while the map was cleared at disconnect, so every
    // slice is live but unbound. The next genuine slice-add must not collide:
    // binding only the new slice would hand it trx 0 on top of the slice the
    // positional fallback already resolves to 0. The fixture reproduces the
    // reclaim END STATE directly (live slices + cleared map) rather than the
    // staging machinery — identical from the map's point of view.
    static bool liveSlicesKeepDistinctTrxAfterReconnect()
    {
        RadioModel model;
        TciServer server(&model);

        QString error;
        if (!model.automationApplySliceFixture(0, QStringLiteral("A"), &error)
            || !model.automationApplySliceFixture(1, QStringLiteral("B"),
                                                  &error)) {
            std::fprintf(stderr, "reconnect fixtures failed: %s\n",
                         error.toUtf8().constData());
            return false;
        }

        // The disconnect/reclaim cycle: bindings die with the connection,
        // slices survive into the new session unbound.
        server.m_trxMap.clear();

        // First slice-add of the new session.
        if (!model.automationApplySliceFixture(2, QStringLiteral("C"),
                                               &error)) {
            std::fprintf(stderr, "reconnect add failed: %s\n",
                         error.toUtf8().constData());
            return false;
        }

        // The invariant the design rests on: no two live slices resolve to
        // the same trx.
        QSet<int> seen;
        for (SliceModel* s : model.slices()) {
            const int trx = server.m_trxMap.trxForSlice(&model, s);
            if (seen.contains(trx)) {
                std::fprintf(stderr,
                             "duplicate trx %d among live slices after "
                             "reconnect + add\n",
                             trx);
                return false;
            }
            seen.insert(trx);
        }
        // And the rebind walk reproduces the numbering the positional policy
        // would have given: A=0 B=1 C=2 in list order.
        return server.m_trxMap.trxForSlice(&model, model.slice(0)) == 0
            && server.m_trxMap.trxForSlice(&model, model.slice(1)) == 1
            && server.m_trxMap.trxForSlice(&model, model.slice(2)) == 2;
    }

    // #4577 review — inside the 500 ms settle window the two lookup
    // directions must not contradict: trxForSlice answers from the held
    // binding (the survivor keeps its number), so sliceForTrx answering the
    // held number positionally would route an inbound command to the wrong
    // slice — during exactly the window a band-changing client is most
    // likely to send one. A held-but-not-live trx answers "no slice right
    // now"; callers already treat null as unknown-receiver and drop the
    // command.
    //
    // Runs against the demo's synthetic wire (the demo_backend_swap_test
    // pattern): the positional fallback opens with an isConnected() guard,
    // so on a disconnected fixture model the pre-fix code answers null by
    // coincidence and the case cannot discriminate. The event loop spins
    // only BEFORE the removal — the deferred release stays held through the
    // assertions, exactly as it is on hardware inside the window.
    static bool heldTrxRefusesInsteadOfRepointing()
    {
        RadioModel model;
        TciServer server(&model);

        RadioInfo demo;
        demo.name    = QStringLiteral("FLEX-6700");
        demo.model   = SimBackend::demoModelName();
        demo.serial  = SimBackend::demoSerial();
        demo.family  = SimBackend::familyName();
        demo.address = QHostAddress(QHostAddress::LocalHost);  // never dialed
        demo.port    = 4992;
        model.connectToRadio(demo);
        for (int i = 0;
             i < 40 && !(model.isConnected() && !model.slices().isEmpty());
             ++i) {
            QEventLoop loop;
            QTimer::singleShot(50, &loop, &QEventLoop::quit);
            loop.exec();
        }
        if (!model.isConnected() || model.slices().isEmpty()) {
            std::fprintf(stderr,
                         "settle: demo connect did not settle (connected=%d "
                         "slices=%d)\n",
                         model.isConnected(), int(model.slices().size()));
            return false;
        }
        SliceModel* live = model.slices().first();

        // A standalone map holding the settle-window state directly: trx 0 is
        // bound to a slice id with no live slice (exactly what the deferred
        // release preserves after a band-change destroy), while a live slice
        // sits at list position 0. The demo cannot grow a second slice and
        // the slice fixtures refuse while connected, so the held state is
        // constructed on the map itself — identical from sliceForTrx's point
        // of view, and the model is genuinely connected, which is what arms
        // the positional fallback the pre-fix code fell through to.
        TciTrxMap map;
        const int heldTrx = map.acquire(9999);            // dead id holds 0
        const int liveTrx = map.acquire(live->sliceId()); // live slice gets 1
        if (heldTrx != 0 || liveTrx != 1) {
            std::fprintf(stderr, "settle setup: expected held=0 live=1, got "
                         "%d/%d\n", heldTrx, liveTrx);
            return false;
        }

        // Outbound: the live slice keeps its own number.
        if (map.trxForSlice(&model, live) != liveTrx) {
            std::fprintf(stderr, "live slice lost trx %d\n", liveTrx);
            return false;
        }
        // Inbound: the held number answers nothing. The pre-fix fallback
        // answered the live slice at list position 0 — an inbound command
        // for the held receiver re-pointed at a different slice, the very
        // failure #4567 is about.
        if (SliceModel* wrong = map.sliceForTrx(&model, heldTrx)) {
            std::fprintf(stderr,
                         "held trx %d answered slice id %d instead of null\n",
                         heldTrx, wrong->sliceId());
            return false;
        }
        // The live slice's own number still resolves to it.
        return map.sliceForTrx(&model, liveTrx) == live;
    }

    // ── vfo: SET must confirm the frequency it actually reached (#4500/#4493) ──
    //
    // A raw `slice tune` written at the connection bypasses SliceModel, and the
    // radio answers it with no RF_frequency status, so the model was never
    // updated and the confirmation echoed the PRE-TUNE frequency forever.
    // WSJT-X's do_frequency() waits on that echo: it concluded the radio had not
    // moved and failed every band change, while the radio was in fact on the new
    // band — which is how transmissions ended up out of band.
    //
    // Asserted on the wire (broadcast() emits tciMessage even with no clients
    // attached) rather than only on the model, because the echo is what clients
    // actually consume.
    static bool vfoSetConfirmsTheAcceptedFrequency()
    {
        RadioModel model;
        QString error;
        if (!model.automationApplySliceFixture(0, QString(), &error)) {
            std::printf("      fixture precondition failed: %s\n", qPrintable(error));
            return false;
        }
        SliceModel* slice = model.slice(0);
        if (!slice)
            return false;

        TciServer server(&model);
        // In production this runs at session setup (MainWindow_Session.cpp),
        // which is what makes frequencyChanged -> broadcastSliceFrequencies()
        // live for a real move; without it this test would only ever exercise
        // tuneSliceAndConfirm's own explicit confirmation, never the automatic
        // path it now defers to on channel 0 (#5086).
        server.wireSlice(0, slice);
        QWebSocket client;
        // broadcastSliceFrequencies() bails early on an empty m_clients (it
        // has no client to send to yet), unlike the explicit confirmation
        // below it, which broadcast()s unconditionally. A registered client
        // is what makes the automatic path observable here at all.
        QWebSocket sock;
        TciServer::ClientState cs;
        cs.socket = &sock;
        server.m_clients.append(cs);

        QStringList sent;
        QObject::connect(&server, &TciServer::tciMessage,
                         [&sent](const QString& dir, const QString& msg) {
            if (dir == QLatin1String("tx"))
                sent << msg.trimmed();
        });

        // Nine successive relative tunes — the Stream Deck encoder case from
        // #4493. Each confirmation must carry the frequency just reached, or a
        // client accumulating "current + delta" recomputes every step from the
        // same stale origin and the radio oscillates instead of walking.
        long long hz = TciProtocol::mhzToHz(slice->frequency());
        if (hz <= 0)
            return false;
        for (int step = 0; step < 9; ++step) {
            const long long want = hz - 1000;
            sent.clear();
            server.tuneSliceAndConfirm(&client, 0, 0, 0, want);

            if (TciProtocol::mhzToHz(slice->frequency()) != want) {
                std::printf("      step %d: model did not take the tune "
                            "(wanted %lld, model holds %lld)\n",
                            step, want, TciProtocol::mhzToHz(slice->frequency()));
                return false;
            }
            if (!sent.contains(QStringLiteral("vfo:0,0,%1;").arg(want))) {
                std::printf("      step %d: no confirmation for %lld; sent: %s\n",
                            step, want, qPrintable(sent.join(QStringLiteral(" "))));
                return false;
            }
            // The pre-tune value must not also go out, or a client cannot tell
            // which of the two echoes is current.
            if (sent.contains(QStringLiteral("vfo:0,0,%1;").arg(hz))) {
                std::printf("      step %d: stale %lld echoed alongside %lld\n",
                            step, hz, want);
                return false;
            }
            hz = want;
        }
        return true;
    }

    // A tune the model refuses must be confirmed with what the model HOLDS, not
    // with what the client asked for — otherwise the client's mirror walks away
    // from the radio, which is the same divergence by a different route.
    static bool vfoSetConfirmsTruthWhenRefused()
    {
        RadioModel model;
        QString error;
        if (!model.automationApplySliceFixture(0, QString(), &error))
            return false;
        SliceModel* slice = model.slice(0);
        if (!slice)
            return false;

        TciServer server(&model);
        QWebSocket client;

        const long long parked = TciProtocol::mhzToHz(slice->frequency());
        slice->setLocked(true);

        QStringList sent;
        QObject::connect(&server, &TciServer::tciMessage,
                         [&sent](const QString& dir, const QString& msg) {
            if (dir == QLatin1String("tx"))
                sent << msg.trimmed();
        });

        server.tuneSliceAndConfirm(&client, 0, 0, 0, parked - 5000);

        if (TciProtocol::mhzToHz(slice->frequency()) != parked) {
            std::printf("      a locked slice moved\n");
            return false;
        }
        // Confirmed at all (never leave a client waiting out its timeout), and
        // confirmed with the truth.
        return sent.contains(QStringLiteral("vfo:0,0,%1;").arg(parked))
            && !sent.contains(QStringLiteral("vfo:0,0,%1;").arg(parked - 5000));
    }

    // A repeat of the frequency the slice is already on still has to answer.
    // This used to short-circuit ahead of the tune and confirm from the model
    // without commanding the radio — harmless alone, but combined with the stale
    // model above it silently dropped real tunes once the two had diverged. With
    // the model authoritative again the short-circuit is gone; what must survive
    // is that a no-op is still acknowledged rather than met with silence.
    static bool vfoSetAcknowledgesANoOp()
    {
        RadioModel model;
        QString error;
        if (!model.automationApplySliceFixture(0, QString(), &error))
            return false;
        SliceModel* slice = model.slice(0);
        if (!slice)
            return false;

        TciServer server(&model);
        QWebSocket client;

        QStringList sent;
        QObject::connect(&server, &TciServer::tciMessage,
                         [&sent](const QString& dir, const QString& msg) {
            if (dir == QLatin1String("tx"))
                sent << msg.trimmed();
        });

        const long long parked = TciProtocol::mhzToHz(slice->frequency());
        server.tuneSliceAndConfirm(&client, 0, 0, 0, parked);
        return sent.contains(QStringLiteral("vfo:0,0,%1;").arg(parked));
    }

    // #5086: a channel-0 SET that actually moves the frequency is already
    // broadcast once by frequencyChanged -> broadcastSliceFrequencies(),
    // fired synchronously inside setFrequency()/tuneAndRecenter() before
    // tuneSliceAndConfirm's own confirmation runs. Sending it again produced
    // two near-simultaneous vfo: frames for the same value, suspected of
    // racing a TCI client's own frequency-restore scheduler (intermittent RX
    // frequency drift on WSJT-X "Fake It" split, after a TX/RX cycle). Exactly
    // one vfo:0,0,<hz>; must reach the wire per real move, not two.
    static bool vfoSetChannelZeroBroadcastsExactlyOnce()
    {
        RadioModel model;
        QString error;
        if (!model.automationApplySliceFixture(0, QString(), &error))
            return false;
        SliceModel* slice = model.slice(0);
        if (!slice)
            return false;

        TciServer server(&model);
        // Matches production session setup (MainWindow_Session.cpp): without
        // this, frequencyChanged -> broadcastSliceFrequencies() is never
        // live, and the test can't observe the duplicate this fix removes.
        server.wireSlice(0, slice);
        QWebSocket client;
        // broadcastSliceFrequencies() bails early on an empty m_clients,
        // unlike the explicit confirmation, which broadcast()s
        // unconditionally — a registered client makes the automatic path
        // observable here at all.
        QWebSocket sock;
        TciServer::ClientState cs;
        cs.socket = &sock;
        server.m_clients.append(cs);

        QStringList sent;
        QObject::connect(&server, &TciServer::tciMessage,
                         [&sent](const QString& dir, const QString& msg) {
            if (dir == QLatin1String("tx"))
                sent << msg.trimmed();
        });

        const long long want = TciProtocol::mhzToHz(slice->frequency()) - 1000;
        server.tuneSliceAndConfirm(&client, 0, 0, 0, want);

        const QString expected = QStringLiteral("vfo:0,0,%1;").arg(want);
        const int occurrences = sent.count(expected);
        if (occurrences != 1) {
            std::printf("      expected exactly one %s, saw %d (sent: %s)\n",
                        qPrintable(expected), occurrences,
                        qPrintable(sent.join(QStringLiteral(" "))));
            return false;
        }
        return true;
    }

    // #4744: switching from resampled TCI RX to native rate carries staged
    // samples into the native frame source. Releasing that buffer before
    // gain conversion or sendBinaryMessage() left the payload pointer dangling.
    static bool native24kAudioRetainsAccumulatedPayload()
    {
        RadioModel model;
        TciServer server(&model);
        if (!server.start(0) || !server.m_server) {
            std::printf("      TCI server failed to bind an ephemeral port\n");
            return false;
        }
        const quint16 port = server.port();

        QByteArray receivedFrame;
        QWebSocket client;
        QObject::connect(&client, &QWebSocket::binaryMessageReceived,
                         [&receivedFrame](const QByteArray& frame) {
            receivedFrame = frame;
        });
        client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1")
                             .arg(port)));

        for (int i = 0; i < 100
             && (client.state() != QAbstractSocket::ConnectedState
                 || server.m_clients.isEmpty()); ++i) {
            spin(10);
        }
        if (client.state() != QAbstractSocket::ConnectedState
            || server.m_clients.isEmpty()) {
            std::printf("      clientState=%d clients=%lld error=%s\n",
                        static_cast<int>(client.state()),
                        static_cast<long long>(server.m_clients.size()),
                        client.errorString().toUtf8().constData());
            return false;
        }

        client.sendTextMessage(QStringLiteral(
            "audio_samplerate:48000;audio_stream_sample_type:float32;"
            "audio_stream_channels:2;audio_start:0;"));
        for (int i = 0; i < 100 && !server.m_clients.first().audioEnabled; ++i) {
            spin(10);
        }
        if (!server.m_clients.first().audioEnabled
            || server.m_clients.first().audioSampleRate != 48000) {
            std::printf("      audioEnabled=%d sampleRate=%d\n",
                        server.m_clients.first().audioEnabled,
                        server.m_clients.first().audioSampleRate);
            return false;
        }

        constexpr int kFrames = 128;
        QByteArray pcm(kFrames * 2 * static_cast<int>(sizeof(float)),
                       Qt::Uninitialized);
        float* samples = reinterpret_cast<float*>(pcm.data());
        for (int i = 0; i < kFrames * 2; ++i) {
            samples[i] = static_cast<float>(i + 1) / 1024.0f;
        }
        constexpr float kGain = 0.5f;
        server.m_rxChannelGain[0] = kGain;

        // Seed a sub-threshold 48 kHz resampler accumulation, then switch to
        // native 24 kHz.  The old implementation retained this staging buffer
        // across the rate change and released it before the native-rate gain
        // loop read it.
        server.onDaxAudioReady(1, pcm);
        spin(20);
        if (!receivedFrame.isEmpty()) {
            std::printf("      48 kHz staging unexpectedly emitted a frame\n");
            return false;
        }

        client.sendTextMessage(QStringLiteral("audio_samplerate:24000;"));
        for (int i = 0; i < 100
             && server.m_clients.first().audioSampleRate != 24000; ++i) {
            spin(10);
        }
        if (server.m_clients.first().audioSampleRate != 24000) {
            std::printf("      sampleRate=%d after native-rate switch\n",
                        server.m_clients.first().audioSampleRate);
            return false;
        }

        QByteArray nextPcm = pcm;
        float* nextSamples = reinterpret_cast<float*>(nextPcm.data());
        for (int i = 0; i < kFrames * 2; ++i) {
            nextSamples[i] = -samples[i];
        }
        QByteArray expectedPcm = pcm + nextPcm;
        float* expectedSamples = reinterpret_cast<float*>(expectedPcm.data());
        for (int i = 0; i < kFrames * 4; ++i) {
            expectedSamples[i] *= kGain;
        }
        server.onDaxAudioReady(1, nextPcm);

        for (int i = 0; i < 100 && receivedFrame.isEmpty(); ++i) {
            spin(10);
        }
        constexpr int kHeaderBytes = 64;
        if (receivedFrame.size() != kHeaderBytes + expectedPcm.size()) {
            std::printf("      received=%lld expected=%lld\n",
                        static_cast<long long>(receivedFrame.size()),
                        static_cast<long long>(kHeaderBytes + expectedPcm.size()));
        }
        return receivedFrame.size() == kHeaderBytes + expectedPcm.size()
            && std::memcmp(receivedFrame.constData() + kHeaderBytes,
                           expectedPcm.constData(),
                           static_cast<size_t>(expectedPcm.size())) == 0;
    }

    // Put a fixture slice on a panadapter of its own. automationApplySliceFixture
    // hardcodes pan 0x40000000 for every slice, which is the shared-pan case —
    // useful on its own, but it cannot exercise four distinct channels.
    static void movePan(RadioModel& model, int sliceId, const QString& panId)
    {
        QMap<QString, QString> kvs;
        kvs.insert(QStringLiteral("pan"), panId);
        model.handleSliceStatusForTest(sliceId, kvs, false);
    }

    struct IqHarness {
        QStringList         iqCommands;
        QVector<QByteArray> frames;
    };

    static QSet<int> frameReceivers(const QVector<QByteArray>& frames)
    {
        QSet<int> receivers;
        for (const QByteArray& frame : frames) {
            if (frame.size() < 64) {
                continue;
            }
            quint32 receiver = 0;
            quint32 type = 0;
            std::memcpy(&receiver, frame.constData(), sizeof(receiver));
            std::memcpy(&type,
                        frame.constData() + 6 * static_cast<int>(sizeof(quint32)),
                        sizeof(type));
            if (type == 0) {
                receivers.insert(static_cast<int>(receiver));
            }
        }
        return receivers;
    }

    // Four receivers on four panadapters: the ordinary skimmer setup. Each takes
    // its documented channel (trx n → DAX IQ channel n+1, #3913), each pan is
    // bound to it, and stopping one leaves the other three streaming.
    static bool fourPansGiveFourIqStreams()
    {
        ScopedCommandLog cmdLog;
        RadioModel model;
        TciServer server(&model);

        QString error;
        for (int sliceId = 0; sliceId < DaxIqModel::NUM_CHANNELS; ++sliceId) {
            if (!model.automationApplySliceFixture(sliceId, QString(), &error)) {
                std::printf("      IQ fixture %d failed: %s\n", sliceId,
                            error.toUtf8().constData());
                return false;
            }
            movePan(model, sliceId,
                    QStringLiteral("0x4000000%1").arg(sliceId));
        }

        IqHarness h;
        QObject::connect(&model.daxIqModel(), &DaxIqModel::commandReady,
                         [&h](const QString& c) { h.iqCommands.append(c); });

        if (!server.start(0) || !server.m_server) {
            std::printf("      TCI server failed to bind an ephemeral port\n");
            return false;
        }

        QWebSocket client;
        QObject::connect(&client, &QWebSocket::binaryMessageReceived,
                         [&h](const QByteArray& f) { h.frames.append(f); });
        client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.port())));
        for (int i = 0; i < 100
             && (client.state() != QAbstractSocket::ConnectedState
                 || server.m_clients.isEmpty()); ++i) {
            spin(10);
        }
        if (client.state() != QAbstractSocket::ConnectedState
            || server.m_clients.isEmpty()) {
            return false;
        }

        client.sendTextMessage(QStringLiteral(
            "iq_samplerate:96000;iq_start:0;iq_start:1;iq_start:2;iq_start:3;"));
        for (int i = 0; i < 100
             && server.m_clients.first().iqReceivers.size()
                    != DaxIqModel::NUM_CHANNELS; ++i) {
            spin(10);
        }
        const QSet<int> allReceivers{0, 1, 2, 3};
        if (server.m_iqSampleRate != 96000
            || server.m_clients.first().iqReceivers != allReceivers) {
            std::printf("      IQ subscriptions=%lld rate=%d\n",
                        static_cast<long long>(
                            server.m_clients.first().iqReceivers.size()),
                        server.m_iqSampleRate);
            return false;
        }
        for (int channel = 1; channel <= DaxIqModel::NUM_CHANNELS; ++channel) {
            if (!h.iqCommands.contains(
                    QStringLiteral("stream create type=dax_iq daxiq_channel=%1")
                        .arg(channel))) {
                std::printf("      no DAX IQ create for channel %d\n", channel);
                return false;
            }
            // The stream is inert until its pan is routed to it. Assert the
            // bind, not just the create — nothing else in this file did, so
            // four channels fighting over one pan looked identical to four
            // working streams.
            if (!cmdLog.contains(
                    QStringLiteral("display pan set 0x4000000%1 daxiq_channel=%2")
                        .arg(channel - 1).arg(channel))) {
                std::printf("      no pan bind for channel %d\n", channel);
                return false;
            }
        }

        float iqPair[2]{0.25f, -0.5f};
        QByteArray rawPayload(reinterpret_cast<const char*>(iqPair),
                              static_cast<int>(sizeof(iqPair)));
        for (int channel = 1; channel <= DaxIqModel::NUM_CHANNELS; ++channel) {
            server.onIqDataReady(channel, rawPayload, 96000);
        }
        for (int i = 0; i < 100
             && h.frames.size() < DaxIqModel::NUM_CHANNELS; ++i) {
            spin(10);
        }
        if (frameReceivers(h.frames) != allReceivers) {
            std::printf("      four-stream frame receivers=%lld\n",
                        static_cast<long long>(frameReceivers(h.frames).size()));
            return false;
        }

        // Stopping receiver 1 must leave 0, 2 and 3 live on this same socket,
        // and must release channel 2 — stream AND pan binding, so the pan is
        // not left routed at a channel nothing reads.
        cmdLog.clear();
        client.sendTextMessage(QStringLiteral("iq_stop:1;"));
        for (int i = 0; i < 100
             && server.m_clients.first().iqReceivers.contains(1); ++i) {
            spin(10);
        }
        if (!cmdLog.contains(
                QStringLiteral("display pan set 0x40000001 daxiq_channel=0"))) {
            std::printf("      stopped receiver left its pan bound\n");
            return false;
        }
        h.frames.clear();
        for (int channel = 1; channel <= DaxIqModel::NUM_CHANNELS; ++channel) {
            server.onIqDataReady(channel, rawPayload, 96000);
        }
        for (int i = 0; i < 100 && h.frames.size() < 3; ++i) {
            spin(10);
        }
        return server.m_clients.first().iqReceivers == QSet<int>{0, 2, 3}
            && frameReceivers(h.frames) == QSet<int>{0, 2, 3};
    }

    // Two receivers whose slices share a panadapter — the ordinary A/B layout.
    // On a FLEX the pan↔channel binding is 1:1 in both directions, so binding a
    // second channel to the same pan takes the first one away and the earlier
    // skimmer goes silent with no error anywhere. Both receivers see the same
    // spectrum, so ONE channel serves both, and each gets its own frame header.
    static bool receiversSharingAPanShareOneChannel()
    {
        ScopedCommandLog cmdLog;
        RadioModel model;
        TciServer server(&model);

        QString error;
        for (int sliceId = 0; sliceId < 2; ++sliceId) {
            if (!model.automationApplySliceFixture(sliceId, QString(), &error)) {
                return false;
            }
        }
        // Both fixtures are already on 0x40000000; assert that rather than
        // assume it, so a fixture change cannot quietly void this test.
        if (model.slice(0)->panId() != model.slice(1)->panId()) {
            std::printf("      fixtures are not on a shared pan\n");
            return false;
        }

        IqHarness h;
        QObject::connect(&model.daxIqModel(), &DaxIqModel::commandReady,
                         [&h](const QString& c) { h.iqCommands.append(c); });

        if (!server.start(0) || !server.m_server) {
            std::printf("      TCI server failed to bind an ephemeral port\n");
            return false;
        }
        QWebSocket client;
        QObject::connect(&client, &QWebSocket::binaryMessageReceived,
                         [&h](const QByteArray& f) { h.frames.append(f); });
        client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.port())));
        for (int i = 0; i < 100
             && (client.state() != QAbstractSocket::ConnectedState
                 || server.m_clients.isEmpty()); ++i) {
            spin(10);
        }
        if (server.m_clients.isEmpty()) return false;

        client.sendTextMessage(QStringLiteral("iq_start:0;iq_start:1;"));
        for (int i = 0; i < 100
             && server.m_clients.first().iqReceivers.size() != 2; ++i) {
            spin(10);
        }
        if (server.m_clients.first().iqReceivers != QSet<int>{0, 1}) {
            std::printf("      shared-pan receivers=%lld\n",
                        static_cast<long long>(
                            server.m_clients.first().iqReceivers.size()));
            return false;
        }
        // Exactly one stream, on channel 1, and no second pan bind that would
        // have stolen it back.
        if (h.iqCommands.filter(QStringLiteral("stream create type=dax_iq")).size() != 1
            || !h.iqCommands.contains(
                   QStringLiteral("stream create type=dax_iq daxiq_channel=1"))) {
            std::printf("      shared pan created %d streams\n",
                        static_cast<int>(h.iqCommands
                            .filter(QStringLiteral("stream create type=dax_iq")).size()));
            return false;
        }
        if (cmdLog.contains(
                QStringLiteral("display pan set 0x40000000 daxiq_channel=2"))) {
            std::printf("      second receiver stole the pan binding\n");
            return false;
        }

        // One payload on that one channel feeds both receivers, each stamped
        // with its own receiver index.
        float iqPair[2]{0.25f, -0.5f};
        QByteArray rawPayload(reinterpret_cast<const char*>(iqPair),
                              static_cast<int>(sizeof(iqPair)));
        server.onIqDataReady(1, rawPayload, 48000);
        for (int i = 0; i < 100 && h.frames.size() < 2; ++i) {
            spin(10);
        }
        if (frameReceivers(h.frames) != QSet<int>{0, 1}) {
            std::printf("      shared-channel frame receivers=%lld\n",
                        static_cast<long long>(frameReceivers(h.frames).size()));
            return false;
        }

        // Stopping one must NOT tear down the channel the other still uses.
        cmdLog.clear();
        client.sendTextMessage(QStringLiteral("iq_stop:0;"));
        for (int i = 0; i < 100
             && server.m_clients.first().iqReceivers.contains(0); ++i) {
            spin(10);
        }
        if (!server.m_tciIqChannels.contains(1)
            || cmdLog.contains(
                   QStringLiteral("display pan set 0x40000000 daxiq_channel=0"))) {
            std::printf("      shared channel released while still in use\n");
            return false;
        }
        h.frames.clear();
        server.onIqDataReady(1, rawPayload, 48000);
        for (int i = 0; i < 100 && h.frames.isEmpty(); ++i) {
            spin(10);
        }
        return frameReceivers(h.frames) == QSet<int>{1};
    }

    // A DAX IQ channel the operator's applet already owns is never taken over.
    // Binding it to the TCI slice's pan makes the radio push daxiq_channel=0 to
    // the operator's pan and stops their consumer at the source, and nothing
    // ever puts it back — so refuse, and SAY so rather than going silent.
    static bool borrowedIqChannelIsRefused()
    {
        ScopedCommandLog cmdLog;
        RadioModel model;
        TciServer server(&model);

        QString error;
        if (!model.automationApplySliceFixture(0, QString(), &error)) {
            return false;
        }
        // The applet owns channel 1: the stream exists and TCI did not make it.
        QMap<QString, QString> kvs;
        kvs.insert(QStringLiteral("daxiq_channel"), QStringLiteral("1"));
        kvs.insert(QStringLiteral("daxiq_rate"), QStringLiteral("192000"));
        model.daxIqModel().applyStreamStatus(0x2000abcd, kvs);
        if (!model.daxIqModel().stream(1).exists) {
            std::printf("      applet stream fixture did not take\n");
            return false;
        }

        IqHarness h;
        QObject::connect(&model.daxIqModel(), &DaxIqModel::commandReady,
                         [&h](const QString& c) { h.iqCommands.append(c); });

        if (!server.start(0) || !server.m_server) {
            std::printf("      TCI server failed to bind an ephemeral port\n");
            return false;
        }
        QStringList replies;
        QWebSocket client;
        QObject::connect(&client, &QWebSocket::textMessageReceived,
                         [&replies](const QString& m) { replies.append(m); });
        client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.port())));
        for (int i = 0; i < 100
             && (client.state() != QAbstractSocket::ConnectedState
                 || server.m_clients.isEmpty()); ++i) {
            spin(10);
        }
        if (server.m_clients.isEmpty()) return false;
        replies.clear();

        client.sendTextMessage(QStringLiteral("iq_start:0;"));
        for (int i = 0; i < 60; ++i) {
            spin(10);
        }
        if (!server.m_clients.first().iqReceivers.isEmpty()) {
            std::printf("      refused start still subscribed the receiver\n");
            return false;
        }
        // The operator's pan binding and rate are untouched...
        if (cmdLog.contains(QStringLiteral("display pan set"))) {
            std::printf("      borrow retargeted the operator's pan\n");
            return false;
        }
        if (!h.iqCommands.isEmpty()) {
            std::printf("      borrow touched the applet's stream: %s\n",
                        h.iqCommands.first().toUtf8().constData());
            return false;
        }
        // ...and the refusal ANSWERS, so a blocked skimmer does not hang.
        for (const QString& r : std::as_const(replies)) {
            if (r.contains(QStringLiteral("iq_stop:0;"))) {
                return true;
            }
        }
        std::printf("      refused start sent no reply\n");
        return false;
    }

    // Ownership, in-flight-create and pending-removal are three separate facts.
    // Conflating them wedged a receiver whose stream create was never confirmed:
    // repeated iq_start could not re-create it (contradicting the documented
    // re-arm), and a stop()/start() cycle — TciApplet's enable toggle and port
    // change both do exactly that on this same object — carried the stale claim
    // into the next session, so no client could ever get IQ again.
    static bool iqStreamRearmsAfterStopStartCycle()
    {
        RadioModel model;
        TciServer server(&model);

        QString error;
        if (!model.automationApplySliceFixture(0, QString(), &error)) {
            return false;
        }
        QStringList iqCommands;
        QObject::connect(&model.daxIqModel(), &DaxIqModel::commandReady,
                         [&iqCommands](const QString& c) { iqCommands.append(c); });

        // Deliberately NOT registered in m_clients: stop() closes and deletes
        // every registered socket, and this test only needs the subscription
        // bookkeeping that startIqForClient drives.
        TciServer::ClientState cs;
        const auto startOnce = [&](TciServer& srv) {
            return srv.startIqForClient(cs, 0);
        };

        if (!server.start(0) || !server.m_server) {
            std::printf("      TCI server failed to bind an ephemeral port\n");
            return false;
        }
        if (!startOnce(server)
            || !iqCommands.contains(
                   QStringLiteral("stream create type=dax_iq daxiq_channel=1"))) {
            std::printf("      first iq_start did not create a stream\n");
            return false;
        }
        // The radio never confirms the create, so stream(1).exists stays false.
        // A repeated iq_start must not re-issue a create while that one is still
        // outstanding — but it must not be wedged by it either.
        server.stop();
        iqCommands.clear();

        if (!server.start(0) || !server.m_server) {
            return false;
        }
        if (!startOnce(server)) {
            std::printf("      iq_start refused after a stop()/start() cycle\n");
            return false;
        }
        if (!iqCommands.contains(
                QStringLiteral("stream create type=dax_iq daxiq_channel=1"))) {
            std::printf("      receiver wedged: no create after restart\n");
            return false;
        }
        return true;
    }

    // Receiver resources are refcounted across clients. One client's stop must
    // not remove a stream another still consumes; only the last stop releases
    // it, and a stop racing the create status stays pending until an id exists.
    static bool sharedIqSubscriptionIsClientScoped()
    {
        RadioModel model;
        TciServer server(&model);

        QString error;
        if (!model.automationApplySliceFixture(0, QString(), &error)) {
            return false;
        }
        QWebSocket clientA;
        QWebSocket clientB;
        TciServer::ClientState a;
        a.socket = &clientA;
        TciServer::ClientState b;
        b.socket = &clientB;
        server.m_clients.append(a);
        server.m_clients.append(b);
        if (!server.startIqForClient(server.m_clients[0], 0)
            || !server.startIqForClient(server.m_clients[1], 0)) {
            std::printf("      shared start refused\n");
            return false;
        }
        if (!server.m_tciIqChannels.contains(1)
            || server.m_iqPanChannel.value(model.slice(0)->panId()) != 1) {
            return false;
        }

        server.stopIqForClient(server.m_clients[0], 0);
        if (!server.m_tciIqChannels.contains(1)
            || server.m_pendingIqRemovals.contains(1)
            || !server.m_clients[1].iqReceivers.contains(0)) {
            std::printf("      one client's stop released a shared channel\n");
            return false;
        }

        // Last subscriber leaves. The radio never confirmed the create, so the
        // removal parks until the status lands — and the pan binding is dropped
        // now rather than being left routed at a channel nothing reads.
        server.stopIqForClient(server.m_clients[1], 0);
        return server.m_pendingIqRemovals.contains(1)
            && server.m_iqPanChannel.isEmpty();
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
    const bool payloadFreeDisconnect
        = AetherSDR::TciServerReviewTest::disconnectSnapshotIsPayloadFreeAndUsesUnsetError();
    const bool outboundTextAccounting
        = AetherSDR::TciServerReviewTest::outboundTextAccountingIsCentralized();
    const bool pttBindsReceiver
        = AetherSDR::TciServerReviewTest::pttBindsToTheDeclaredAudioReceiver();
    const bool pttUsesStableMap
        = AetherSDR::TciServerReviewTest::pttResolvesThroughTheStableMap();
    const bool powerRateLimits
        = AetherSDR::TciServerReviewTest::powerBroadcastRateLimits();
    const bool trxCacheHolds
        = AetherSDR::TciServerReviewTest::driveTrxSurvivesTxFlagClear();
    const bool cacheResets
        = AetherSDR::TciServerReviewTest::powerCacheResetsWhenClientsLeave();
    const bool flagSeedsDeDups
        = AetherSDR::TciServerReviewTest::flagRelaySeedsAndDeDups();
    const bool flagSeedSettled
        = AetherSDR::TciServerReviewTest::flagSeedReadsSettledNotTransient();
    const bool vfoConfirmsAccepted
        = AetherSDR::TciServerReviewTest::vfoSetConfirmsTheAcceptedFrequency();
    const bool vfoConfirmsRefusal
        = AetherSDR::TciServerReviewTest::vfoSetConfirmsTruthWhenRefused();
    const bool vfoAcksNoOp
        = AetherSDR::TciServerReviewTest::vfoSetAcknowledgesANoOp();
    const bool vfoChannelZeroOnce
        = AetherSDR::TciServerReviewTest::vfoSetChannelZeroBroadcastsExactlyOnce();
    const bool txTrxResets
        = AetherSDR::TciServerReviewTest::lastTxTrxResetsOnClose();
    const bool activeSliceSeed
        = AetherSDR::TciServerReviewTest::activeSliceSeedsFromCurrentState();
    const bool activeSliceRemoval
        = AetherSDR::TciServerReviewTest::activeSliceFollowsSliceRemoval();
    const bool trxStableRecreate
        = AetherSDR::TciServerReviewTest::trxStableAcrossSliceRecreate();
    const bool trxDistinctReconnect
        = AetherSDR::TciServerReviewTest::liveSlicesKeepDistinctTrxAfterReconnect();
    const bool heldTrxRefuses
        = AetherSDR::TciServerReviewTest::heldTrxRefusesInsteadOfRepointing();
    const bool routeLogStaleCache
        = AetherSDR::TciServerReviewTest::pttRouteLogExposesStaleCache();
    const bool routeLogSampleOrder
        = AetherSDR::TciServerReviewTest::pttRouteLogSamplesCacheBeforeResolve();
    const bool routeLogSanitizes
        = AetherSDR::TciServerReviewTest::pttRouteLogSanitizesClientSource();
    const bool native24kPayload
        = AetherSDR::TciServerReviewTest::native24kAudioRetainsAccumulatedPayload();
    const bool fourIqStreams
        = AetherSDR::TciServerReviewTest::fourPansGiveFourIqStreams();
    const bool sharedPanChannel
        = AetherSDR::TciServerReviewTest::receiversSharingAPanShareOneChannel();
    const bool borrowRefused
        = AetherSDR::TciServerReviewTest::borrowedIqChannelIsRefused();
    const bool iqRearms
        = AetherSDR::TciServerReviewTest::iqStreamRearmsAfterStopStartCycle();
    const bool iqClientScoped
        = AetherSDR::TciServerReviewTest::sharedIqSubscriptionIsClientScoped();

    std::printf("%s  isolated settings profile\n",
                validProfile ? "PASS" : "FAIL");
    std::printf("%s  deferred TRX abort is client-scoped\n",
                deferredAbort ? "PASS" : "FAIL");
    std::printf("%s  VFO-B route failure is observable\n",
                observableFailure ? "PASS" : "FAIL");
    std::printf("%s  disconnect snapshot omits peer text and uses unset error\n",
                payloadFreeDisconnect ? "PASS" : "FAIL");
    std::printf("%s  outbound TCI text accounting is centralized\n",
                outboundTextAccounting ? "PASS" : "FAIL");
    std::printf("%s  PTT binds to the declared audio receiver (#4547)\n",
                pttBindsReceiver ? "PASS" : "FAIL");
    std::printf("%s  PTT resolves through the stable trx map (#4547/#4567)\n",
                pttUsesStableMap ? "PASS" : "FAIL");
    std::printf("%s  drive: rate-limits and de-dups\n",
                powerRateLimits ? "PASS" : "FAIL");
    std::printf("%s  drive: trx survives a TX-flag clear\n",
                trxCacheHolds ? "PASS" : "FAIL");
    std::printf("%s  power cache resets when clients leave\n",
                cacheResets ? "PASS" : "FAIL");
    std::printf("%s  flag relay seeds and de-dups\n",
                flagSeedsDeDups ? "PASS" : "FAIL");
    std::printf("%s  flag seed reads settled, no band-change transient\n",
                flagSeedSettled ? "PASS" : "FAIL");
    std::printf("%s  m_lastTxTrx resets when TX slice is closed\n",
                txTrxResets ? "PASS" : "FAIL");
    std::printf("%s  active_slice seeds from current GUI focus (#4160)\n",
                activeSliceSeed ? "PASS" : "FAIL");
    std::printf("%s  active_slice holds trx / clears on slice removal (#4160/#4567)\n",
                activeSliceRemoval ? "PASS" : "FAIL");
    std::printf("%s  receiver numbers stable across slice recreate (#4567)\n",
                trxStableRecreate ? "PASS" : "FAIL");
    std::printf("%s  live slices keep distinct trx after reconnect (#4577)\n",
                trxDistinctReconnect ? "PASS" : "FAIL");
    std::printf("%s  held trx refuses instead of re-pointing in the settle "
                "window (#4577)\n",
                heldTrxRefuses ? "PASS" : "FAIL");
    std::printf("%s  vfo: SET confirms the frequency reached (#4500/#4493)\n",
                vfoConfirmsAccepted ? "PASS" : "FAIL");
    std::printf("%s  vfo: SET confirms the truth when the tune is refused\n",
                vfoConfirmsRefusal ? "PASS" : "FAIL");
    std::printf("%s  vfo: SET still acknowledges a no-op\n",
                vfoAcksNoOp ? "PASS" : "FAIL");
    std::printf("%s  vfo: SET on channel 0 broadcasts exactly once per move "
                "(#5086)\n",
                vfoChannelZeroOnce ? "PASS" : "FAIL");
    std::printf("%s  PTT route log shows the cache disagreeing with live TX "
                "(#4547)\n",
                routeLogStaleCache ? "PASS" : "FAIL");
    std::printf("%s  PTT route log samples the cache before the resolve "
                "(#4547)\n",
                routeLogSampleOrder ? "PASS" : "FAIL");
    std::printf("%s  PTT route log neutralises a client-supplied source "
                "(#4547)\n",
                routeLogSanitizes ? "PASS" : "FAIL");
    std::printf("%s  native 24 kHz TCI RX retains accumulated payload (#4744)\n",
                native24kPayload ? "PASS" : "FAIL");
    std::printf("%s  four pans give one TCI client four independent IQ streams\n",
                fourIqStreams ? "PASS" : "FAIL");
    std::printf("%s  receivers sharing a pan share one DAX IQ channel\n",
                sharedPanChannel ? "PASS" : "FAIL");
    std::printf("%s  an applet-owned DAX IQ channel is refused, not borrowed\n",
                borrowRefused ? "PASS" : "FAIL");
    std::printf("%s  IQ re-arms after a stop()/start() cycle\n",
                iqRearms ? "PASS" : "FAIL");
    std::printf("%s  shared IQ subscriptions are client-scoped\n",
                iqClientScoped ? "PASS" : "FAIL");

    return validProfile && deferredAbort && observableFailure
        && payloadFreeDisconnect && outboundTextAccounting && pttBindsReceiver
        && pttUsesStableMap
        && powerRateLimits && trxCacheHolds && cacheResets && flagSeedsDeDups
        && flagSeedSettled && txTrxResets
        && activeSliceSeed && activeSliceRemoval && trxStableRecreate
        && trxDistinctReconnect && heldTrxRefuses
        && vfoConfirmsAccepted && vfoConfirmsRefusal && vfoAcksNoOp
        && vfoChannelZeroOnce
        && routeLogStaleCache && routeLogSampleOrder && routeLogSanitizes
        && native24kPayload && fourIqStreams && sharedPanChannel
        && borrowRefused && iqRearms && iqClientScoped
        ? 0 : 1;
}
