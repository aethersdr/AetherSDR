#include "core/TciProtocol.h"
#include "core/TciRoutingState.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>

using namespace AetherSDR;

namespace
{

bool check(bool condition, const char* message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "%s\n", message);
    return false;
}

bool testRoutingPolicy()
{
    using Action = TciRoutingState::RouteAction;
    using Owner = TciRoutingState::TxRouteOwner;

    TciRoutingState routing;
    QVector<TciSliceEndpoint> oneSlice { { 4, true } };
    auto decision = routing.resolveVfoB(4, oneSlice);
    if (!check(decision.action == Action::Create,
            "single-slice VFO B must request a distinct TX slice")) {
        return false;
    }

    QVector<TciSliceEndpoint> satellite { { 4, false }, { 7, true } };
    decision = routing.resolveVfoB(4, satellite);
    if (!check(decision.action == Action::UseExisting && decision.txSliceId == 7
                && decision.owner == Owner::External,
            "external satellite TX slice must be preserved")) {
        return false;
    }
    if (!check(routing.resolvePttSlice(4, satellite) == 7,
            "PTT must resolve to the externally selected TX slice")) {
        return false;
    }
    if (!check(!routing.setSplitRequested(false)
                && routing.owner() == Owner::External
                && routing.txSliceId() == 7,
            "steady split false must not reclaim an external TX route")) {
        return false;
    }

    routing.reset();
    QVector<TciSliceEndpoint> independentReceiver { { 4, true }, { 7, false } };
    decision = routing.resolveVfoB(4, independentReceiver);
    if (!check(decision.action == Action::Create && decision.txSliceId < 0
                && decision.owner == Owner::TciCreated,
            "VFO B must not commandeer an independent second receiver")) {
        return false;
    }
    if (!check(!routing.setSplitRequested(false)
                && !routing.ownsRoute()
                && routing.txSliceId() < 0,
            "steady split false must leave the independent receiver untouched")) {
        return false;
    }

    if (!check(routing.setSplitRequested(true), "split false-to-true must be an edge")) {
        return false;
    }
    if (!check(!routing.setSplitRequested(true), "duplicate split true must be idempotent")) {
        return false;
    }
    if (!check(routing.setSplitRequested(false) && !routing.ownsRoute(),
            "split disable without a route must remain ownership-free")) {
        return false;
    }

    routing.bindCreatedRoute(4, 7);
    routing.removeSlice(4);
    if (!check(routing.txSliceId() < 0 && !routing.splitRequested(),
            "removing the stable RX slice must invalidate the route")) {
        return false;
    }
    return true;
}

bool testWsjtxRoutingContracts()
{
    using Action = TciRoutingState::RouteAction;
    using Owner = TciRoutingState::TxRouteOwner;

    // WSJT-X programs VFO B after explicitly reporting split false. That
    // steady false is compatibility state, not permission to erase VFO B.
    TciRoutingState singleRoute;
    TciProtocol singleProtocol(nullptr, &singleRoute);
    singleProtocol.handleCommand(QStringLiteral("split_enable:0,false"));
    const auto singleSplit = singleProtocol.takeSplitRequest();
    if (!check(singleSplit && !singleSplit->enabled
                && !singleRoute.setSplitRequested(singleSplit->enabled),
            "WSJT-X single-slice steady split false must be a no-op")) {
        return false;
    }
    singleProtocol.handleCommand(QStringLiteral("vfo:0,1,14076000"));
    const auto singleVfo = singleProtocol.takeVfoRequest();
    QVector<TciSliceEndpoint> singleTopology { { 4, true } };
    const auto singleDecision = singleRoute.resolveVfoB(4, singleTopology);
    if (!check(singleVfo && singleVfo->channel == 1
                && singleVfo->frequencyHz == 14076000
                && singleDecision.action == Action::Create,
            "WSJT-X single-slice VFO B must create a distinct TX route")) {
        return false;
    }
    singleRoute.bindCreatedRoute(4, 9);
    QVector<TciSliceEndpoint> createdTopology { { 4, false }, { 9, true } };
    if (!check(singleRoute.owner() == Owner::TciCreated
                && singleRoute.resolvePttSlice(4, createdTopology) == 9,
            "WSJT-X single-slice TRX must key the created VFO B slice")) {
        return false;
    }
    singleProtocol.handleCommand(QStringLiteral("trx:0,true,tci"));
    const auto singleTrx = singleProtocol.takeTrxRequest();
    if (!check(singleTrx && singleTrx->transmitting,
            "WSJT-X single-slice TRX request must survive route setup")) {
        return false;
    }

    // A second non-TX slice may be an operator's independent receiver. WSJT-X
    // must leave it untouched and create a separately owned VFO B route.
    TciRoutingState multiRoute;
    TciProtocol multiProtocol(nullptr, &multiRoute);
    multiProtocol.handleCommand(QStringLiteral("split_enable:0,false"));
    const auto multiSplit = multiProtocol.takeSplitRequest();
    QVector<TciSliceEndpoint> occupiedTopology { { 4, true }, { 7, false } };
    const auto occupiedDecision = multiRoute.resolveVfoB(4, occupiedTopology);
    if (!check(multiSplit && !multiRoute.setSplitRequested(multiSplit->enabled)
                && occupiedDecision.action == Action::Create,
            "WSJT-X multi-slice VFO B must preserve an independent receiver")) {
        return false;
    }
    multiProtocol.handleCommand(QStringLiteral("vfo:0,1,14076000"));
    const auto multiVfo = multiProtocol.takeVfoRequest();
    multiRoute.bindCreatedRoute(4, 9);
    QVector<TciSliceEndpoint> createdMultiTopology {
        { 4, false }, { 7, false }, { 9, true }
    };
    if (!check(multiVfo && multiVfo->channel == 1
                && multiRoute.owner() == Owner::TciCreated
                && multiRoute.resolvePttSlice(4, createdMultiTopology) == 9,
            "WSJT-X multi-slice TRX must key the new route, not the other receiver")) {
        return false;
    }

    // Satellite controllers own the selected TX slice. TCI may tune and key
    // it, but a steady false split report must never move TX back to RX.
    TciRoutingState externalRoute;
    QVector<TciSliceEndpoint> satelliteTopology { { 4, false }, { 7, true } };
    const auto externalDecision = externalRoute.resolveVfoB(4, satelliteTopology);
    if (!check(externalDecision.action == Action::UseExisting
                && externalDecision.owner == Owner::External
                && !externalRoute.setSplitRequested(false)
                && externalRoute.resolvePttSlice(4, satelliteTopology) == 7,
            "WSJT-X external multi-slice route must preserve satellite TX ownership")) {
        return false;
    }

    return true;
}

bool testDeferredCommands()
{
    TciRoutingState routing;
    TciProtocol protocol(nullptr, &routing);

    if (!check(protocol.handleCommand(QStringLiteral("vfo:0,1,14074000")).isEmpty(),
            "VFO SET must not be acknowledged by the parser")) {
        return false;
    }
    const auto vfo = protocol.takeVfoRequest();
    if (!check(vfo && vfo->trx == 0 && vfo->channel == 1 && vfo->frequencyHz == 14074000,
            "VFO B SET must preserve trx/channel/frequency")) {
        return false;
    }
    if (!check(!protocol.takeVfoRequest(), "VFO request must be consumed exactly once")) {
        return false;
    }
    protocol.handleCommand(QStringLiteral("vfo:0,2,14074000"));
    if (!check(!protocol.takeVfoRequest(),
            "VFO SET must reject channels outside the advertised pair")) {
        return false;
    }

    protocol.handleCommand(QStringLiteral("split_enable:0,true"));
    const auto split = protocol.takeSplitRequest();
    if (!check(split && split->trx == 0 && split->enabled,
            "split SET must be deferred with explicit requested state")) {
        return false;
    }

    protocol.handleCommand(QStringLiteral("trx:0,true,tci"));
    const auto trx = protocol.takeTrxRequest();
    if (!check(trx && trx->trx == 0 && trx->transmitting && trx->source == QStringLiteral("tci"),
            "TRX SET must preserve source and await radio confirmation")) {
        return false;
    }

    if (!check(protocol.handleCommand(QStringLiteral("tx_enable:0,true")).isEmpty()
                && protocol.pendingNotification().isEmpty(),
            "incoming TX_ENABLE must not mutate server state")) {
        return false;
    }
    return true;
}

bool testDriveWireContract()
{
    TciProtocol protocol(nullptr);
    struct PowerCommand {
        const char* name;
    };
    const PowerCommand commands[] = {
        { "drive" },
        { "tune_drive" },
    };

    for (const PowerCommand& command : commands) {
        const QString name = QString::fromLatin1(command.name);

        const QString legacyRead = protocol.handleCommand(name);
        if (!check(legacyRead == QStringLiteral("%1:0,0;").arg(name),
                "legacy power read must emit trx,power")) {
            return false;
        }

        const QString specRead = protocol.handleCommand(
            QStringLiteral("%1:1").arg(name));
        if (!check(specRead == QStringLiteral("%1:1,0;").arg(name)
                    && protocol.pendingNotification().isEmpty(),
                "one-argument power command must be a non-mutating TRX read")) {
            return false;
        }

        if (!check(protocol.handleCommand(QStringLiteral("%1:1,73").arg(name)).isEmpty()
                    && protocol.pendingNotification()
                        == QStringLiteral("%1:1,73;").arg(name),
                "two-argument power SET must notify with exact trx,power shape")) {
            return false;
        }

        protocol.handleCommand(QStringLiteral("%1:1,101").arg(name));
        if (!check(protocol.pendingNotification().isEmpty(),
                "out-of-range power SET must be rejected")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    TciProtocol protocol(nullptr);

    const QString response = protocol.handleCommand(
        QStringLiteral("iq_samplerate:44100"));
    if (response != QStringLiteral("iq_samplerate:48000;")) {
        std::fprintf(stderr,
                     "unsupported iq_samplerate should report the current rate; got %s\n",
                     response.toUtf8().constData());
        return 1;
    }

    if (!protocol.pendingNotification().isEmpty()) {
        std::fprintf(stderr,
                     "rejected iq_samplerate must not notify other clients\n");
        return 1;
    }

    const QStringList greeting = protocol.generateInitBurst().split(
        QLatin1Char(';'), Qt::SkipEmptyParts);
    const int readyIndex = greeting.indexOf(QStringLiteral("ready"));
    const int iqRateIndex = greeting.indexOf(QStringLiteral("iq_samplerate:48000"));
    const int startIndex = greeting.indexOf(QStringLiteral("start"));
    const int channelsIndex = greeting.indexOf(QStringLiteral("channels_count:2"));
    if (readyIndex < 0 || iqRateIndex < 0 || startIndex < 0 || channelsIndex < 0
        || iqRateIndex >= readyIndex || readyIndex >= startIndex) {
        std::fprintf(stderr,
            "TCI greeting must advertise two channels and order "
            "iq_samplerate before ready before start\n");
        return 1;
    }

    for (const QString& command : greeting) {
        if (command.startsWith(QStringLiteral("audio_start"))
            || command.startsWith(QStringLiteral("iq_start"))) {
            std::fprintf(stderr,
                         "TCI greeting must not emit client-owned stream command: %s\n",
                         command.toUtf8().constData());
            return 1;
        }
    }

    if (!testRoutingPolicy() || !testWsjtxRoutingContracts()
        || !testDeferredCommands() || !testDriveWireContract()) {
        return 1;
    }

    std::printf("tci_protocol_test: all checks passed\n");
    return 0;
}
