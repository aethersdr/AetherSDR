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
#include <QEventLoop>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QWebSocket>

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
        const char* const required[] = {
            "trx=1",
            "rxSlice=1(trx1)",
            "-> txSlice=0(trx0)",
            "(NOT the requested slice)",
            "liveTx=1(trx1)",       // transmit is really on slice 1...
            "cachedTx=0(trx0)",     // ...and the cache still says slice 0
            "owner=tci-created",
        };
        for (const char* needle : required) {
            if (!route.contains(QLatin1String(needle))) {
                std::fprintf(stderr, "route line missing %s\n  got: %s\n",
                             needle, route.toUtf8().constData());
                return false;
            }
        }
        // And the move away from the slice that actually holds transmit.
        return !findLine(log, "TCI PTT: transmit will move from slice 1(trx1) "
                              "to slice 0(trx0)")
                    .isEmpty();
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

        // trx 0 addresses slice 0, which is not the TX slice: the requested
        // receiver is resolved away onto slice 1. Defect 1 of #4547.
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
        QWebSocket client;

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
    const bool flagSeedSettled
        = AetherSDR::TciServerReviewTest::flagSeedReadsSettledNotTransient();
    const bool vfoConfirmsAccepted
        = AetherSDR::TciServerReviewTest::vfoSetConfirmsTheAcceptedFrequency();
    const bool vfoConfirmsRefusal
        = AetherSDR::TciServerReviewTest::vfoSetConfirmsTruthWhenRefused();
    const bool vfoAcksNoOp
        = AetherSDR::TciServerReviewTest::vfoSetAcknowledgesANoOp();
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
    std::printf("%s  PTT route log shows the cache disagreeing with live TX "
                "(#4547)\n",
                routeLogStaleCache ? "PASS" : "FAIL");
    std::printf("%s  PTT route log samples the cache before the resolve "
                "(#4547)\n",
                routeLogSampleOrder ? "PASS" : "FAIL");
    std::printf("%s  PTT route log neutralises a client-supplied source "
                "(#4547)\n",
                routeLogSanitizes ? "PASS" : "FAIL");

    return validProfile && deferredAbort && observableFailure
        && powerRateLimits && trxCacheHolds && cacheResets && flagSeedsDeDups
        && flagSeedSettled && txTrxResets
        && activeSliceSeed && activeSliceRemoval && trxStableRecreate
        && trxDistinctReconnect && heldTrxRefuses
        && vfoConfirmsAccepted && vfoConfirmsRefusal && vfoAcksNoOp
        && routeLogStaleCache && routeLogSampleOrder && routeLogSanitizes
        ? 0 : 1;
}
