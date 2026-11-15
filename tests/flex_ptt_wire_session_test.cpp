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
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    protocolEligibility();
    protocolLossRetainsStop();
    noDispatch();
    rapidReleaseAndHandoff();
    partialAndStale();
    return failures ? 1 : 0;
}
