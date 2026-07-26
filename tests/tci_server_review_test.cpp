#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/TciServer.h"
#include "core/backends/SliceDelta.h"
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

    // #4160 — trx is positional, so removing a slice renumbers every later one
    // while the focused slice emits nothing (it never lost focus). Unlike
    // vfo:/modulation: there is no follow-up event to self-correct, so the
    // sliceRemoved → publishActiveTrx() path is the only thing keeping the
    // tracked trx (and every client seeded from it) pointing at the right
    // slice. Previously covered only on hardware.
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

        // Remove the EARLIER, unfocused slice. Focus stays with B, but B's
        // positional trx renumbers 1 → 0. Nothing re-fires activeChanged, so
        // only the removal hook can correct it.
        if (!model.automationRemoveSliceFixture(0, &error)) {
            std::fprintf(stderr, "remove slice 0 failed: %s\n",
                         error.toUtf8().constData());
            return false;
        }
        if (server.m_activeTrx != 0
            || server.m_activeLetter != QStringLiteral("B")) {
            std::fprintf(stderr,
                         "after removing trx 0: expected focus B renumbered to trx 0, "
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

    // ── #4493 VFO tune completion contract ──────────────────────────────────

    // Shared rig: one wired slice plus a capture of every outbound vfo: line.
    // sendCmdPublic() is deliberately never reached — RadioModel::m_connection
    // is null on an unconnected model and sendCmd() does not guard it — so
    // these exercise the completion contract directly.
    struct TuneRig {
        RadioModel model;
        TciServer server { &model };
        QWebSocket sock;
        QStringList vfos;

        bool setUp()
        {
            TciServer::ClientState cs;
            cs.socket = &sock;
            server.m_clients.append(cs);
            QObject::connect(&server, &TciServer::tciMessage,
                [this](const QString& dir, const QString& msg) {
                    if (dir == QLatin1String("tx")
                        && msg.startsWith(QLatin1String("vfo:")))
                        vfos << msg.trimmed();
                });
            QString error;
            if (!model.automationApplySliceFixture(0, QStringLiteral("A"),
                                                   &error)) {
                std::fprintf(stderr, "tune fixture failed: %s\n",
                             error.toUtf8().constData());
                return false;
            }
            server.wireSlice(0, model.slices().value(0));
            return model.slices().value(0) != nullptr;
        }

        SliceModel* slice() { return model.slices().value(0); }
        long long modelHz()
        {
            return TciProtocol::mhzToHz(slice()->frequency());
        }
    };

    // An accepted tune confirms the REQUESTED coordinate and moves the model
    // with it. The pre-#4493 read-back barrier echoed the pre-tune value here,
    // so a client could never accumulate a relative change.
    static bool acceptedTuneEchoesRequestedFrequency()
    {
        TuneRig rig;
        if (!rig.setUp()) return false;
        const int sliceId = rig.slice()->sliceId();

        const long long preTune = rig.modelHz();
        rig.server.handleTuneReply(
            0, 0, sliceId,
            TciServer::TuneAttempt{ .requestedHz = 14076000,
                                    .preTuneHz = preTune },
            0, QString());

        if (rig.modelHz() != 14076000) {
            std::fprintf(stderr,
                         "accepted tune: model did not adopt, got %lld\n",
                         rig.modelHz());
            return false;
        }
        if (!rig.vfos.contains(QStringLiteral("vfo:0,0,14076000;"))) {
            std::fprintf(stderr, "accepted tune: no confirmation, saw [%s]\n",
                         rig.vfos.join(QLatin1Char(' ')).toUtf8().constData());
            return false;
        }
        // The channel-0 relay already covers this coordinate; confirming it a
        // second time would double every dial step on the wire.
        const int dupes = rig.vfos.count(QStringLiteral("vfo:0,0,14076000;"));
        if (dupes != 1) {
            std::fprintf(stderr,
                         "accepted tune: expected 1 confirmation, got %d\n",
                         dupes);
            return false;
        }
        return true;
    }

    // When a rejection does supply a coordinate, it wins over the request. No
    // observed rejection actually does (see below); this covers the shape
    // #4493's triage asked for.
    static bool rejectedTuneEchoesReplyBody()
    {
        TuneRig rig;
        if (!rig.setUp()) return false;
        const int sliceId = rig.slice()->sliceId();

        const long long preTune = rig.modelHz();
        rig.vfos.clear();

        // Radio refuses the requested 14.076 and reports where it actually is.
        rig.server.handleTuneReply(
            0, 0, sliceId,
            TciServer::TuneAttempt{ .requestedHz = 14076000,
                                    .preTuneHz = preTune },
            0x50000015, QStringLiteral("14.074000"));

        if (rig.modelHz() != 14074000) {
            std::fprintf(stderr,
                         "rejected tune: model not corrected, got %lld\n",
                         rig.modelHz());
            return false;
        }
        if (!rig.vfos.contains(QStringLiteral("vfo:0,0,14074000;"))) {
            std::fprintf(stderr,
                         "rejected tune: reply-body value not broadcast, saw [%s]\n",
                         rig.vfos.join(QLatin1Char(' ')).toUtf8().constData());
            return false;
        }

        return true;
    }

    // A rejection carrying no coordinate must report the PRE-tune frequency.
    // This is what real rejections look like: across every captured session
    // every non-zero reply body is prose or empty. Both observed shapes are
    // covered — a FLEX-6600 refusing a tune on a locked slice answers
    //   50000068|Unable to tune a locked slice -- unlock first
    // and most error codes answer with an empty body.
    static bool rejectedTuneWithUnusableBodyRestoresPreTune()
    {
        TuneRig rig;
        if (!rig.setUp()) return false;
        const int sliceId = rig.slice()->sliceId();
        const long long preTune = rig.modelHz();

        rig.vfos.clear();

        // Verbatim from the radio (FLEX-6600, v4.x).
        rig.server.handleTuneReply(
            0, 0, sliceId,
            TciServer::TuneAttempt{ .requestedHz = preTune + 2000,
                                    .preTuneHz = preTune },
            0x50000068,
            QStringLiteral("Unable to tune a locked slice -- unlock first"));

        if (rig.modelHz() != preTune) {
            std::fprintf(stderr,
                         "radio rejection: model moved (%lld -> %lld)\n",
                         preTune, rig.modelHz());
            return false;
        }

        // An empty body — the most common error shape on the wire — takes the
        // same branch.
        rig.vfos.clear();
        rig.server.handleTuneReply(
            0, 0, sliceId,
            TciServer::TuneAttempt{ .requestedHz = preTune + 2000,
                                    .preTuneHz = preTune },
            0x50000061, QString());

        if (rig.modelHz() != preTune) {
            std::fprintf(stderr,
                         "unusable rejection body: model kept the refused value "
                         "(pre-tune %lld, now %lld)\n",
                         preTune, rig.modelHz());
            return false;
        }
        // And it must still say something — silence costs WSJT-X's
        // do_frequency() its 2 s timeout.
        const QString expected = QStringLiteral("vfo:0,0,%1;").arg(preTune);
        if (!rig.vfos.contains(expected)) {
            std::fprintf(stderr,
                         "unusable rejection body: expected %s, saw [%s]\n",
                         expected.toUtf8().constData(),
                         rig.vfos.join(QLatin1Char(' ')).toUtf8().constData());
            return false;
        }
        return true;
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
    const bool txTrxResets
        = AetherSDR::TciServerReviewTest::lastTxTrxResetsOnClose();
    const bool activeSliceSeed
        = AetherSDR::TciServerReviewTest::activeSliceSeedsFromCurrentState();
    const bool activeSliceRemoval
        = AetherSDR::TciServerReviewTest::activeSliceFollowsSliceRemoval();
    const bool tuneEchoesRequested
        = AetherSDR::TciServerReviewTest::acceptedTuneEchoesRequestedFrequency();
    const bool tuneRejectionCorrects
        = AetherSDR::TciServerReviewTest::rejectedTuneEchoesReplyBody();
    const bool unusableBodyReverts
        = AetherSDR::TciServerReviewTest::rejectedTuneWithUnusableBodyRestoresPreTune();

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
    std::printf("%s  active_slice renumbers/clears on slice removal (#4160)\n",
                activeSliceRemoval ? "PASS" : "FAIL");
    std::printf("%s  accepted tune echoes the requested frequency (#4493)\n",
                tuneEchoesRequested ? "PASS" : "FAIL");
    std::printf("%s  rejected tune corrects from the reply body (#4493)\n",
                tuneRejectionCorrects ? "PASS" : "FAIL");
    std::printf("%s  unusable rejection body reverts to pre-tune (#4493)\n",
                unusableBodyReverts ? "PASS" : "FAIL");

    return validProfile && deferredAbort && observableFailure
        && powerRateLimits && trxCacheHolds && cacheResets && flagSeedsDeDups
        && flagSeedSettled && txTrxResets
        && activeSliceSeed && activeSliceRemoval
        && tuneEchoesRequested && tuneRejectionCorrects
        && unusableBodyReverts
        ? 0 : 1;
}
