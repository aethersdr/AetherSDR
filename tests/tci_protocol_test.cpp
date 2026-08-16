#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioDiscovery.h"
#include "core/TciProtocol.h"
#include "core/TciRoutingState.h"
#include "core/backends/sim/SimBackend.h"
#include "models/DaxIqModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QString>
#include <QTimer>

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

// #4547 secondary defect: a cached route that the live topology contradicts
// must never win under a bare PTT. The operator moves TX from the GUI; the
// cache is refreshed only in branch 1 and the VFO-B paths, so a client that
// addresses the slice actually holding TX skips branch 1 and — before the fix
// — keyed the stale slice's band and antenna.
bool testStaleRouteFailsSafe()
{
    using Owner = TciRoutingState::TxRouteOwner;

    // 1. WSJT-X negotiates a route while slice 0 holds TX.
    TciRoutingState routing;
    QVector<TciSliceEndpoint> bound { { 4, false }, { 0, true } };
    const auto decision = routing.resolveVfoB(4, bound);
    if (!check(decision.txSliceId == 0 && routing.txSliceId() == 0,
            "setup: VFO B must bind the externally selected TX slice")) {
        return false;
    }

    // 2. The operator moves TX to slice 1 from the GUI. Nothing refreshes the
    //    cache. 3. The client addresses the slice that actually holds TX —
    //    #4547's literal repro, which returned the stale slice 0.
    QVector<TciSliceEndpoint> moved { { 4, false }, { 0, false }, { 1, true } };
    if (!check(routing.resolvePttSlice(1, moved) == 1,
            "a stale cached route must not move TX off the live TX slice")) {
        return false;
    }

    // The same staleness reached through the client's own bound RX slice, where
    // the route does apply: the live TX slice still wins, and the cache is
    // refreshed so it cannot answer for a slice that no longer holds TX.
    TciRoutingState bound2;
    bound2.resolveVfoB(4, bound);
    if (!check(bound2.resolvePttSlice(4, moved) == 1 && bound2.txSliceId() == 1,
            "an applicable route must refresh against the live TX slice")) {
        return false;
    }

    // The refresh must also drop TCI ownership. handleSplitRequest() issues
    // `slice remove` for a TciCreated route on teardown, so carrying that
    // owner onto a slice TCI never created would delete an operator's slice.
    TciRoutingState created;
    created.bindCreatedRoute(4, 9);
    QVector<TciSliceEndpoint> operatorMoved { { 4, false }, { 9, false }, { 1, true } };
    if (!check(created.resolvePttSlice(4, operatorMoved) == 1
                && created.owner() != Owner::TciCreated,
            "a refreshed route must not stay TCI-owned on a foreign slice")) {
        return false;
    }

    // Backends that report no TX slice at all still fall back to the cache,
    // then to the requested slice — the Flex always-one-TX-slice invariant
    // does not hold on the seam backends (HL2).
    TciRoutingState noTx;
    QVector<TciSliceEndpoint> headless { { 4, false }, { 7, false } };
    noTx.bindCreatedRoute(4, 7);
    if (!check(noTx.resolvePttSlice(4, headless) == 7,
            "with no live TX slice the tracked route still answers")) {
        return false;
    }
    TciRoutingState bare;
    if (!check(bare.resolvePttSlice(4, headless) == 4,
            "with no route and no TX slice PTT resolves to the requested slice")) {
        return false;
    }
    return true;
}

// #4547 primary defect: a bare `trx:N,true` must key slice N. A Flex always
// marks exactly one TX slice, so the unconditional external-TX branch fired on
// every request whose slice was not already TX — the common path. Two WSJT-X
// instances on different slices both keyed whichever slice happened to hold TX.
bool testBarePttKeysTheRequestedSlice()
{
    // Operator left TX on slice 7. A client with no split and no bound route
    // asks for slice 4.
    QVector<TciSliceEndpoint> twoSlices { { 4, false }, { 7, true } };
    TciRoutingState bare;
    if (!check(bare.resolvePttSlice(4, twoSlices) == 4,
            "a bare PTT must key the requested slice, not the incidental TX slice")) {
        return false;
    }
    // ...and the client working slice 7 still keys 7.
    TciRoutingState other;
    if (!check(other.resolvePttSlice(7, twoSlices) == 7,
            "a second client must key its own slice")) {
        return false;
    }

    // Split is the signal that a distinct TX route is intended: once the client
    // asks for it, the externally selected TX slice wins again.
    TciRoutingState split;
    split.setSplitRequested(true);
    if (!check(split.resolvePttSlice(4, twoSlices) == 7,
            "a split client must still key the external TX slice")) {
        return false;
    }

    // A bare PTT must not adopt a route bound for a different RX slice, and
    // must stay stable when repeated. Recording the requested slice here would
    // pair it with the surviving m_txSliceId, so the SECOND identical request
    // would take the route branch and key slice 7 after the first keyed 5.
    QVector<TciSliceEndpoint> threeSlices {
        { 4, false }, { 5, false }, { 7, true }
    };
    TciRoutingState bound;
    bound.resolveVfoB(4, twoSlices);          // route bound for slice 4 → 7
    if (!check(bound.resolvePttSlice(5, threeSlices) == 5
                && bound.resolvePttSlice(5, threeSlices) == 5,
            "a bare PTT must not adopt another slice's route, even when repeated")) {
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

// #4523: "volume:" (colon, nothing after it) splits to a single empty-string
// argument, not an empty arg list, so it used to reach the SET branch and
// parse via toDouble() — an empty string silently becomes 0.0, and 0 dB is
// the loudest value in range, so malformed input landed on max volume. Fixed
// to check toDouble()'s ok-flag and drop the command instead, matching the
// "ignore silently" posture already used for unrecognised commands.
bool testVolumeMalformedInputIsDropped()
{
    TciProtocol protocol(nullptr);

    // Sanity: a real SET still works, so the fix isn't just refusing everything.
    protocol.handleCommand(QStringLiteral("volume:-12"));
    if (!check(protocol.pendingMasterVolume() >= 0
                   && !protocol.pendingNotification().isEmpty(),
            "a well-formed volume SET must still take effect")) {
        return false;
    }

    // The non-spec percent form (val >= 1.0) is load-bearing: AetherSDR's own
    // bundled Stream Deck plugin sends "volume:79", not a dB value (#4523).
    // Pinned here so a future tightening of this parser can't drop it silently.
    protocol.handleCommand(QStringLiteral("volume:79"));
    if (!check(protocol.pendingMasterVolume() == 79,
            "the legacy percent form must still be accepted as a percentage")) {
        return false;
    }

    // The bug's exact repro: colon, nothing after it.
    protocol.handleCommand(QStringLiteral("volume:"));
    if (!check(protocol.pendingMasterVolume() == -1,
            "\"volume:\" must not be recorded as a master-volume SET")) {
        return false;
    }
    if (!check(protocol.pendingNotification().isEmpty(),
            "\"volume:\" must not notify other clients of a volume change")) {
        return false;
    }

    // Same shape via the legacy trx-prefixed 2-arg form: "volume:0," is
    // trx=0, empty value — the value arg is what toDouble() silently ate.
    protocol.handleCommand(QStringLiteral("volume:0,"));
    if (!check(protocol.pendingMasterVolume() == -1,
            "\"volume:0,\" must not be recorded as a master-volume SET")) {
        return false;
    }

    // The ok flag alone does not cover this: Qt's toDouble() parses the
    // literals "inf"/"-inf"/"nan" and reports ok, so they satisfy the check
    // above and reach the arithmetic. "inf" takes the val >= 1.0 percent
    // branch, std::lround(+inf) is out of long's range, and the narrowing
    // cast lands on 0 — the malformed input would MUTE the radio and
    // broadcast a well-formed "volume:-60;". Same undetectable-from-the-wire
    // failure as "volume:", at the other end of the range.
    for (const auto& nonFinite : { QStringLiteral("volume:inf"),
                                   QStringLiteral("volume:-inf"),
                                   QStringLiteral("volume:nan"),
                                   QStringLiteral("volume:0,inf") }) {
        protocol.handleCommand(nonFinite);
        if (!check(protocol.pendingMasterVolume() == -1
                       && protocol.pendingNotification().isEmpty(),
                qPrintable(QStringLiteral("\"%1\" must not be recorded as a "
                                          "master-volume SET").arg(nonFinite)))) {
            return false;
        }
    }

    // Overflow already cleared the ok flag before this change; pinned so the
    // isfinite() check above can't be "simplified" back out on the theory
    // that toDouble() rejects everything non-finite. It rejects this one and
    // accepts the literals above.
    protocol.handleCommand(QStringLiteral("volume:1e400"));
    if (!check(protocol.pendingMasterVolume() == -1,
            "\"volume:1e400\" must not be recorded as a master-volume SET")) {
        return false;
    }

    // Bare "volume" (no colon at all) is the documented GET form and must be
    // unaffected — this is what distinguishes "no argument" from "malformed
    // argument", which is the whole point of the fix.
    const QString getResponse = protocol.handleCommand(QStringLiteral("volume"));
    if (!check(getResponse.startsWith(QStringLiteral("volume:")),
            "bare \"volume\" GET must still reply with the current level")) {
        return false;
    }
    return true;
}

// #4523 (triage item 2): cmdTxGain is the same command shape as cmdVolume —
// 1 arg = spec form, 2 = legacy trx-prefixed — and had the same defect.
// "tx_gain:" splits to a single empty string, reaches the SET branch, and an
// unchecked toInt() silently yields 0: the WSJT-X/JTDX TX audio path muted,
// broadcast as a well-formed "tx_gain:0;" a second client can't distinguish
// from a real change. This is the third instance of the class after #4345.
bool testTxGainMalformedInputIsDropped()
{
    TciProtocol protocol(nullptr);

    // Sanity first, so "the fix refuses everything" can't pass as success —
    // both the spec form and the legacy trx-prefixed one.
    protocol.handleCommand(QStringLiteral("tx_gain:60"));
    if (!check(protocol.pendingTxGain() == 60
                   && !protocol.pendingNotification().isEmpty(),
            "a well-formed tx_gain SET must still take effect")) {
        return false;
    }
    protocol.handleCommand(QStringLiteral("tx_gain:0,60"));
    if (!check(protocol.pendingTxGain() == 60,
            "the legacy trx-prefixed tx_gain SET must still take effect")) {
        return false;
    }

    // Clamping is unchanged: it applies to a value that parsed, not to one
    // that didn't. 0 remains a legitimate operator-requested mute.
    protocol.handleCommand(QStringLiteral("tx_gain:150"));
    if (!check(protocol.pendingTxGain() == 100,
            "an out-of-range tx_gain must still clamp rather than drop")) {
        return false;
    }
    protocol.handleCommand(QStringLiteral("tx_gain:0"));
    if (!check(protocol.pendingTxGain() == 0,
            "an explicit tx_gain:0 must still be honoured as a real mute")) {
        return false;
    }

    // The defect: malformed input must not land on 0 — which is exactly what
    // an explicit mute looks like on the wire, hence the assertion above.
    for (const auto& malformed : { QStringLiteral("tx_gain:"),
                                   QStringLiteral("tx_gain:0,"),
                                   QStringLiteral("tx_gain:loud") }) {
        protocol.handleCommand(malformed);
        if (!check(protocol.pendingTxGain() == -1
                       && protocol.pendingNotification().isEmpty(),
                qPrintable(QStringLiteral("\"%1\" must not be recorded as a "
                                          "tx_gain SET").arg(malformed)))) {
            return false;
        }
    }

    // Bare "tx_gain" is the GET form and must still read.
    const QString getResponse = protocol.handleCommand(QStringLiteral("tx_gain"));
    if (!check(getResponse.startsWith(QStringLiteral("tx_gain:")),
            "bare \"tx_gain\" GET must still reply with the current gain")) {
        return false;
    }
    return true;
}

// #4523: an unrecognised modulation name used to silently fall through to
// USB (tciToSmartSDR's QMap::value default) and get applied as if the client
// had asked for it. tciToSmartSDR now reports success via an out-parameter so
// a SET-path caller (cmdModulation) can reject the name instead of guessing.
bool testTciToSmartSdrReportsUnrecognisedNames()
{
    bool ok = false;

    const QString usb = TciProtocol::tciToSmartSDR(QStringLiteral("usb"), &ok);
    if (!check(ok && usb == QStringLiteral("USB"),
            "a recognised modulation name must report ok and its mapped mode")) {
        return false;
    }

    ok = true;  // start true so a no-op bug in the fix can't hide as a false negative
    const QString unknown = TciProtocol::tciToSmartSDR(QStringLiteral("ft8"), &ok);
    if (!check(!ok,
            "an unrecognised modulation name must report !ok rather than silently mapping to USB")) {
        return false;
    }
    // The fallback return value is still "USB" for source compatibility —
    // there is currently no caller that relies on it (tciToSmartSDR has one
    // call site, cmdModulation's SET path, and it checks ok).
    if (!check(unknown == QStringLiteral("USB"),
            "the fallback return value contract is unchanged for callers that don't check ok")) {
        return false;
    }

    // Case-insensitivity must not accidentally launder an unknown name into
    // a match.
    ok = true;
    TciProtocol::tciToSmartSDR(QStringLiteral("FT8"), &ok);
    if (!check(!ok, "rejection must be case-insensitive, not just lowercase-literal")) {
        return false;
    }
    return true;
}

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// The demo entry exactly as ConnectionPanel::addDemoRadio() builds it (see
// radiomodel_audio_mute_test.cpp).
static RadioInfo demoInfo()
{
    RadioInfo i;
    i.name    = QStringLiteral("FLEX-6700");
    i.model   = SimBackend::demoModelName();
    i.serial  = SimBackend::demoSerial();
    i.family  = SimBackend::familyName();
    i.address = QHostAddress(QHostAddress::LocalHost);   // synthetic; never dialed
    i.port    = 4992;
    return i;
}

// #4523/#4848: the branch this PR actually adds is cmdModulation's
// `if (!modOk) return {};` — testTciToSmartSdrReportsUnrecognisedNames above
// exercises the helper it calls, but a modelless TciProtocol(nullptr) can't
// reach cmdModulation's SET path at all: sliceForTrx()'s resolvers both
// require model->isConnected(), which the disconnected-only
// automationApplySliceFixture() fixture cannot satisfy. Driving this through
// the in-process demo backend gets a real connected model with a real slice,
// per the pattern in radiomodel_audio_mute_test.cpp / demo_backend_swap_test.cpp.
bool testModulationEndToEndRejectsUnknownName()
{
    // The demo backend is in-process, so both waits below settle in a few
    // milliseconds locally. They poll to a deadline rather than sleeping a
    // fixed span, so a loaded CI runner makes this test slower rather than
    // red; the bound is deliberately generous for that reason, and costs
    // nothing when it is not needed.
    constexpr int kSettleTries = 100;   // × 50 ms = 5 s ceiling
    RadioModel model;
    model.connectToRadio(demoInfo());
    for (int i = 0; i < kSettleTries && !model.isConnected(); ++i)
        spin(50);
    if (!check(model.isConnected(), "precondition: connected to the demo backend")) {
        return false;
    }
    SliceModel* slice = nullptr;
    for (int i = 0; i < kSettleTries && model.slices().isEmpty(); ++i)
        spin(50);
    slice = model.slices().value(0);
    if (!check(slice != nullptr, "precondition: the demo backend announced a slice")) {
        return false;
    }

    TciProtocol protocol(&model);

    // Sanity: a recognised name still takes effect end to end, so the fix
    // isn't refusing every SET. setMode() is invoked via a queued
    // QMetaObject::invokeMethod, so events must be processed before reading
    // the result back.
    slice->setMode(QStringLiteral("USB"));
    protocol.handleCommand(QStringLiteral("modulation:0,digu"));
    QCoreApplication::processEvents();
    if (!check(slice->mode() == QStringLiteral("DIGU")
                   && !protocol.pendingNotification().isEmpty(),
            "a recognised modulation name must still take effect end to end")) {
        return false;
    }

    // The issue's exact repro, driven end to end: an unrecognised name on a
    // real SET must not reach setMode() and must not notify.
    protocol.handleCommand(QStringLiteral("modulation:0,ft8"));
    QCoreApplication::processEvents();
    if (!check(slice->mode() == QStringLiteral("DIGU"),
            "an unrecognised modulation name must not change the slice's mode")) {
        return false;
    }
    if (!check(protocol.pendingNotification().isEmpty(),
            "an unrecognised modulation name must not notify other clients")) {
        return false;
    }
    return true;
}

// #4867: the sharpest form of the malformed-argument class. Every verb that
// takes a trx used to do `int trx = args[0].toInt();` unchecked, and
// sliceForTrx() resolves positionally — so a malformed trx did not fail to
// resolve, it resolved to slice 0 and the command landed on the WRONG SLICE,
// broadcasting a notification naming slice 0 as though the client had asked
// for it. This is the assertion none of the three previous fixes in this
// class (#4345, #4523's two) ever made, which is why the class survived them.
bool testMalformedTrxDoesNotLandOnSliceZero()
{
    constexpr int kSettleTries = 100;   // × 50 ms = 5 s ceiling; see above
    RadioModel model;
    model.connectToRadio(demoInfo());
    for (int i = 0; i < kSettleTries && !model.isConnected(); ++i)
        spin(50);
    if (!check(model.isConnected(), "precondition: connected to the demo backend")) {
        return false;
    }
    for (int i = 0; i < kSettleTries && model.slices().isEmpty(); ++i)
        spin(50);
    SliceModel* slice = model.slices().value(0);
    if (!check(slice != nullptr, "precondition: the demo backend announced a slice")) {
        return false;
    }

    TciProtocol protocol(&model);

    // Positive control first, so "the fix refuses everything" cannot pass as
    // success: a well-formed trx still reaches slice 0.
    slice->setAudioMute(false);
    protocol.handleCommand(QStringLiteral("mute:0,true"));
    QCoreApplication::processEvents();
    if (!check(slice->audioMute() && !protocol.pendingNotification().isEmpty(),
            "a well-formed trx must still apply to the addressed slice")) {
        return false;
    }

    // Each of these used to become trx=0 and mutate slice 0 while emitting a
    // well-formed "…:0,…;" notification — indistinguishable, on the wire,
    // from the client having addressed slice 0 deliberately.
    struct Case { const char* cmd; const char* what; };
    static const Case kCases[] = {
        { "mute:abc,false",            "mute with a non-numeric trx" },
        { "mute:,false",               "mute with an empty trx" },
        { "mute:0x1,false",            "mute with a hex trx" },
        { "rx_mute:zz,false",          "rx_mute with a non-numeric trx" },
    };
    for (const auto& c : kCases) {
        slice->setAudioMute(true);
        QCoreApplication::processEvents();
        protocol.handleCommand(QString::fromLatin1(c.cmd));
        QCoreApplication::processEvents();
        if (!check(slice->audioMute(),
                qPrintable(QStringLiteral("%1 must not unmute slice 0")
                               .arg(QString::fromLatin1(c.what))))) {
            return false;
        }
        if (!check(protocol.pendingNotification().isEmpty(),
                qPrintable(QStringLiteral("%1 must not notify other clients")
                               .arg(QString::fromLatin1(c.what))))) {
            return false;
        }
    }

    // A per-slice verb with NO trx at all must not answer for slice 0 either.
    // This is the argument-missing half of the helper's contract, as distinct
    // from the argument-present-but-unparseable half above: without it, a
    // helper that treated an out-of-range index as "0" would pass every other
    // assertion in this file. (Verified by mutation: it does.)
    if (!check(protocol.handleCommand(QStringLiteral("mute")).isEmpty(),
            "bare \"mute\" must not answer for slice 0")) {
        return false;
    }
    if (!check(protocol.handleCommand(QStringLiteral("rx_volume")).isEmpty(),
            "bare \"rx_volume\" must not answer for slice 0")) {
        return false;
    }

    // Same defect on a different verb and a different observable: a malformed
    // trx must not reach setMode() on slice 0 either.
    slice->setMode(QStringLiteral("USB"));
    QCoreApplication::processEvents();
    protocol.handleCommand(QStringLiteral("modulation:abc,lsb"));
    QCoreApplication::processEvents();
    if (!check(slice->mode() == QStringLiteral("USB"),
            "a malformed trx must not change slice 0's mode")) {
        return false;
    }

    // And on the filter pair, where the old behaviour was a zero-width filter:
    // the malformed VALUE arguments must be rejected too, not just the trx.
    // Both edges are checked, so rejecting only the first would still fail.
    for (const auto& cmd : { QStringLiteral("rx_filter_band:0,abc,2800"),
                             QStringLiteral("rx_filter_band:0,100,abc"),
                             QStringLiteral("rx_filter_band:0,,2800") }) {
        const int lowBefore = slice->filterLow();
        const int highBefore = slice->filterHigh();
        protocol.handleCommand(cmd);
        QCoreApplication::processEvents();
        if (!check(slice->filterLow() == lowBefore
                       && slice->filterHigh() == highBefore,
                qPrintable(QStringLiteral("\"%1\" must not collapse the passband")
                               .arg(cmd)))) {
            return false;
        }
        if (!check(protocol.pendingNotification().isEmpty(),
                qPrintable(QStringLiteral("\"%1\" must not notify other clients")
                               .arg(cmd)))) {
            return false;
        }
    }

    // The remaining per-slice value arguments, each of which used to land on
    // 0 — a legitimate value in every one of these ranges, which is exactly
    // what made the defect undetectable downstream.
    protocol.handleCommand(QStringLiteral("agc_gain:0,40"));
    QCoreApplication::processEvents();
    if (!check(!protocol.pendingNotification().isEmpty(),
            "a well-formed agc_gain SET must still take effect")) {
        return false;
    }
    for (const auto& cmd : { QStringLiteral("agc_gain:0,abc"),
                             QStringLiteral("agc_gain:0,"),
                             QStringLiteral("sql_level:0,abc"),
                             QStringLiteral("rx_balance:0,abc"),
                             QStringLiteral("rx_volume:0,abc"),
                             QStringLiteral("rx_nb_param:0,0,abc"),
                             QStringLiteral("rit_offset:0,abc"),
                             QStringLiteral("xit_offset:0,abc") }) {
        protocol.handleCommand(cmd);
        QCoreApplication::processEvents();
        if (!check(protocol.pendingNotification().isEmpty(),
                qPrintable(QStringLiteral("\"%1\" must not notify other clients")
                               .arg(cmd)))) {
            return false;
        }
    }
    return true;
}

// #4867: the value-argument half of the class, on the verbs that need no
// slice. Each of these used to turn unparseable input into 0 and broadcast
// it as a well-formed change.
bool testMalformedValueArgsAreDropped()
{
    TciProtocol protocol(nullptr);

    // The GLOBAL verbs re-derive GET/SET from the argument list instead of
    // trusting the dispatcher's `isSet = (args.size() >= 2)`, which is
    // computed from the trx-prefixed shape the per-slice verbs use. Both
    // forms must set: the spec form (one argument, the value) and the legacy
    // trx-prefixed one (two arguments, value last). Before this change the
    // first was a READ that discarded the value and the second read the trx
    // position as the value, so `cw_macros_delay:0,250;` set the delay to 0.
    protocol.handleCommand(QStringLiteral("cw_macros_delay:250"));
    if (!check(protocol.pendingNotification()
                   == QStringLiteral("cw_macros_delay:250;"),
            "the spec-form global SET must take effect, not be read as a GET")) {
        return false;
    }
    protocol.handleCommand(QStringLiteral("cw_macros_delay:0,180"));
    if (!check(protocol.pendingNotification()
                   == QStringLiteral("cw_macros_delay:180;"),
            "the legacy trx-prefixed global SET must read the VALUE, not the trx")) {
        return false;
    }
    // Bare — no colon at all — is the read, and must not disturb the value.
    if (!check(protocol.handleCommand(QStringLiteral("cw_macros_delay"))
                   == QStringLiteral("cw_macros_delay:180;"),
            "bare \"cw_macros_delay\" must read back the last value set")) {
        return false;
    }

    // cw_terminal is the BOOLEAN global verb, and modelless-reachable, so the
    // argToBool half of the contract gets its positive controls here: both
    // wire forms must set, and the value comes from the last argument.
    protocol.handleCommand(QStringLiteral("cw_terminal:true"));
    if (!check(protocol.handleCommand(QStringLiteral("cw_terminal"))
                   == QStringLiteral("cw_terminal:true;"),
            "the spec-form boolean global SET must take effect, not be read as a GET")) {
        return false;
    }
    protocol.handleCommand(QStringLiteral("cw_terminal:0,false"));
    if (!check(protocol.handleCommand(QStringLiteral("cw_terminal"))
                   == QStringLiteral("cw_terminal:false;"),
            "the legacy trx-prefixed boolean SET must read the VALUE, not the trx")) {
        return false;
    }
    // Case-insensitivity is preserved from the expression argToBool replaced.
    protocol.handleCommand(QStringLiteral("cw_terminal:TRUE"));
    if (!check(protocol.handleCommand(QStringLiteral("cw_terminal"))
                   == QStringLiteral("cw_terminal:true;"),
            "argToBool must stay case-insensitive, as the old comparison was")) {
        return false;
    }

    static const char* kMalformed[] = {
        "cw_macros_delay:",         // the reported shape: colon, nothing after
        "cw_macros_delay:abc",
        "cw_macros_delay:0,",       // legacy shape, empty value
        "cw_macros_delay:0,abc",
        // The boolean half of the same class (#4867 review): everything that
        // was not the word "true" used to become false and apply, so these
        // used to be indistinguishable from a deliberate `cw_terminal:false`.
        "cw_terminal:",
        "cw_terminal:yes",
        "cw_terminal:1",
        "cw_terminal:0,yes",
    };
    for (const char* cmd : kMalformed) {
        protocol.handleCommand(QString::fromLatin1(cmd));
        if (!check(protocol.pendingNotification().isEmpty(),
                qPrintable(QStringLiteral("\"%1\" must not notify other clients")
                               .arg(QString::fromLatin1(cmd))))) {
            return false;
        }
    }

    // cw_macros_delay keeps its value across commands (it is a static), so a
    // dropped malformed SET must leave the last good one in place rather than
    // silently resetting it to 0 — which is what the unchecked toInt() did.
    const QString readBack =
        protocol.handleCommand(QStringLiteral("cw_macros_delay"));
    if (!check(readBack == QStringLiteral("cw_macros_delay:180;"),
            "a dropped cw_macros_delay SET must not have clobbered the stored value")) {
        return false;
    }
    // Same property for the boolean helper: argToBool leaves `out` untouched
    // on failure, so the malformed cw_terminal SETs above must have left the
    // last good value (true) rather than flipping it to false.
    if (!check(protocol.handleCommand(QStringLiteral("cw_terminal"))
                   == QStringLiteral("cw_terminal:true;"),
            "a dropped boolean SET must not have clobbered the stored value")) {
        return false;
    }

    // iq_samplerate is the deliberate exception to the drop-on-malformed
    // posture: #3913 made it REPLY to a rejection because a blocked skimmer
    // would otherwise hang, so an unparseable rate must report the rate still
    // in force rather than returning nothing.
    const QString rateReply =
        protocol.handleCommand(QStringLiteral("iq_samplerate:abc"));
    if (!check(rateReply == QStringLiteral("iq_samplerate:48000;"),
            "an unparseable iq_samplerate must report the rate still in force")) {
        return false;
    }
    if (!check(protocol.pendingNotification().isEmpty(),
            "a rejected iq_samplerate must not notify other clients")) {
        return false;
    }
    return true;
}

// #4867 review: the rest of the global-verb class, plus the boolean helper.
// The first commit ended the numeric half; these are the sites that needed a
// real connected model to observe, and the two the second commit missed.
bool testGlobalVerbsAndBooleansEndToEnd()
{
    constexpr int kSettleTries = 100;   // × 50 ms = 5 s ceiling
    RadioModel model;
    model.connectToRadio(demoInfo());
    for (int i = 0; i < kSettleTries && !model.isConnected(); ++i)
        spin(50);
    if (!check(model.isConnected(), "precondition: connected to the demo backend")) {
        return false;
    }
    for (int i = 0; i < kSettleTries && model.slices().isEmpty(); ++i)
        spin(50);
    SliceModel* slice = model.slices().value(0);
    if (!check(slice != nullptr, "precondition: the demo backend announced a slice")) {
        return false;
    }

    TciProtocol protocol(&model);
    auto settle = [] { QCoreApplication::processEvents(); };

    // ── digl_offset / digu_offset: instances five and six ──────────────────
    // Both take no trx (they hardcode sliceForTrx(0)), so the dispatcher's
    // trx-shaped isSet was wrong in both directions: the spec form read as a
    // GET and discarded the value, the legacy form set the offset to the trx
    // slot and broadcast "digl_offset:0;".
    protocol.handleCommand(QStringLiteral("digl_offset:500"));
    settle();
    if (!check(slice->diglOffset() == 500,
            "the spec-form digl_offset SET must take effect, not be read as a GET")) {
        return false;
    }
    protocol.handleCommand(QStringLiteral("digu_offset:0,650"));
    settle();
    if (!check(slice->diguOffset() == 650,
            "the legacy trx-prefixed digu_offset SET must read the VALUE, not the trx")) {
        return false;
    }
    for (const auto& cmd : { QStringLiteral("digl_offset:abc"),
                             QStringLiteral("digl_offset:"),
                             QStringLiteral("digl_offset:0,abc") }) {
        protocol.handleCommand(cmd);
        settle();
        if (!check(slice->diglOffset() == 500,
                qPrintable(QStringLiteral("\"%1\" must not clobber the stored offset")
                               .arg(cmd)))) {
            return false;
        }
        if (!check(protocol.pendingNotification().isEmpty(),
                qPrintable(QStringLiteral("\"%1\" must not notify other clients")
                               .arg(cmd)))) {
            return false;
        }
    }
    // A bare read must still answer, and must report what was actually set.
    if (!check(protocol.handleCommand(QStringLiteral("digl_offset"))
                   == QStringLiteral("digl_offset:500;"),
            "bare \"digl_offset\" must read back the last value set")) {
        return false;
    }

    // ── mon_enable: the boolean half of the same defect ────────────────────
    model.transmitModel().setSbMonitor(false);
    settle();
    protocol.handleCommand(QStringLiteral("mon_enable:true"));
    settle();
    if (!check(model.transmitModel().sbMonitor(),
            "the spec-form mon_enable SET must take effect, not be read as a GET")) {
        return false;
    }
    protocol.handleCommand(QStringLiteral("mon_enable:0,false"));
    settle();
    if (!check(!model.transmitModel().sbMonitor(),
            "the legacy trx-prefixed mon_enable SET must read the VALUE, not the trx")) {
        return false;
    }

    // ── mon_volume: the verb whose ambiguous form the operator can hear ────
    protocol.handleCommand(QStringLiteral("mon_volume:37"));
    settle();
    if (!check(model.transmitModel().monGainSb() == 37,
            "the spec-form mon_volume SET must take effect, not be read as a GET")) {
        return false;
    }
    // Per the #4867 review ruling: mon_volume:0 IS a SET of 0 per spec and is
    // deliberately NOT special-cased — silently refusing a legitimate 0 would
    // be the same undetectable-from-the-wire failure in the other direction.
    protocol.handleCommand(QStringLiteral("mon_volume:0"));
    settle();
    if (!check(model.transmitModel().monGainSb() == 0,
            "mon_volume:0 must be honoured as a SET of 0, not refused as ambiguous")) {
        return false;
    }
    protocol.handleCommand(QStringLiteral("mon_volume:0,44"));
    settle();
    if (!check(model.transmitModel().monGainSb() == 44,
            "the legacy trx-prefixed mon_volume SET must read the VALUE, not the trx")) {
        return false;
    }
    protocol.handleCommand(QStringLiteral("mon_volume:abc"));
    settle();
    if (!check(protocol.pendingNotification().isEmpty(),
            "an unparseable mon_volume must not notify other clients")) {
        return false;
    }
    if (!check(model.transmitModel().monGainSb() == 44,
            "a dropped mon_volume SET must not have clobbered the stored value")) {
        return false;
    }

    // ── the two CW speed globals, which need a model since #4867's review ──
    protocol.handleCommand(QStringLiteral("cw_keyer_speed:25"));
    if (!check(protocol.pendingNotification()
                   == QStringLiteral("cw_keyer_speed:25;"),
            "the spec-form cw_keyer_speed SET must take effect, not be read as a GET")) {
        return false;
    }
    protocol.handleCommand(QStringLiteral("cw_macros_speed:0,30"));
    if (!check(protocol.pendingNotification()
                   == QStringLiteral("cw_macros_speed:30;"),
            "the legacy trx-prefixed cw_macros_speed SET must read the VALUE, not the trx")) {
        return false;
    }
    for (const auto& cmd : { QStringLiteral("cw_keyer_speed:abc"),
                             QStringLiteral("cw_keyer_speed:0,abc"),
                             QStringLiteral("cw_macros_speed:abc"),
                             QStringLiteral("cw_macros_speed:0,abc") }) {
        protocol.handleCommand(cmd);
        if (!check(protocol.pendingNotification().isEmpty(),
                qPrintable(QStringLiteral("\"%1\" must not notify other clients")
                               .arg(cmd)))) {
            return false;
        }
    }

    // ── argToBool on the per-slice verbs ───────────────────────────────────
    // Positive control first: "true"/"false" still work, in either case.
    slice->setAudioMute(false);
    settle();
    protocol.handleCommand(QStringLiteral("mute:0,TRUE"));
    settle();
    if (!check(slice->audioMute(),
            "a well-formed boolean must still apply, case-insensitively")) {
        return false;
    }
    // Then the defect: anything that is not the word "true" used to become
    // false and UNMUTE slice 0, broadcasting a well-formed "mute:0,false;"
    // that no second client could tell from a deliberate unmute.
    for (const auto& cmd : { QStringLiteral("mute:0,yes"),
                             QStringLiteral("mute:0,1"),
                             QStringLiteral("mute:0,"),
                             QStringLiteral("rx_mute:0,off") }) {
        protocol.handleCommand(cmd);
        settle();
        if (!check(slice->audioMute(),
                qPrintable(QStringLiteral("\"%1\" must not unmute slice 0")
                               .arg(cmd)))) {
            return false;
        }
        if (!check(protocol.pendingNotification().isEmpty(),
                qPrintable(QStringLiteral("\"%1\" must not notify other clients")
                               .arg(cmd)))) {
            return false;
        }
    }
    // The same on a non-mute boolean, so the assertion is not mute-specific.
    protocol.handleCommand(QStringLiteral("rx_nb_enable:0,true"));
    settle();
    protocol.handleCommand(QStringLiteral("rx_nb_enable:0,nope"));
    settle();
    if (!check(slice->nbOn(),
            "an unparseable rx_nb_enable must not turn the blanker off")) {
        return false;
    }

    // ── the TX-keying verbs are deliberately NOT argToBool ─────────────────
    // cmdTune and cmdKeyer must fail CLOSED, not silent: "not the word true"
    // has to keep meaning stop/up, because dropping an unparseable stop would
    // leave a keyed transmitter keyed (Constitution VI).
    //
    // Only the safe direction is asserted, and it is a smoke check rather
    // than a mutation-proof one: proving the fail-closed property properly
    // would mean starting a tune and then stopping it with malformed input,
    // and a test must not drive the keyed direction. The property is held by
    // the comment at the call site and by rule 7 of tci-receivers.md.
    if (!check(!model.transmitModel().isTuning(),
            "precondition: the demo backend is not tuning")) {
        return false;
    }
    protocol.handleCommand(QStringLiteral("tune:0,garbage"));
    settle();
    if (!check(!model.transmitModel().isTuning(),
            "a malformed tune state must never START a tune")) {
        return false;
    }

    // ── iq_start/iq_stop bound the index before the addition ───────────────
    // `trx + 1` on an INT_MAX index was signed overflow (argToInt accepts
    // INT_MAX — it is "99999999999" that clears ok). Bounding trx first makes
    // the arithmetic defined, but it changes NO observable behaviour:
    // DaxIqModel::createStream() already rejects an out-of-range channel, so
    // the mutation that removes the bound is caught by UBSan, not by a test.
    // Asserting a rejection here would be a test that pins nothing, so the
    // only assertion is the one with something to observe — that the bound
    // did not over-tighten and an in-range index still creates its stream.
    QStringList daxCommands;
    QObject::connect(&model.daxIqModel(), &DaxIqModel::commandReady,
                     [&daxCommands](const QString& c) { daxCommands << c; });
    protocol.handleCommand(QStringLiteral("iq_start:0"));
    settle();
    if (!check(!daxCommands.isEmpty(),
            "a well-formed iq_start must still create the DAX IQ stream")) {
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    // Must be constructed before QCoreApplication (see TestSettingsProfile.h)
    // — only testModulationEndToEndRejectsUnknownName needs it (it constructs
    // a real RadioModel, which touches AppSettings), but every other test in
    // this file runs against a modelless TciProtocol(nullptr) and is
    // unaffected by an isolated settings profile being active.
    TestSettingsProfile settingsProfile(QStringLiteral("aether-tci-protocol-test"));
    QCoreApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();

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

    if (!testRoutingPolicy() || !testStaleRouteFailsSafe()
        || !testBarePttKeysTheRequestedSlice()
        || !testWsjtxRoutingContracts()
        || !testDeferredCommands() || !testDriveWireContract()
        || !testVolumeMalformedInputIsDropped()
        || !testTxGainMalformedInputIsDropped()
        || !testTciToSmartSdrReportsUnrecognisedNames()
        || !testModulationEndToEndRejectsUnknownName()
        || !testMalformedValueArgsAreDropped()
        || !testMalformedTrxDoesNotLandOnSliceZero()
        || !testGlobalVerbsAndBooleansEndToEnd()) {
        return 1;
    }

    // ── active_slice (#4160) — AetherSDR extension, read-only ──────────
    // Focus is unknown until TciServer observes an activeChanged(true), and
    // there is no model here to scan: report nothing rather than guess trx 0,
    // which is exactly the wrong answer the issue is about.
    if (!protocol.handleCommand(QStringLiteral("active_slice")).isEmpty()) {
        std::fprintf(stderr,
                     "active_slice GET must stay silent while focus is unknown\n");
        return 1;
    }

    // trx is positional and the operator never sees it; the display letter is
    // what the GUI shows, so both are reported.
    protocol.setActiveSlice(1, QStringLiteral("C"));
    const QString activeGet =
        protocol.handleCommand(QStringLiteral("active_slice"));
    if (activeGet != QStringLiteral("active_slice:1,C;")) {
        std::fprintf(stderr,
                     "active_slice GET should report trx and letter; got %s\n",
                     activeGet.toUtf8().constData());
        return 1;
    }

    // A radio-supplied letter carrying TCI delimiters would corrupt framing
    // for every client on the socket. The RAW string is passed in on purpose:
    // sanitizing is the setter's job, so the invariant does not depend on each
    // caller remembering to pre-clean.
    protocol.setActiveSlice(1, QStringLiteral("A;drive:0,100"));
    const QString sanitized =
        protocol.handleCommand(QStringLiteral("active_slice"));
    if (sanitized.count(QLatin1Char(';')) != 1
        || sanitized.count(QLatin1Char(',')) != 1) {
        std::fprintf(stderr,
                     "active_slice letter must not inject delimiters; got %s\n",
                     sanitized.toUtf8().constData());
        return 1;
    }
    protocol.setActiveSlice(1, QStringLiteral("C"));

    // 0-1 args = GET, matching the split handleCommand() documents for every
    // other command. active_slice has no per-TRX form, but `active_slice:0;` is
    // the shape a client written against `rx_volume:0;` will send, so it is
    // answered rather than silently dropped.
    if (protocol.handleCommand(QStringLiteral("active_slice:0"))
        != QStringLiteral("active_slice:1,C;")) {
        std::fprintf(stderr,
                     "active_slice GET with a redundant trx arg must still report\n");
        return 1;
    }

    // SET (2+ args) is ignored — focus is GUI-owned. A client must not be able
    // to steal it, and must not desync every other client by appearing to.
    if (!protocol.handleCommand(QStringLiteral("active_slice:0,1")).isEmpty()) {
        std::fprintf(stderr, "active_slice SET must not reply\n");
        return 1;
    }
    if (!protocol.pendingNotification().isEmpty()) {
        std::fprintf(stderr, "active_slice SET must not notify other clients\n");
        return 1;
    }
    if (protocol.handleCommand(QStringLiteral("active_slice"))
        != QStringLiteral("active_slice:1,C;")) {
        std::fprintf(stderr, "active_slice SET must not change reported focus\n");
        return 1;
    }

    // Focus can become unknown again: removing the focused slice renumbers
    // trx and leaves nothing focused until the radio picks a new slice, so
    // TciServer pushes -1. Report nothing rather than a stale trx.
    protocol.setActiveSlice(-1, QString());
    if (!protocol.handleCommand(QStringLiteral("active_slice")).isEmpty()) {
        std::fprintf(stderr,
                     "active_slice GET must stay silent once focus is cleared\n");
        return 1;
    }
    // Removing an earlier slice renumbers trx while the letter stays put —
    // exactly why the letter is reported alongside it.
    protocol.setActiveSlice(0, QStringLiteral("C"));
    if (protocol.handleCommand(QStringLiteral("active_slice"))
        != QStringLiteral("active_slice:0,C;")) {
        std::fprintf(stderr, "active_slice GET must track renumbered focus\n");
        return 1;
    }

    // The greeting carries focus only alongside the rest of the per-slice
    // state dump, so a model-less protocol must not synthesize one.
    for (const QString& command : greeting) {
        if (command.startsWith(QStringLiteral("active_slice"))) {
            std::fprintf(stderr,
                         "modelless TCI greeting must not emit active_slice\n");
            return 1;
        }
    }

    std::printf("tci_protocol_test: all checks passed\n");
    return 0;
}
