#include "core/backends/flex/FlexPttWireSession.h"

#include <QCoreApplication>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}
constexpr QStringView kIdle = u"S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1 amplifier=";
constexpr QStringView kRequested = u"S0|interlock tx_client_handle=0x12345678 state=PTT_REQUESTED reason= source=SW tx_allowed=1 amplifier=";
constexpr QStringView kTransmitting = u"S0|interlock tx_client_handle=0x12345678 state=TRANSMITTING reason= source=SW tx_allowed=1 amplifier=";
constexpr QStringView kUnkey = u"S0|interlock tx_client_handle=0x12345678 state=UNKEY_REQUESTED reason= source= tx_allowed=1 amplifier=";
constexpr QStringView kReadyOwned = u"S0|interlock tx_client_handle=0x12345678 state=READY reason= source= tx_allowed=1 amplifier=";

struct Harness {
    qint64 now{100};
    QStringList commands;
    bool fullWrite{true};
    TxStopEvidence evidence;
    TxCoordinator::StopRequest stop;
    TxCoordinator coordinator{[this](const auto& operation, auto) {
        stop = coordinator.requestStopConfirmation(operation);
    }, [this] { return now; }};
    TxCoordinator::Actor a{coordinator.registerActor({true, 10000, 100000, 1, true})};
    TxCoordinator::Actor b{coordinator.registerActor({true, 10000, 100000, 1, true})};
    TxCoordinator::Operation operation{coordinator.acquire(a, now).operation};
    FlexPttWireSession wire{[this](quint32, const QString& text) {
        commands.append(text);
        return fullWrite;
    }, [this](const TxStopEvidence& proof) { evidence = proof; }};
    Harness() { wire.reset(1, 0x12345678); feed(kIdle); }
    void feed(QStringView line) { wire.observe(line, now); }
    void key() { wire.key(101, {operation, true}, now); }
    void cancel()
    {
        check(coordinator.cancel(a, operation), "fence operation before queued stop");
    }
    void stopWire() { wire.stop(102, operation, stop, now); }
    void keyReadback() { feed(u"R101|0|"); feed(kRequested); feed(kTransmitting); }
    void stopReadback() { feed(u"R102|0|"); feed(kUnkey); feed(kReadyOwned); feed(kIdle); }
};

void dspDeathHelperSnapshotRecovery()
{
    constexpr QStringView snapshot = u"S87654321|interlock tx_client_handle=0x12345678 state=READY reason= source= tx_allowed=1 amplifier=";
    for (int stage = 0; stage < 7; ++stage) {
        Harness h;
        h.key(); h.keyReadback(); h.feed(kUnkey); h.feed(kReadyOwned);
        if (stage >= 3) { h.feed(kIdle); }
        if (stage >= 1) { h.cancel(); h.stopWire(); }
        if (stage == 2 || stage >= 5) { h.feed(u"R102|0|"); }
        if (stage == 6) { h.feed(kIdle); }
        // FLEX-8600 4.3.16 receive-guardian subscriptions broadcast the
        // original owner's READY while its stop exchange is still pending.
        h.feed(snapshot);
        if (stage < 6) {
            check(!h.evidence.valid(h.now), "helper-owned READY cannot supply release evidence");
        }
        if (stage == 0) { h.cancel(); h.stopWire(); }
        if (stage != 2 && stage < 5) { h.feed(u"R102|0|"); }
        if (stage != 6) {
            check(!h.evidence.valid(h.now), "stop ACK still requires original owner-clear");
            h.feed(kIdle);
        }
        check(h.evidence.valid(h.now), "original ACK and owner-clear survive receive-guardian snapshot");
        check(h.coordinator.confirmStopped(h.evidence.request), "snapshot preserves the exact original stop certificate");
        check(h.commands == QStringList{"xmit 1", "xmit 0"}, "snapshot recovery adds no keying commands");
    }
    Harness normal;
    normal.key(); normal.keyReadback(); normal.cancel(); normal.stopWire();
    normal.feed(u"R102|0|"); normal.feed(kUnkey); normal.feed(kReadyOwned);
    normal.feed(snapshot);
    check(!normal.evidence.valid(normal.now), "normal release snapshot cannot substitute for owner-clear");
    normal.feed(kIdle);
    check(normal.evidence.valid(normal.now), "normal release also tolerates an already-observed READY snapshot");

    Harness missingReady;
    missingReady.key(); missingReady.keyReadback(); missingReady.feed(kUnkey);
    missingReady.feed(snapshot); missingReady.cancel(); missingReady.stopWire();
    missingReady.feed(u"R102|0|"); missingReady.feed(kIdle);
    check(!missingReady.evidence.valid(missingReady.now), "helper snapshot cannot replace missing original owned READY");
    Harness missingReply;
    missingReply.key(); missingReply.keyReadback(); missingReply.feed(kUnkey); missingReply.feed(kReadyOwned);
    missingReply.cancel(); missingReply.stopWire(); missingReply.feed(snapshot); missingReply.feed(kIdle);
    check(!missingReply.evidence.valid(missingReply.now), "helper snapshot cannot replace missing stop ACK");

    for (QStringView contradiction : {
             u"S87654321|interlock tx_client_handle=0x87654321 state=READY reason= source= tx_allowed=1",
             u"S87654321|interlock tx_client_handle=0x12345678 state=TRANSMITTING reason= source=SW tx_allowed=1",
             u"S87654321|interlock tx_client_handle=0x12345678 state=READY reason= source=MIC tx_allowed=1",
             u"S87654321|interlock tx_client_handle=0x12345678 state=READY reason=FAULT source= tx_allowed=1",
             u"S87654321|interlock tx_client_handle=0x12345678 state=READY reason= source= tx_allowed=0",
             u"S87654321|interlock tx_client_handle=0x12345678 state=READY reason= source="}) {
        Harness h;
        h.key(); h.keyReadback(); h.feed(kUnkey); h.feed(kReadyOwned);
        h.cancel(); h.stopWire(); h.feed(contradiction); h.feed(u"R102|0|"); h.feed(kIdle);
        check(!h.evidence.valid(h.now), "contradictory or incomplete helper snapshots still fail closed");
    }
}

void noDispatch()
{
    Harness h;
    h.cancel();
    h.key(); // Original key queued before cancel, delivered after it.
    h.stopWire();
    h.wire.poll(++h.now);
    check(h.commands.isEmpty() && h.evidence.valid(h.now), "canceled queued key has exact no-dispatch proof and sends no unkey to another client");
    check(h.coordinator.confirmStopped(h.evidence.request), "local no-dispatch proof releases exact operation");
    check(h.coordinator.acquire(h.b, h.now).accepted(), "fresh other actor can acquire after no-dispatch completion");
    Harness acquiredOnly;
    acquiredOnly.cancel();
    acquiredOnly.stopWire();
    check(acquiredOnly.commands.isEmpty() && acquiredOnly.evidence.valid(acquiredOnly.now), "release without any key request is locally provable");
}

void acknowledgedStopRemainsReady()
{
    Harness h;
    h.key(); h.keyReadback(); h.cancel(); h.stopWire(); h.stopReadback();
    check(h.coordinator.confirmStopped(h.evidence.request), "consume original stop proof");
    h.now += FlexPttStopTracker::kTransitionTimeoutMs + 1;
    h.wire.poll(h.now);
    check(h.wire.ready() && !h.wire.needsPolling(),
          "consumed stop remains ready beyond old deadline without acquiring another grant");
    check(h.commands == QStringList{"xmit 1", "xmit 0"},
          "readiness recovery sends no keying command");
    h.feed(u"S0|interlock tx_allowed=0");
    check(!h.wire.ready(), "new partial status still withdraws readiness after consumed stop");
    h.feed(kIdle);
    check(h.wire.ready(), "complete fresh idle restores pre-admission readiness");
}

void protocolEligibility()
{
    for (QStringView version : {u"1.4.0.0", u"1.4.9.12345", u"1.4.2147483647.0"}) {
        check(FlexPttWireSession::supportsProtocol(version),
              "API 1.4 accepts developer-version changes without a firmware/model allowlist");
    }
    for (QStringView version : {u"", u"1.4", u"1.4.0", u"1.4.0.0.1", u"1.4.0.0beta",
             u"1.4.0.-1", u"1.4.0.+1", u"1.4.0.01", u"1.4.0.2147483648",
             u"1.4.0.999999999999999999999999999999999999", u" 1.4.0.0", u"1.4.0.0 ",
             u"1.3.0.0", u"1.5.0.0", u"2.4.0.0", u"4.2.18.41174"}) {
        check(!FlexPttWireSession::supportsProtocol(version),
              "unknown/malformed API and firmware-as-protocol remain unsupported");
    }
}

void protocolLossRetainsStop()
{
    Harness h;
    h.key();
    h.keyReadback();
    h.wire.rejectProtocol();
    check(!h.wire.ready(), "changed protocol immediately removes readiness");
    h.cancel();
    h.stopWire();
    h.stopReadback();
    check(h.commands == QStringList{"xmit 1", "xmit 0"}
          && !h.evidence.valid(h.now) && h.coordinator.recovering(),
          "changed protocol preserves authorized unkey but cannot prove handoff");

    Harness queued;
    queued.key(); queued.keyReadback(); queued.cancel(); queued.stopWire(); queued.stopReadback();
    const TxStopEvidence proof = queued.evidence;
    check(proof.valid(queued.now), "complete stop produces evidence before protocol contradiction");
    queued.wire.rejectProtocol();
    check(!proof.valid(queued.now), "protocol contradiction invalidates already queued stop evidence");
}

void rapidReleaseAndHandoff()
{
    Harness h;
    h.key();
    h.cancel();
    h.stopWire();
    check(h.commands == QStringList{"xmit 1", "xmit 0"}, "rapid release writes unkey before waiting for key-on readback");
    h.keyReadback();
    h.feed(u"R102|0|");
    h.feed(kUnkey);
    h.feed(kReadyOwned);
    check(!h.evidence.valid(h.now), "owned READY still cannot release another client");
    h.feed(kIdle);
    check(h.evidence.valid(h.now) && h.coordinator.confirmStopped(h.evidence.request), "complete ordered rapid-release sequence confirms exact stop");
    const auto next = h.coordinator.acquire(h.b, h.now);
    h.wire.key(103, {next.operation, true}, h.now);
    check(next.accepted() && h.commands.size() == 3, "next actor's fresh key-on reaches writer after confirmed handoff");
    check(!h.evidence.valid(h.now), "old queued certificate cannot survive the next operation");
}

void helperSubscriptionsDuringRelease()
{
    // FLEX-8600 4.3.16 stop trace: helper subscriptions interleave configuration
    // and idle snapshots with the original connection's transition sequence.
    constexpr QStringView timing = u"SBBA5FE5|interlock acc_txreq_enable=0 rca_txreq_enable=0 acc_tx_enabled=0 tx1_enabled=0 tx2_enabled=0 tx3_enabled=0 tx_delay=0 acc_tx_delay=0 tx1_delay=0 tx2_delay=0 tx3_delay=0 acc_txreq_polarity=0 rca_txreq_polarity=0 timeout=20000 tune_tx_suppress=0";
    constexpr QStringView helperIdle = u"S5C51784B|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1 amplifier= ";
    Harness h;
    h.key(); h.keyReadback(); h.cancel(); h.stopWire();
    h.feed(timing);
    h.feed(u"R102|0|");
    h.feed(kUnkey);
    h.feed(kReadyOwned);
    h.feed(helperIdle);
    check(!h.evidence.valid(h.now) && h.coordinator.recovering(),
          "helper-triggered idle does not certify the original stop");
    h.now += 431;
    h.feed(kIdle);
    check(h.evidence.valid(h.now), "original owner-clear completes stop despite helper subscription broadcasts");
    h.feed(timing);
    h.feed(helperIdle);
    check(h.evidence.valid(h.now) && h.coordinator.confirmStopped(h.evidence.request),
          "unrelated broadcasts cannot revoke complete original stop evidence");
    check(h.commands == QStringList{"xmit 1", "xmit 0"} && h.operation.hasConfirmedStop(),
          "release completes without extra keying or fabricated confirmation");

    Harness missing;
    missing.key(); missing.keyReadback(); missing.cancel(); missing.stopWire();
    missing.feed(u"R102|0|"); missing.feed(kUnkey); missing.feed(kReadyOwned);
    missing.feed(helperIdle);
    missing.now += FlexPttStopTracker::kTransitionTimeoutMs;
    missing.wire.poll(missing.now);
    check(!missing.evidence.valid(missing.now) && missing.coordinator.recovering(),
          "helper idle never replaces missing owner-clear or extends the deadline");
}

void independentReleaseBeforeDesktopStop()
{
    Harness h;
    h.key(); h.keyReadback();
    h.now += 12000; // All desktop threads resume after the bounded native cutoff.
    h.feed(kUnkey); h.feed(kReadyOwned); h.feed(kIdle);
    check(!h.evidence.valid(h.now) && !h.wire.ready(),
          "independent release alone cannot certify handoff or admit another key");
    h.cancel(); h.stopWire();
    check(h.commands == QStringList{"xmit 1", "xmit 0"},
          "recovery sends only the original operation's authorized stop");
    h.feed(u"R102|0|");
    check(!h.evidence.valid(h.now), "stop acknowledgment still needs fresh complete idle");
    h.now += 237;
    h.feed(kIdle);
    check(h.evidence.valid(h.now) && h.evidence.request.sameRequest(h.stop),
          "captured stall sequence plus acknowledged stop and fresh idle certifies original operation");
    check(h.coordinator.confirmStopped(h.evidence.request),
          "original entered-writer barrier accepts independently released operation");
    h.wire.poll(++h.now);
    check(h.wire.ready() && h.operation.hasConfirmedStop()
          && h.commands == QStringList{"xmit 1", "xmit 0"},
          "recovery retires old operation without automatic keying");
}

void independentReleaseOverlapsStop()
{
    const QList<QStringView> transition{kUnkey, kReadyOwned, kIdle};
    for (int stopAfter = 0; stopAfter <= 2; ++stopAfter) {
        for (int replyAfter = stopAfter; replyAfter <= 3; ++replyAfter) {
            Harness h;
            h.key(); h.keyReadback();
            if (stopAfter == 0) { h.cancel(); h.stopWire(); }
            if (replyAfter == 0) { h.feed(u"R102|0|"); }
            for (int i = 0; i < 3; ++i) {
                h.feed(transition[i]);
                if (i + 1 == stopAfter) { h.cancel(); h.stopWire(); }
                if (i + 1 == replyAfter) { h.feed(u"R102|0|"); }
                if (i < 2 || replyAfter == 3) {
                    check(!h.evidence.valid(h.now),
                          "overlapping stop requires both complete release and idle after its ACK");
                }
            }
            if (replyAfter == 3) { h.feed(kIdle); }
            check(h.evidence.valid(h.now) && h.evidence.request.sameRequest(h.stop),
                  "stop request and ACK may interleave with original independent release");
            check(h.coordinator.confirmStopped(h.evidence.request)
                  && h.commands == QStringList{"xmit 1", "xmit 0"},
                  "overlapping release confirms original operation with no new positive command");
        }
    }
}

void independentReleaseRefusals()
{
    const QList<QStringView> transition{kUnkey, kReadyOwned, kIdle};
    for (int omitted = 0; omitted < 3; ++omitted) {
        Harness h;
        h.key(); h.keyReadback();
        for (int i = 0; i < 3; ++i) {
            if (i != omitted) { h.feed(transition[i]); }
        }
        h.cancel(); h.stopWire(); h.feed(u"R102|0|");
        if (omitted != 2) { h.feed(kIdle); }
        check(!h.evidence.valid(h.now) && h.coordinator.recovering(),
              "recovery still requires every original release transition and owner-clear");
    }
    for (QStringView reply : {u"R999|0|", u"R102|1|", u"R102|garbage|"}) {
        Harness h;
        h.key(); h.keyReadback(); h.feed(kUnkey); h.feed(kReadyOwned); h.feed(kIdle);
        h.cancel(); h.stopWire(); h.feed(reply); h.feed(kIdle);
        check(!h.evidence.valid(h.now) && h.coordinator.recovering(),
              "missing, failed or malformed recovery acknowledgment cannot release operation");
    }
    for (int stopAfter = 1; stopAfter <= 2; ++stopAfter) {
        Harness missingReply;
        missingReply.key(); missingReply.keyReadback(); missingReply.feed(kUnkey);
        if (stopAfter == 2) { missingReply.feed(kReadyOwned); }
        missingReply.cancel(); missingReply.stopWire();
        if (stopAfter == 1) { missingReply.feed(kReadyOwned); }
        missingReply.feed(kIdle); missingReply.feed(kIdle);
        check(!missingReply.evidence.valid(missingReply.now) && missingReply.coordinator.recovering(),
              "overlapping independent release never replaces missing stop ACK");

        Harness deadline;
        deadline.key(); deadline.keyReadback(); deadline.feed(kUnkey);
        if (stopAfter == 2) { deadline.feed(kReadyOwned); }
        deadline.now += 4000;
        deadline.cancel(); deadline.stopWire(); deadline.feed(u"R102|0|");
        if (stopAfter == 1) { deadline.feed(kReadyOwned); }
        deadline.now += 1000;
        deadline.feed(kIdle);
        check(!deadline.evidence.valid(deadline.now),
              "stop during independent release cannot extend its first-unkey deadline");
    }
    Harness replacement;
    replacement.key(); replacement.keyReadback(); replacement.feed(kUnkey);
    replacement.cancel(); replacement.stopWire(); replacement.feed(u"R102|0|");
    const TxCoordinator::StopRequest newStop = replacement.coordinator.requestStopConfirmation(replacement.operation);
    replacement.wire.stop(103, replacement.operation, newStop, replacement.now);
    replacement.feed(kReadyOwned); replacement.feed(kIdle);
    check(!replacement.evidence.valid(replacement.now) && replacement.coordinator.recovering(),
          "replacement stop token cannot inherit the original acknowledgment during partial release");

    Harness earlierStopDeadline;
    earlierStopDeadline.key(); earlierStopDeadline.keyReadback();
    earlierStopDeadline.cancel(); earlierStopDeadline.stopWire();
    earlierStopDeadline.now += 4000;
    earlierStopDeadline.feed(kUnkey); earlierStopDeadline.feed(u"R102|0|"); earlierStopDeadline.feed(kReadyOwned);
    earlierStopDeadline.now += 1000;
    earlierStopDeadline.feed(kIdle);
    check(!earlierStopDeadline.evidence.valid(earlierStopDeadline.now),
          "independent unkey cannot renew an already pending stop deadline");

    Harness earlyIdle;
    earlyIdle.key(); earlyIdle.keyReadback();
    earlyIdle.feed(kUnkey); earlyIdle.feed(kReadyOwned); earlyIdle.feed(kIdle);
    earlyIdle.cancel(); earlyIdle.stopWire(); earlyIdle.feed(kIdle); earlyIdle.feed(u"R102|0|");
    check(!earlyIdle.evidence.valid(earlyIdle.now), "pre-ACK idle never counts as fresh recovery evidence");
    earlyIdle.feed(u"S87654321|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1");
    check(!earlyIdle.evidence.valid(earlyIdle.now), "helper-triggered idle cannot finish recovery");
    earlyIdle.feed(kIdle);
    check(earlyIdle.evidence.valid(earlyIdle.now), "later original-connection idle completes qualified recovery");

    for (int stage = 0; stage < 6; ++stage) {
        Harness h;
        h.key(); h.keyReadback(); h.feed(kUnkey);
        if (stage >= 1) { h.feed(kReadyOwned); }
        if (stage >= 2) { h.feed(kIdle); }
        if (stage >= 3) { h.cancel(); }
        if (stage >= 4) { h.stopWire(); }
        if (stage >= 5) { h.feed(u"R102|0|"); }
        h.feed(kTransmitting);
        if (stage < 3) { h.cancel(); }
        if (stage < 4) { h.stopWire(); }
        if (stage < 5) { h.feed(u"R102|0|"); }
        h.feed(kIdle);
        check(!h.evidence.valid(h.now) && h.coordinator.recovering(),
              "another TX interval at any recovery stage cannot reuse the old release");
    }
    for (QStringView contradiction : {
             u"S0|interlock state=READY",
             u"S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=0",
             u"S0|interlock tx_client_handle=0x87654321 state=TRANSMITTING reason= source=SW tx_allowed=1",
             u"S0|interlock tx_client_handle=0x12345678 state=TRANSMITTING reason= source=MIC tx_allowed=1"}) {
        Harness h;
        h.key(); h.keyReadback(); h.feed(kUnkey); h.feed(kReadyOwned); h.feed(kIdle);
        h.cancel(); h.stopWire(); h.feed(u"R102|0|"); h.feed(contradiction); h.feed(kIdle);
        check(!h.evidence.valid(h.now), "partial, inhibited, foreign or hardware TX invalidates recovery");
    }
    Harness partial;
    partial.key(); partial.keyReadback(); partial.feed(kUnkey); partial.feed(kReadyOwned); partial.feed(kIdle);
    partial.cancel(); partial.fullWrite = false; partial.stopWire(); partial.feed(u"R102|0|"); partial.feed(kIdle);
    check(!partial.evidence.valid(partial.now), "partial recovery stop write cannot produce a certificate");

    Harness deadline;
    deadline.key(); deadline.keyReadback(); deadline.feed(kUnkey);
    deadline.now += 4000;
    deadline.feed(kReadyOwned); deadline.feed(kIdle); deadline.cancel(); deadline.stopWire();
    deadline.feed(u"R102|0|");
    deadline.now += 1000;
    deadline.feed(kIdle);
    check(!deadline.evidence.valid(deadline.now), "recovery request and ACK do not renew first-unkey deadline");

    Harness disconnected;
    disconnected.key(); disconnected.keyReadback();
    disconnected.feed(kUnkey); disconnected.feed(kReadyOwned); disconnected.feed(kIdle);
    disconnected.cancel(); disconnected.stopWire(); disconnected.feed(u"R102|0|"); disconnected.feed(kIdle);
    const TxStopEvidence proof = disconnected.evidence;
    check(proof.valid(disconnected.now), "recovery produces a revocable certificate");
    disconnected.wire.disconnect();
    check(!proof.valid(disconnected.now), "disconnect revokes an unconsumed recovery certificate");
}

void partialAndStale()
{
    Harness partial;
    partial.fullWrite = false;
    partial.key();
    partial.cancel();
    partial.fullWrite = true;
    partial.stopWire();
    partial.keyReadback();
    partial.stopReadback();
    check(partial.commands.size() == 2 && !partial.evidence.valid(partial.now) && partial.coordinator.recovering(),
          "partial key write is never no-dispatch proof; cleanup still writes unkey");
    Harness stale;
    stale.key(); stale.keyReadback(); stale.cancel(); stale.stopWire(); stale.stopReadback();
    const TxStopEvidence queued = stale.evidence;
    stale.feed(u"S0|interlock tx_client_handle=0x87654321 state=TRANSMITTING reason= source=SW tx_allowed=1");
    check(!queued.valid(stale.now), "contradiction invalidates a certificate before its queued receiver runs");
    Harness disconnected;
    disconnected.key(); disconnected.keyReadback(); disconnected.cancel(); disconnected.stopWire(); disconnected.stopReadback();
    const TxStopEvidence old = disconnected.evidence;
    disconnected.wire.reset(2, 0x12345678);
    check(!old.valid(disconnected.now), "reconnect invalidates queued old-session stop proof");
    Harness delayed;
    delayed.key(); delayed.keyReadback(); delayed.cancel(); delayed.stopWire(); delayed.stopReadback();
    check(delayed.evidence.valid(delayed.now)
          && !delayed.evidence.valid(delayed.now + FlexPttStopTracker::kTransitionTimeoutMs),
          "receiver rejects expired certificate even before transport timer delivery");
}

struct DesktopHarness {
    qint64 now{100};
    QStringList commands;
    bool fullWrite{true};
    TxStopEvidence evidence;
    TxCoordinator coordinator{[](const auto&, auto) {}, [this] { return now; }};
    TxCoordinator::Actor actor{coordinator.registerActor({true, 0})};
    TxCoordinator::Actor competitor{coordinator.registerActor({true, 10000, 100000, 1, true})};
    TxCoordinator::Operation operation{coordinator.acquire(actor, now).operation};
    FlexPttWireSession wire{[this](quint32, const QString& text) {
        commands.append(text);
        return fullWrite;
    }, [this](const TxStopEvidence& proof) { evidence = proof; }};
    DesktopHarness() { wire.reset(1, 0x12345678); feed(kIdle); }
    void feed(QStringView line) { wire.observe(line, now); }
    void edge(bool keying, const TxCoordinator::Operation& original)
    {
        const TxCoordinator::Dispatch entered = original.beginDispatch(now, keying);
        if (entered) {
            wire.desktopPtt(keying ? 101 : 102, {original, keying}, now);
        }
    }
    void key() { edge(true, operation); feed(u"R101|0|"); feed(kRequested); feed(kTransmitting); }
    TxCoordinator::StopRequest bind()
    {
        check(coordinator.finishLocalIntent(operation), "desktop local completion fences its original operation");
        const TxCoordinator::StopRequest request = coordinator.requestStopConfirmation(operation);
        wire.stop(999, operation, request, now);
        return request;
    }
};

void desktopReleaseBinding()
{
    const QList<QStringView> release{u"R102|0|", kUnkey, kReadyOwned, kIdle};
    for (int bindAfter = 0; bindAfter <= release.size(); ++bindAfter) {
        DesktopHarness h;
        h.key(); h.edge(false, h.operation);
        TxCoordinator::StopRequest request;
        if (bindAfter == 0) { request = h.bind(); }
        for (int index = 0; index < release.size(); ++index) {
            h.feed(release[index]);
            if (index + 1 < release.size() || !request.valid()) {
                check(!h.evidence.valid(h.now) && h.coordinator.hasOwnership(),
                      "desktop handoff needs original stop token and every terminal release observation");
            }
            if (index + 1 == bindAfter) { request = h.bind(); }
        }
        check(h.evidence.valid(h.now) && h.evidence.request.sameRequest(request)
              && h.commands == QStringList{"xmit 1", "xmit 0"},
              "binding before or after readback qualifies only the recorded desktop lifecycle without another command");
        check(h.coordinator.acquire(h.competitor, h.now).refusal == TxCoordinator::Refusal::Busy,
              "wire certificate alone retains desktop ownership until model confirmation");
        check(h.coordinator.confirmStopped(request) && h.operation.hasConfirmedStop(),
              "exact desktop operation accepts full qualified stop proof");
        h.wire.poll(++h.now);
        check(h.wire.ready() && h.coordinator.acquire(h.competitor, h.now).accepted()
              && h.commands.size() == 2,
              "fresh analyzer actor can acquire after confirmed desktop release without automatic keying");
    }
}

void desktopReleaseRefusals()
{
    for (int omitted = 0; omitted < 7; ++omitted) {
        DesktopHarness h;
        const QList<QStringView> readback{u"R101|0|", kRequested, kTransmitting,
                                         u"R102|0|", kUnkey, kReadyOwned, kIdle};
        h.edge(true, h.operation);
        for (int index = 0; index < readback.size(); ++index) {
            if (index == 3) { h.edge(false, h.operation); (void)h.bind(); }
            if (index != omitted) { h.feed(readback[index]); }
        }
        check(!h.evidence.valid(h.now) && h.coordinator.hasOwnership(),
              "desktop release cannot omit any key/stop acknowledgment or ownership transition");
    }
    for (int failedEdge : {0, 1}) {
        DesktopHarness h;
        h.fullWrite = failedEdge != 0;
        h.key();
        h.fullWrite = failedEdge != 1;
        h.edge(false, h.operation); (void)h.bind();
        h.feed(u"R102|0|"); h.feed(kUnkey); h.feed(kReadyOwned); h.feed(kIdle);
        check(!h.evidence.valid(h.now) && h.commands == QStringList{"xmit 1", "xmit 0"},
              "partial desktop writes preserve ordinary PTT commands but refuse handoff evidence");
    }
    DesktopHarness anonymous;
    anonymous.key();
    anonymous.edge(false, anonymous.coordinator.cleanupFence());
    (void)anonymous.bind();
    anonymous.feed(u"R102|0|"); anonymous.feed(kUnkey); anonymous.feed(kReadyOwned); anonymous.feed(kIdle);
    check(!anonymous.evidence.valid(anonymous.now), "anonymous cleanup cannot borrow a desktop operation's key history");

    DesktopHarness noDispatch;
    check(noDispatch.coordinator.finishLocalIntent(noDispatch.operation), "unwritten desktop operation locally completes");
    const auto request = noDispatch.coordinator.requestStopConfirmation(noDispatch.operation);
    noDispatch.wire.stop(102, noDispatch.operation, request, noDispatch.now);
    check(noDispatch.commands.isEmpty() && !noDispatch.evidence.valid(noDispatch.now),
          "desktop operations never inherit independent local no-dispatch proof");

    DesktopHarness expiry;
    expiry.key(); expiry.edge(false, expiry.operation);
    expiry.feed(u"R102|0|"); expiry.feed(kUnkey); expiry.feed(kReadyOwned); expiry.feed(kIdle);
    expiry.now += FlexPttStopTracker::kTransitionTimeoutMs;
    (void)expiry.bind();
    check(!expiry.evidence.valid(expiry.now), "delayed desktop token cannot renew the original off-write deadline");

    DesktopHarness stale;
    stale.key(); stale.edge(false, stale.operation); (void)stale.bind();
    stale.feed(u"R102|0|"); stale.feed(kUnkey); stale.feed(kReadyOwned); stale.feed(kIdle);
    const TxStopEvidence proof = stale.evidence;
    stale.wire.disconnect();
    check(!proof.valid(stale.now), "desktop certificate is revoked at transport teardown before model delivery");

    DesktopHarness superseded;
    superseded.key(); superseded.edge(false, superseded.operation);
    (void)superseded.bind();
    const auto replacement = superseded.coordinator.requestStopConfirmation(superseded.operation);
    superseded.wire.stop(103, superseded.operation, replacement, superseded.now);
    superseded.feed(u"R102|0|"); superseded.feed(kUnkey); superseded.feed(kReadyOwned); superseded.feed(kIdle);
    check(!superseded.evidence.valid(superseded.now),
          "superseded desktop stop token cannot reuse the earlier terminal off write");

    DesktopHarness lateContradiction;
    lateContradiction.key(); lateContradiction.edge(false, lateContradiction.operation);
    lateContradiction.feed(u"R102|0|"); lateContradiction.feed(kUnkey);
    lateContradiction.feed(kReadyOwned); lateContradiction.feed(kIdle);
    lateContradiction.feed(u"S0|interlock tx_client_handle=0x87654321 state=TRANSMITTING reason= source=SW tx_allowed=1");
    (void)lateContradiction.bind();
    check(!lateContradiction.evidence.valid(lateContradiction.now),
          "contradiction before desktop token binding invalidates the recorded release sequence");
}

void desktopCleanupAfterConfirmedRelease()
{
    DesktopHarness h;
    h.key(); h.edge(false, h.operation); (void)h.bind();
    h.feed(u"R102|0|"); h.feed(kUnkey); h.feed(kReadyOwned); h.feed(kIdle);
    check(h.coordinator.confirmStopped(h.evidence.request), "initial desktop release is qualified");
    h.wire.poll(++h.now);
    check(h.wire.ready(), "consumed desktop release permits initial analyzer readiness");
    {
        const auto dispatch = h.operation.beginDispatch(h.now, false);
        check(bool(dispatch), "explicit Stop retains only the confirmed operation's cleanup authority");
        h.wire.desktopPtt(103, {h.operation, false}, h.now);
    }
    check(!h.wire.ready() && !h.evidence.valid(h.now) && h.operation.hasConfirmedStop(),
          "additional Stop withdraws readiness without manufacturing another stop certificate");
    h.feed(kIdle);
    check(!h.wire.ready(), "idle before the extra Stop acknowledgment cannot restore readiness");
    h.feed(u"R103|0|");
    h.feed(u"S87654321|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1");
    check(!h.wire.ready(), "helper-triggered idle cannot finish the extra Stop readiness barrier");
    h.feed(u"S0|interlock tx_allowed=1");
    check(!h.wire.ready(), "extra Stop acknowledgment and partial idle cannot restore readiness");
    h.feed(kIdle);
    check(h.wire.ready() && !h.evidence.valid(h.now) && !h.coordinator.hasOwnership()
          && h.commands == QStringList{"xmit 1", "xmit 0", "xmit 0"},
          "fresh complete idle after extra Stop restores readiness without reconnect or another key");

    DesktopHarness repeated;
    repeated.key(); repeated.edge(false, repeated.operation); (void)repeated.bind();
    repeated.feed(u"R102|0|"); repeated.feed(kUnkey); repeated.feed(kReadyOwned); repeated.feed(kIdle);
    check(repeated.coordinator.confirmStopped(repeated.evidence.request), "second fixture confirms original release");
    repeated.wire.desktopPtt(103, {repeated.operation, false}, repeated.now);
    repeated.now += 4000;
    repeated.wire.desktopPtt(104, {repeated.operation, false}, repeated.now);
    repeated.now += 1000;
    repeated.feed(u"R104|0|"); repeated.feed(kIdle);
    check(!repeated.wire.ready() && !repeated.evidence.valid(repeated.now),
          "repeated extra Stop cannot renew the first cleanup deadline or infer idle on expiry");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    acknowledgedStopRemainsReady();
    protocolEligibility();
    protocolLossRetainsStop();
    dspDeathHelperSnapshotRecovery();
    noDispatch();
    rapidReleaseAndHandoff();
    helperSubscriptionsDuringRelease();
    independentReleaseBeforeDesktopStop();
    independentReleaseOverlapsStop();
    independentReleaseRefusals();
    partialAndStale();
    desktopReleaseBinding();
    desktopReleaseRefusals();
    desktopCleanupAfterConfirmedRelease();
    return failures ? 1 : 0;
}
