#include "core/backends/flex/FlexPttStopTracker.h"

#include <QCoreApplication>

#include <cstdio>
#include <limits>
#include <thread>

using AetherSDR::FlexPttStopTracker;
using AetherSDR::TxCoordinator;
using Phase = FlexPttStopTracker::Phase;
using Failure = FlexPttStopTracker::Failure;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}

// Actual FLEX-6700 4.2.18.41174 LAN transition shapes captured through the
// native bridge on 2026-09-18. Only the ephemeral client handle and command
// sequences are normalized. These are input frames, never a firmware peer.
constexpr QStringView kIdle = u"S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1 amplifier=";
constexpr QStringView kRequested = u"S0|interlock tx_client_handle=0x12345678 state=PTT_REQUESTED reason= source=SW tx_allowed=1 amplifier=";
constexpr QStringView kTransmitting = u"S0|interlock tx_client_handle=0x12345678 state=TRANSMITTING reason= source=SW tx_allowed=1 amplifier=";
constexpr QStringView kUnkey = u"S0|interlock tx_client_handle=0x12345678 state=UNKEY_REQUESTED reason= source= tx_allowed=1 amplifier=";
constexpr QStringView kReadyOwned = u"S0|interlock tx_client_handle=0x12345678 state=READY reason= source= tx_allowed=1 amplifier=";
constexpr QStringView kOutOfBand = u"S0|interlock tx_client_handle=0x00000000 state=NOT_READY reason=OUT_OF_BAND source= tx_allowed=0 amplifier=";

struct Harness {
    qint64 now{100};
    quint64 ordinal{0};
    TxCoordinator::StopRequest stop;
    TxCoordinator coordinator{[this](const auto& operation, auto) {
        stop = coordinator.requestStopConfirmation(operation);
    }, [this] { return now; }};
    TxCoordinator::Actor actor{coordinator.registerActor({true, 10000, 100000, 1, true})};
    TxCoordinator::Actor other{coordinator.registerActor({true, 10000, 100000, 1, true})};
    TxCoordinator::Operation operation{coordinator.acquire(actor, now).operation};
    FlexPttStopTracker tracker;

    Harness()
    {
        tracker.reset(1, 0x12345678);
        feed(kIdle);
    }
    void feed(QStringView line) { tracker.observe({1, ++ordinal}, line, now); }
    void written(quint32 sequence, bool key, bool success = true)
    { tracker.commandWritten({1, ++ordinal}, sequence, key, success, now); }
    void start()
    {
        check(tracker.begin(operation, 101, now), "arm original key command identity");
        written(101, true);
        feed(u"R101|0|");
        feed(kRequested);
        feed(kTransmitting);
        check(tracker.phase() == Phase::Transmitting, "own software PTT observed after matched key reply");
    }
    void stopRequest()
    {
        check(coordinator.cancel(actor, operation), "coordinator fences operation before stop");
        check(tracker.requestStop(operation, stop, 102, now), "bind exact operation and stop attempt");
    }
    void stopReply()
    {
        stopRequest();
        written(102, false);
        feed(u"R102|0|");
    }
    void complete()
    {
        stopReply();
        feed(kUnkey);
        feed(kReadyOwned);
        feed(kIdle);
    }
};

void capturedSequenceAndHandoff()
{
    Harness h;
    h.start();
    h.stopReply();
    check(!h.tracker.evidence(h.now).valid(), "successful xmit-off reply is not stop proof");
    h.feed(kUnkey);
    check(!h.tracker.evidence(h.now).valid(), "UNKEY_REQUESTED is transitional");
    h.feed(kReadyOwned);
    check(!h.tracker.evidence(h.now).valid(), "READY retaining our client handle cannot release ownership");
    check(h.coordinator.acquire(h.other, h.now).refusal == TxCoordinator::Refusal::Recovering,
          "another actor remains refused while firmware retains the old owner");
    h.now += 427; // measured gap, not a grace-period heuristic
    h.feed(kIdle);
    const auto candidate = h.tracker.evidence(h.now);
    check(candidate.sameRequest(h.stop), "later owner-clear completes the captured candidate sequence");
    check(h.coordinator.confirmStopped(candidate), "test-injected candidate crosses coordinator stop barrier");
    check(!h.tracker.evidence(h.now).valid(), "acknowledgment retires the candidate token");
    h.now += 6000;
    const auto next = h.coordinator.acquire(h.other, h.now);
    check(next.accepted() && h.tracker.begin(next.operation, 103, h.now),
          "fresh actor and sequence can begin after acknowledgment, even after old timeout");
    check(!h.coordinator.confirmStopped(candidate), "old proof cannot release replacement operation");
}

void enteredWriterBarrier()
{
    Harness h;
    h.start();
    auto writer = h.operation.beginDispatch(h.now);
    check(bool(writer), "enter terminal writer before cancellation");
    h.complete();
    check(h.tracker.evidence(h.now).valid()
          && !h.coordinator.confirmStopped(h.tracker.evidence(h.now)),
          "candidate evidence cannot release an entered terminal writer");
    writer = {};
    check(h.coordinator.confirmStopped(h.tracker.evidence(h.now)),
          "same candidate can complete after writer exits, without a new stop attempt");
}

void missingAndReorderedTransitions()
{
    Harness noUnkey;
    noUnkey.start();
    noUnkey.stopReply();
    noUnkey.feed(kReadyOwned);
    noUnkey.feed(kIdle);
    check(noUnkey.tracker.phase() == Phase::Failed && !noUnkey.tracker.evidence(noUnkey.now).valid(),
          "idle with no observed UNKEY_REQUESTED cannot fill in the missing transition");

    Harness noOwnedReady;
    noOwnedReady.start();
    noOwnedReady.stopReply();
    noOwnedReady.feed(kUnkey);
    noOwnedReady.feed(kIdle);
    check(noOwnedReady.tracker.phase() == Phase::Failed, "owner-clear alone cannot skip READY with old owner");

    Harness beforeReply;
    beforeReply.start();
    beforeReply.stopRequest();
    beforeReply.written(102, false);
    beforeReply.feed(kUnkey);
    beforeReply.feed(u"R102|0|");
    beforeReply.feed(kReadyOwned);
    beforeReply.feed(kIdle);
    check(!beforeReply.tracker.evidence(beforeReply.now).valid(),
          "status before the matching stop reply cannot be relabeled as later evidence");

    Harness early;
    check(early.tracker.begin(early.operation, 101, early.now), "prepare early cancellation");
    early.written(101, true);
    check(early.coordinator.cancel(early.actor, early.operation), "cancel before keyed status");
    check(early.tracker.requestStop(early.operation, early.stop, 102, early.now),
          "cancel-before-observed-keying binds stop without delaying unkey");
    early.feed(kIdle);
    check(!early.tracker.evidence(early.now).valid(), "preexisting idle is not no-dispatch proof");

    Harness duplicate;
    duplicate.start();
    duplicate.stopReply();
    duplicate.tracker.observe({1, duplicate.ordinal}, kUnkey, duplicate.now);
    check(duplicate.tracker.failure() == Failure::Ordering, "duplicate current-session ordinal poisons attempt");
}

void strictFramesAndOwnership()
{
    const QStringList replies{QStringLiteral("R102|garbage|"), QStringLiteral("R102||"),
        QStringLiteral("R102|"), QStringLiteral("R102|100000000|"), QStringLiteral("R102|F3000001|"),
        QStringLiteral("R102|+0|"), QStringLiteral("R102| 0|"), QStringLiteral("R4294967296|0|")};
    for (const QString& reply : replies) {
        Harness h;
        h.start();
        h.stopRequest();
        h.written(102, false);
        h.feed(reply);
        check(h.tracker.failure() == Failure::Reply, "invalid/failed reply cannot become result zero");
    }
    const QStringList badStatuses{
        QStringLiteral("S0|interlock state=UNKEY_REQUESTED"),
        QStringLiteral("S0|interlock tx_client_handle=garbage state=UNKEY_REQUESTED reason= source= tx_allowed=1"),
        QStringLiteral("S0|interlock tx_client_handle=0x12345678 state=UNKEY_REQUESTED state=READY reason= source= tx_allowed=1"),
        QStringLiteral("S0|interlock tx_client_handle=0x12345678 state=UNKEY_REQUESTED reason= source=SW source= tx_allowed=1"),
        QStringLiteral("S0|interlock tx_client_handle=0x12345678 state=UNKEY_REQUESTED reason= source= SW tx_allowed=1"),
        QStringLiteral("Sg|interlock tx_client_handle=0x12345678 state=UNKEY_REQUESTED reason= source= tx_allowed=1"),
        QStringLiteral("S0|interlock tx_client_handle=0x87654321 state=UNKEY_REQUESTED reason= source= tx_allowed=1"),
        QStringLiteral("S87654321|interlock tx_client_handle=0x12345678 state=UNKEY_REQUESTED reason= source= tx_allowed=1"),
        QStringLiteral("S0|interlock tx_client_handle=0x12345678 state=UNKEY_REQUESTED reason= source=MIC tx_allowed=1")};
    for (const QString& status : badStatuses) {
        Harness h;
        h.start();
        h.stopReply();
        h.feed(status);
        h.feed(kReadyOwned);
        h.feed(kIdle);
        check(h.tracker.phase() == Phase::Failed && !h.tracker.evidence(h.now).valid(),
              "malformed/partial/foreign/physical-key status cannot authorize handoff");
    }
    Harness outOfBand;
    outOfBand.start();
    outOfBand.stopReply();
    outOfBand.feed(kUnkey);
    outOfBand.feed(kReadyOwned);
    outOfBand.feed(kOutOfBand); // actual third capture after restoring receive frequency
    check(!outOfBand.tracker.evidence(outOfBand.now).valid(),
          "observed OUT_OF_BAND owner-clear is not a qualified READY handoff");

    Harness unrelated;
    unrelated.start();
    unrelated.stopReply();
    unrelated.feed(u"R999|0|");
    unrelated.feed(u"S0|interlock acc_tx_delay=0 tx_delay=0 timeout=0");
    unrelated.feed(u"S0|interlock band 24 band_name=GEN acc_tx_enabled=0");
    unrelated.feed(u"S0|atu status=TUNE_BYPASS atu_enabled=1 memories_enabled=1 using_mem=1");
    unrelated.feed(kUnkey);
    unrelated.feed(kReadyOwned);
    unrelated.feed(kIdle);
    check(unrelated.tracker.evidence(unrelated.now).valid(), "unrelated real status/replies do not stand in for transitions");
}

void partialInterlockBeforeArming()
{
    const QStringList deltas{
        QStringLiteral("S0|interlock tx_allowed=0"),
        QStringLiteral("S0|interlock tx_allowed=1"),
        QStringLiteral("S0|interlock state=READY"),
        QStringLiteral("S0|interlock source="),
        QStringLiteral("S0|interlock reason="),
        QStringLiteral("S0|interlock tx_client_handle=0x00000000")};
    for (const QString& delta : deltas) {
        Harness h;
        h.feed(delta);
        check(h.tracker.phase() == Phase::AwaitIdle && h.tracker.failure() == Failure::None,
              "valid partial interlock withdraws idle without poisoning the session");
        check(!h.tracker.begin(h.operation, 101, h.now), "partial idle never permits key admission");
        h.feed(kIdle);
        check(h.tracker.phase() == Phase::Idle, "later complete idle recovers without reconnect");
        h.start();
        h.complete();
        check(h.tracker.evidence(h.now).valid(), "recovered session retains complete stop evidence requirements");

        Harness active;
        active.start();
        active.feed(delta);
        active.feed(kIdle);
        check(active.tracker.phase() == Phase::Failed && !active.tracker.evidence(active.now).valid(),
              "partial status during an armed lifecycle remains terminal and cannot certify stop");
    }
    FlexPttStopTracker fresh;
    fresh.reset(1, 0x12345678);
    quint64 ordinal = 0;
    for (const QString& delta : deltas) {
        fresh.observe({1, ++ordinal}, delta, 100);
        check(fresh.phase() == Phase::AwaitIdle && fresh.failure() == Failure::None,
              "initial partial updates never accumulate into complete idle evidence");
    }
    fresh.observe({1, ++ordinal}, kIdle, 100);
    check(fresh.phase() == Phase::Idle, "complete idle after initial deltas admits readiness");

    for (const QString& malformed : {
             QStringLiteral("S0|interlock tx_allowed=garbage"),
             QStringLiteral("S0|interlock tx_client_handle=garbage"),
             QStringLiteral("S0|interlock tx_allowed=1 tx_allowed=0"),
             QStringLiteral("S0|interlock state=READY junk")}) {
        Harness h;
        h.feed(malformed);
        h.feed(kIdle);
        check(h.tracker.failure() == Failure::InvalidInput,
              "malformed fields still fail closed before arming");
    }
}

void attemptIdentityAndReconnect()
{
    Harness h;
    h.start();
    h.complete();
    const auto old = h.tracker.evidence(h.now);
    const auto replacement = h.coordinator.requestStopConfirmation(h.operation);
    check(replacement.valid() && !old.valid() && !h.tracker.evidence(h.now).valid(),
          "superseding attempt invalidates captured evidence without new radio events");
    check(!h.tracker.requestStop(h.operation, replacement, 103, h.now),
          "new token cannot be attached to an old READY sequence");

    Harness wrong;
    Harness foreign;
    wrong.start();
    foreign.start();
    check(wrong.coordinator.cancel(wrong.actor, wrong.operation)
          && foreign.coordinator.cancel(foreign.actor, foreign.operation), "create distinct stopping operations");
    check(!foreign.stop.matchesOperation(wrong.operation)
          && !wrong.tracker.requestStop(wrong.operation, foreign.stop, 102, wrong.now),
          "valid stop token from another coordinator cannot confirm this operation");

    Harness reconnect;
    reconnect.start();
    reconnect.complete();
    reconnect.tracker.reset(2, 0x87654321);
    reconnect.tracker.observe({1, ++reconnect.ordinal}, kIdle, reconnect.now);
    check(reconnect.tracker.phase() == Phase::AwaitIdle
          && !reconnect.tracker.evidence(reconnect.now).valid(), "old transport delivery is inert after reconnect");
    reconnect.tracker.reset(2, 0x87654321);
    check(reconnect.tracker.phase() == Phase::Disconnected, "transport session identity cannot be reused");
    reconnect.tracker.reset(3, 0);
    check(reconnect.tracker.phase() == Phase::Disconnected, "handle zero cannot identify a controlling client");
}

void failedWritesDeadlinesAndLateEvents()
{
    Harness h;
    h.start();
    h.stopRequest();
    h.written(102, false, false);
    h.feed(u"R102|0|");
    h.feed(kUnkey);
    h.feed(kReadyOwned);
    h.feed(kIdle);
    check(h.tracker.failure() == Failure::Write && !h.tracker.evidence(h.now).valid(),
          "partial/failed terminal write cannot be repaired by later idle");

    Harness extra;
    extra.start();
    extra.stopReply();
    extra.written(103, true);
    check(extra.tracker.failure() == Failure::UnsupportedActivity,
          "unexpected TX writer invalidates software-PTT-only attempt");

    Harness timeout;
    timeout.start();
    timeout.stopReply();
    timeout.now += FlexPttStopTracker::kTransitionTimeoutMs;
    timeout.tracker.poll(timeout.now);
    timeout.feed(kUnkey);
    timeout.feed(kReadyOwned);
    timeout.feed(kIdle);
    check(timeout.tracker.failure() == Failure::Timeout && !timeout.tracker.evidence(timeout.now).valid(),
          "exact deadline is a refusal, not a delayed idle certificate");

    Harness confirmed;
    confirmed.start();
    confirmed.complete();
    check(!confirmed.tracker.evidence(confirmed.now + FlexPttStopTracker::kTransitionTimeoutMs).valid(),
          "candidate expires even if no owner-thread timer runs");
    confirmed.feed(kTransmitting);
    check(!confirmed.tracker.evidence(confirmed.now).valid(), "new TX observation revokes unconsumed candidate");

    Harness backwards;
    backwards.start();
    backwards.tracker.poll(backwards.now - 1);
    check(backwards.tracker.failure() == Failure::InvalidInput, "clock regression fails closed");
    Harness overflow;
    overflow.tracker.poll(std::numeric_limits<qint64>::max());
    check(overflow.tracker.failure() == Failure::InvalidInput, "deadline arithmetic cannot overflow");
    Harness bounded;
    bounded.feed(QString(FlexPttStopTracker::kMaximumLineSize + 1, u'x'));
    check(bounded.tracker.failure() == Failure::InvalidInput, "oversized evidence input is bounded");

    Harness thread;
    thread.start();
    thread.complete();
    bool acceptedOffThread = false;
    std::thread worker([&] { acceptedOffThread = thread.tracker.evidence(thread.now).valid(); });
    worker.join();
    check(!acceptedOffThread && thread.tracker.evidence(thread.now).valid(),
          "off-thread evidence inspection cannot acknowledge or mutate owner state");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    capturedSequenceAndHandoff();
    enteredWriterBarrier();
    missingAndReorderedTransitions();
    strictFramesAndOwnership();
    partialInterlockBeforeArming();
    attemptIdentityAndReconnect();
    failedWritesDeadlinesAndLateEvents();
    return failures == 0 ? 0 : 1;
}
