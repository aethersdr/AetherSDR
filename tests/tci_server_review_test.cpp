#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/TciServer.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QJsonObject>
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
    const bool activeSliceSeed
        = AetherSDR::TciServerReviewTest::activeSliceSeedsFromCurrentState();

    std::printf("%s  isolated settings profile\n",
                validProfile ? "PASS" : "FAIL");
    std::printf("%s  deferred TRX abort is client-scoped\n",
                deferredAbort ? "PASS" : "FAIL");
    std::printf("%s  VFO-B route failure is observable\n",
                observableFailure ? "PASS" : "FAIL");
    std::printf("%s  active_slice seeds from current GUI focus (#4160)\n",
                activeSliceSeed ? "PASS" : "FAIL");

    return validProfile && deferredAbort && observableFailure
        && activeSliceSeed ? 0 : 1;
}
