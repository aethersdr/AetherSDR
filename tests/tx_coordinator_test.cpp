#include "core/TxCoordinator.h"

#include <QCoreApplication>
#include <QThread>
#include <QSemaphore>

#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

using AetherSDR::TxCoordinator;
using Activity = TxCoordinator::Activity;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}

void ownershipAndRecovery()
{
    int stops = 0;
    TxCoordinator::Operation stopped;
    TxCoordinator::Actor competitor;
    TxCoordinator* callbackCoordinator = nullptr;
    TxCoordinator coordinator([&](const TxCoordinator::Operation& operation,
                                 TxCoordinator::StopReason) {
        ++stops;
        stopped = operation;
        check(!operation.permitsDispatch(100), "stop invalidates delivery before callback");
        check(callbackCoordinator->acquire(competitor, 100).refusal == TxCoordinator::Refusal::Recovering,
              "reentrant competing start cannot race stop cleanup");
    });
    callbackCoordinator = &coordinator;
    const TxCoordinator::Actor owner = coordinator.registerActor({true, 0});
    competitor = coordinator.registerActor({true, 0});
    const TxCoordinator::Actor observer = coordinator.registerActor({false, 0});
    check(coordinator.acquire({}, 0).refusal == TxCoordinator::Refusal::InvalidActor,
          "default actor has no authority");
    check(coordinator.acquire(observer, 0).refusal == TxCoordinator::Refusal::Denied,
          "authenticated actor without transmit permission cannot acquire");
    const TxCoordinator::Admission first = coordinator.acquire(owner, 0);
    check(first.accepted() && first.operation.permitsDispatch(0), "owner acquires usable operation");
    check(stops == 0, "acquiring never dispatches stop or keying");
    check(coordinator.acquire(competitor, 1).refusal == TxCoordinator::Refusal::Busy,
          "second actor cannot take the owner slot");
    check(coordinator.acquire(owner, 2).operation.sameOperation(first.operation),
          "repeated start retains the existing operation");
    check(!coordinator.cancel(competitor, first.operation), "nonowner cannot stop owner");
    check(!coordinator.finishLocalIntent({}), "invalid handle cannot finish another operation");
    check(coordinator.cancel(owner, first.operation) && stops == 1,
          "owner cancellation dispatches one stop");
    check(!coordinator.cancel(owner, first.operation) && stops == 1,
          "duplicate cancellation is inert");
    check(!coordinator.acknowledgeStopped({}), "unrelated stop acknowledgment cannot clear recovery");
    check(coordinator.acknowledgeStopped(stopped), "matching qualified stop acknowledgment clears recovery");
    const TxCoordinator::Admission next = coordinator.acquire(competitor, 101);
    check(next.accepted(), "next actor can acquire after recovery");
    check(!coordinator.finishLocalIntent(first.operation), "late old completion cannot stop new owner");
    check(!coordinator.acknowledgeStopped(stopped), "late old acknowledgment is inert");
    check(next.operation.permitsDispatch(102), "new owner survives old callbacks");
    check(coordinator.finishLocalIntent(next.operation), "local intent completes normally");
    check(!next.operation.permitsDispatch(103), "normal completion fences queued work");
    check(next.operation.permitsCleanup(), "normal completion retains queued key-up cleanup");
    check(coordinator.acquire(owner, 104).refusal == TxCoordinator::Refusal::Busy,
          "local completion cannot authorize a different owner");
    check(coordinator.acknowledgeStopped(next.operation), "qualified normal stop releases the owner slot");
    check(coordinator.acquire(owner, 104).accepted(), "new owner starts after qualified completion");
    check(!next.operation.permitsCleanup(), "old queued key-up cannot unkey a newer operation");
    check(stops == 1, "normal completion never sends an extra unkey");
}

void expiryAndRevocation()
{
    int stops = 0;
    TxCoordinator::Operation stopped;
    TxCoordinator::StopReason reason = TxCoordinator::StopReason::Reset;
    TxCoordinator coordinator([&](const TxCoordinator::Operation& operation,
                                 TxCoordinator::StopReason why) {
        ++stops;
        stopped = operation;
        reason = why;
    });
    const TxCoordinator::Actor bounded = coordinator.registerActor({true, 20});
    const TxCoordinator::Actor other = coordinator.registerActor({true, 0});
    const TxCoordinator::Admission first = coordinator.acquire(bounded, 100);
    check(first.operation.permitsDispatch(119), "bounded operation survives before deadline");
    check(!first.operation.permitsDispatch(120), "transport fence rejects exact deadline before timer runs");
    check(coordinator.acquire(bounded, 119).accepted(), "repeat admission before deadline succeeds");
    coordinator.expire(120);
    check(stops == 1 && reason == TxCoordinator::StopReason::Expired,
          "repeat acquisition cannot extend maximum continuous duration");
    coordinator.expire(121);
    check(stops == 1, "expired operation stops only once");
    check(coordinator.acknowledgeStopped(stopped), "expiry recovery acknowledged");
    const TxCoordinator::Admission second = coordinator.acquire(bounded, 122);
    coordinator.revoke(other);
    check(second.operation.permitsDispatch(123) && stops == 1,
          "revoking another actor does not affect owner");
    coordinator.revoke(bounded);
    check(stops == 2 && reason == TxCoordinator::StopReason::ActorRevoked,
          "owner revocation cancels active transmission");
    check(!second.operation.permitsDispatch(123), "revocation fences pending output");
    check(coordinator.acknowledgeStopped(stopped), "revocation recovery acknowledged");
    check(coordinator.acquire(bounded, 124).refusal == TxCoordinator::Refusal::InvalidActor,
          "revoked actor cannot reacquire");
}

void unconfirmedCompletion()
{
    int stops = 0;
    TxCoordinator coordinator([&](const auto&, auto) { ++stops; });
    const TxCoordinator::Actor owner = coordinator.registerActor({true, 0});
    const TxCoordinator::Actor other = coordinator.registerActor({true, 0});
    const TxCoordinator::Operation first = coordinator.acquire(owner, 0).operation;
    check(!coordinator.acknowledgeStopped(first), "active intent cannot be acknowledged as stopped");
    check(coordinator.finishLocalIntent(first), "local completion records the preceding owner");
    check(!coordinator.finishLocalIntent(first), "duplicate local completion is inert");
    check(coordinator.owns(owner, first) && !coordinator.owns(other, first),
          "unconfirmed tail retains only its original owner's cancellation authority");
    check(!first.permitsDispatch(1) && first.permitsCleanup(),
          "unconfirmed tail admits cleanup but cannot send more key-on work");
    check(!coordinator.recovering() && stops == 0,
          "normal completion does not force cleanup or latch operator admission");
    check(coordinator.acquire(other, 1).refusal == TxCoordinator::Refusal::Busy,
          "a competitor cannot treat local drain as radio-idle evidence");
    check(!coordinator.cancel(other, first), "competitor cannot cancel an unconfirmed tail");
    std::unique_ptr<QThread> worker(QThread::create([&] {
        check(!coordinator.finishLocalIntent(first) && !coordinator.acknowledgeStopped(first)
                  && !coordinator.cancel(owner, first),
              "worker-thread completion, acknowledgment and cancellation cannot mutate a tail");
    }));
    worker->start();
    worker->wait();
    check(coordinator.owns(owner, first), "off-thread calls leave unconfirmed ownership intact");
    const TxCoordinator::Operation second = coordinator.acquire(owner, 2).operation;
    check(second.permitsDispatch(std::numeric_limits<qint64>::max())
              && !second.sameOperation(first) && !first.permitsCleanup(),
          "same unbounded operator reengages with a new queue generation and no timeout");
    check(!coordinator.acknowledgeStopped(first) && !coordinator.finishLocalIntent(first),
          "an old completion cannot release ownership after same-actor reengagement");
    check(coordinator.finishLocalIntent(second), "reengaged operator can release again");
    check(!coordinator.acknowledgeStopped(first)
              && coordinator.acquire(other, 3).refusal == TxCoordinator::Refusal::Busy,
          "an old readback cannot release a newer unconfirmed tail");
    check(coordinator.acknowledgeStopped(second), "only matching qualified completion releases ownership");
    check(coordinator.acquire(other, 4).accepted(), "handoff is available after the qualified stop");
    check(stops == 0, "qualified normal completion never sends a redundant stop");
}

void unconfirmedStopSources()
{
    for (const TxCoordinator::StopReason reason : {TxCoordinator::StopReason::OwnerCancelled,
             TxCoordinator::StopReason::ActorRevoked, TxCoordinator::StopReason::Reset,
             TxCoordinator::StopReason::Emergency}) {
        int stops = 0;
        TxCoordinator::Operation stopped;
        TxCoordinator::StopReason actual = TxCoordinator::StopReason::Expired;
        TxCoordinator* current = nullptr;
        TxCoordinator::Actor other;
        TxCoordinator coordinator([&](const TxCoordinator::Operation& operation, TxCoordinator::StopReason why) {
            ++stops;
            stopped = operation;
            actual = why;
            check(current->recovering() && !operation.permitsDispatch(2),
                  "a tail stop publishes recovery and cancellation before callback");
            check(current->acquire(other, 2).refusal == TxCoordinator::Refusal::Recovering,
                  "tail cleanup cannot race reentrant admission");
        });
        current = &coordinator;
        const TxCoordinator::Actor owner = coordinator.registerActor({true, 0});
        other = coordinator.registerActor({true, 0});
        const TxCoordinator::Operation operation = coordinator.acquire(owner, 0).operation;
        check(coordinator.finishLocalIntent(operation), "stop source fixture has an unconfirmed tail");
        switch (reason) {
        case TxCoordinator::StopReason::OwnerCancelled:
            check(coordinator.cancel(owner, operation), "owner may cancel its unconfirmed tail");
            break;
        case TxCoordinator::StopReason::ActorRevoked:
            coordinator.revoke(owner);
            break;
        case TxCoordinator::StopReason::Reset:
            coordinator.reset();
            break;
        case TxCoordinator::StopReason::Emergency:
            coordinator.emergencyStop();
            break;
        case TxCoordinator::StopReason::Expired:
            break;
        }
        check(stops == 1 && actual == reason && stopped.sameOperation(operation),
              "every stop source retains the exact unconfirmed operation");
        coordinator.emergencyStop();
        check(stops == 1, "duplicate tail stop is inert");
        check(coordinator.acknowledgeStopped(stopped), "tail recovery accepts matching qualified acknowledgment");
        check(coordinator.acquire(other, 3).accepted(), "tail recovery does not permanently latch admission");
    }
}

void unconfirmedDeadline()
{
    int stops = 0;
    TxCoordinator::Operation stopped;
    TxCoordinator coordinator([&](const TxCoordinator::Operation& operation, TxCoordinator::StopReason reason) {
        ++stops;
        stopped = operation;
        check(reason == TxCoordinator::StopReason::Expired, "unconfirmed deadline uses expiry cleanup");
    });
    const TxCoordinator::Actor owner = coordinator.registerActor({true, 20});
    const TxCoordinator::Operation first = coordinator.acquire(owner, 100).operation;
    check(coordinator.finishLocalIntent(first), "bounded operation finishes local intent");
    coordinator.expire(118);
    check(stops == 0, "local fence cancellation is not itself deadline expiry");
    const TxCoordinator::Operation second = coordinator.acquire(owner, 119).operation;
    check(second.permitsDispatch(119) && !second.permitsDispatch(120),
          "reengagement without stop evidence cannot renew the original deadline");
    check(coordinator.finishLocalIntent(second), "bounded reengagement finishes local intent");
    coordinator.expire(120);
    check(stops == 1 && stopped.sameOperation(second), "an unconfirmed tail expires at the original deadline");
    check(coordinator.acknowledgeStopped(stopped), "expired tail acknowledges through the existing recovery path");
    const TxCoordinator::Operation third = coordinator.acquire(owner, 121).operation;
    check(third.permitsDispatch(140) && !third.permitsDispatch(141),
          "qualified recovery permits a fresh bounded duration");
    check(coordinator.finishLocalIntent(third), "fresh duration ends local intent before acquire-time expiry");
    check(coordinator.acquire(owner, 141).refusal == TxCoordinator::Refusal::Recovering && stops == 2,
          "acquisition cannot bypass an expired unconfirmed tail before the timer runs");
}

void lifetimeAndIdentity()
{
    TxCoordinator::Operation stale;
    TxCoordinator::Actor staleActor;
    {
        TxCoordinator coordinator([](const auto&, auto) {});
        staleActor = coordinator.registerActor({true, 0});
        stale = coordinator.acquire(staleActor, 0).operation;
    }
    check(!stale.permitsDispatch(1), "coordinator destruction fences retained worker handles");
    TxCoordinator other([](const auto&, auto) {});
    check(other.acquire(staleActor, 1).refusal == TxCoordinator::Refusal::InvalidActor,
          "actor from destroyed or different coordinator cannot be adopted");
    const TxCoordinator::Actor actor = other.registerActor({true, 0});
    const TxCoordinator::Admission operation = other.acquire(actor, 0);
    check(operation.operation.permitsDispatch(std::numeric_limits<qint64>::max()),
          "ordinary local operation has no newly imposed timeout");
    other.reset();
    check(!operation.operation.permitsDispatch(1), "connection reset invalidates old work");
    check(!operation.operation.permitsCleanup(), "connection reset invalidates old key-ups too");
    check(other.recovering(), "reset remains fail closed until cleanup is acknowledged");
    check(other.acknowledgeStopped(operation.operation), "reset cleanup tied to exact old operation");
    const TxCoordinator::Admission next = other.acquire(actor, 2);
    check(!next.operation.sameOperation(operation.operation), "new connection uses new operation identity");
    other.emergencyStop();
    check(!next.operation.permitsDispatch(3), "emergency stop invalidates active operation");

    TxCoordinator::Operation tail;
    {
        TxCoordinator completed([](const auto&, auto) {});
        tail = completed.acquire(completed.registerActor({true, 0}), 0).operation;
        check(completed.finishLocalIntent(tail), "lifetime fixture ends local intent");
    }
    check(!tail.permitsDispatch(1) && !tail.permitsCleanup(),
          "destruction retires unconfirmed tail handles too");
}

void limitsAndThread()
{
    TxCoordinator coordinator([](const auto&, auto) {});
    check(coordinator.acquire(coordinator.registerActor({true, -1}), 0).refusal
              == TxCoordinator::Refusal::InvalidActor, "negative time policy is rejected");
    std::vector<TxCoordinator::Actor> actors;
    for (int i = 0; i < TxCoordinator::kMaximumActors; ++i) {
        actors.push_back(coordinator.registerActor({true, 0}));
    }
    check(coordinator.acquire(coordinator.registerActor({true, 0}), 0).refusal
              == TxCoordinator::Refusal::InvalidActor, "actor registry is bounded");
    coordinator.revoke(actors.front());
    const TxCoordinator::Actor replacement = coordinator.registerActor({true, 0});
    check(coordinator.acquire(replacement, 0).accepted(), "revocation frees a registration slot");
    std::unique_ptr<QThread> thread(QThread::create([&] {
        check(coordinator.acquire(replacement, 1).refusal == TxCoordinator::Refusal::WrongThread,
              "off-thread admission fails in release builds");
        coordinator.emergencyStop();
    }));
    thread->start();
    thread->wait();
    check(coordinator.acquire(replacement, 2).accepted(), "off-thread mutation did not change state");

    TxCoordinator large([](const auto&, auto) {});
    const auto bounded = large.registerActor({true, 20});
    const auto result = large.acquire(bounded, std::numeric_limits<qint64>::max() - 10);
    check(result.operation.permitsDispatch(std::numeric_limits<qint64>::max()),
          "large clock values do not overflow deadline addition");
    check(!result.operation.permitsDispatch(-1), "backwards or invalid clock fails closed");
}

void acknowledgedCallbackCannotReenter()
{
    TxCoordinator::Actor actor;
    TxCoordinator* callbackCoordinator = nullptr;
    TxCoordinator coordinator([&](const TxCoordinator::Operation& operation,
                                 TxCoordinator::StopReason) {
        check(callbackCoordinator->acknowledgeStopped(operation), "synchronous stop may acknowledge cleanup");
        check(callbackCoordinator->acquire(actor, 2).refusal == TxCoordinator::Refusal::Recovering,
              "acknowledgment cannot admit new work inside old stop handler");
    });
    callbackCoordinator = &coordinator;
    actor = coordinator.registerActor({true, 0});
    const TxCoordinator::Operation before = coordinator.acquire(actor, 0).operation;
    coordinator.reset();
    const TxCoordinator::Admission after = coordinator.acquire(actor, 3);
    check(after.accepted() && after.operation.permitsCleanup(),
          "reset cannot invalidate a reentrant new operation");
    check(!before.permitsCleanup(), "reset fences the old generation after synchronous cleanup");
}

void stopOnlyFences()
{
    TxCoordinator coordinator([](const auto&, auto) {});
    const TxCoordinator::Operation fence = coordinator.cleanupFence();
    check(fence.permitsCleanup() && !fence.permitsDispatch(0), "idle cleanup fence cannot authorize key-on");
    check(!coordinator.finishLocalIntent(fence) && !coordinator.acknowledgeStopped(fence),
          "cleanup fence grants no ownership or recovery authority");
    const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
    check(coordinator.acquire(actor, 0).accepted(), "owner starts after idle cleanup fence");
    check(!fence.permitsCleanup(), "idle queued key-up cannot affect a newer operation");
}

void terminalDispatchBarrier()
{
    TxCoordinator coordinator([](const auto&, auto) {});
    const auto actor = coordinator.registerActor({true, 0});
    const auto other = coordinator.registerActor({true, 0});
    const auto operation = coordinator.acquire(actor, 100).operation;
    QSemaphore entered;
    QSemaphore finish;
    bool accepted = false;
    std::unique_ptr<QThread> worker(QThread::create([&] {
        const auto dispatch = operation.beginDispatch(101);
        accepted = bool(dispatch);
        entered.release();
        (void)finish.tryAcquire(1, 5000);
    }));
    worker->start();
    check(entered.tryAcquire(1, 5000) && accepted && coordinator.hasInFlightDispatches(),
          "terminal writer enters a counted nonblocking dispatch guard");
    check(coordinator.cancel(actor, operation) && !operation.beginDispatch(102),
          "cancellation refuses new writes while an entered write remains accounted for");
    check(!coordinator.acknowledgeStopped(operation)
              && coordinator.acquire(other, 103).refusal == TxCoordinator::Refusal::Recovering,
          "neither stop acknowledgment nor ownership transfer can outrun an entered write");
    finish.release();
    check(worker->wait(5000) && !coordinator.hasInFlightDispatches(),
          "writer return retires its guard without waiting for the engine thread");
    check(coordinator.acknowledgeStopped(operation), "qualified acknowledgment succeeds after writes finish");
    const auto next = coordinator.acquire(other, 104).operation;
    check(!operation.beginDispatch(104, false) && next.permitsDispatch(104),
          "stale cleanup cannot enter after the next generation is admitted");
    auto current = next.beginDispatch(105);
    check(coordinator.finishLocalIntent(next)
              && coordinator.acquire(other, 106).refusal == TxCoordinator::Refusal::Recovering,
          "same-owner reengagement cannot pass a still-entered old write");
    auto moved = std::move(current);
    check(!current && moved && coordinator.hasInFlightDispatches(),
          "moving a dispatch guard transfers its single outstanding hold");
    moved = {};
    check(!coordinator.hasInFlightDispatches() && coordinator.acquire(other, 107).accepted(),
          "guard release permits fresh same-owner intent without a queued auto-start");

    TxCoordinator idle([](const auto&, auto) {});
    const auto idleActor = idle.registerActor({true, 0});
    const auto cleanup = idle.cleanupFence();
    auto cleanupWrite = cleanup.beginDispatch(0, false);
    check(cleanupWrite && !cleanup.beginDispatch(0)
              && idle.acquire(idleActor, 0).refusal == TxCoordinator::Refusal::Recovering,
          "idle cleanup is counted but conveys no key-on or admission authority");
    cleanupWrite = {};
    check(idle.acquire(idleActor, 1).accepted() && !cleanup.beginDispatch(1, false),
          "a new generation atomically excludes subsequent old idle cleanup");

    TxCoordinator::Dispatch survivingWrite;
    TxCoordinator::Operation destroyed;
    {
        TxCoordinator temporary([](const auto&, auto) {});
        const auto owner = temporary.registerActor({true, 0});
        destroyed = temporary.acquire(owner, 0).operation;
        survivingWrite = destroyed.beginDispatch(0);
    }
    check(survivingWrite && !destroyed.permitsCleanup() && !destroyed.beginDispatch(1, false),
          "a surviving writer cannot keep a destroyed coordinator open for more work");
}

void producerIntents()
{
    TxCoordinator coordinator([](const auto&, auto) {});
    const auto actor = coordinator.registerActor({true, 0});
    const auto competitor = coordinator.registerActor({true, 0});
    const auto operation = coordinator.acquire(actor, 100).operation;
    const auto first = coordinator.beginIntent(operation, {}, Activity::Mox);
    const auto second = coordinator.beginIntent(operation, {}, Activity::Cwx);
    check(first.pending() && second.pending() && !first.sameIntent(second),
          "two producers sharing an operation receive distinct intent handles");
    check(!coordinator.beginIntent(operation, first, Activity::CwKey).pending(),
          "a live handle cannot be reused for a different activity");
    check(!coordinator.beginIntent(operation, {}, static_cast<Activity>(3)).pending(),
          "a combined activity value cannot masquerade as one producer intent");
    check(coordinator.beginIntent(operation, first, Activity::Mox).sameIntent(first),
          "repeated producer admission is idempotent, not another hold");
    check(!coordinator.finishLocalIntent(operation),
          "operation completion cannot bypass outstanding producer intents");
    check(coordinator.requestIntentEnd(first) && first.permitsDispatch(101),
          "normal release retains the original producer tail until consumed");
    check(coordinator.activeActivities(operation)
              == (static_cast<unsigned>(Activity::Mox) | static_cast<unsigned>(Activity::Cwx)),
          "draining contributions remain visible to activity interlocks");
    const auto reengaged = coordinator.beginIntent(operation, first, Activity::Mox);
    check(reengaged.pending() && !reengaged.sameIntent(first),
          "reengagement during an old tail creates a distinct intent");
    check(coordinator.endIntent(first) && !coordinator.endIntent(first),
          "a captured release is consumed exactly once");
    check(!first.permitsDispatch(102) && second.permitsDispatch(102)
              && reengaged.permitsDispatch(102),
          "old release fences only its producer contribution");
    check(coordinator.endIntent(second) && !coordinator.finishLocalIntent(operation),
          "finishing a second producer cannot end the reengaged contribution");
    check(coordinator.activeActivities(operation) == static_cast<unsigned>(Activity::Mox),
          "derived activity view drops only the finished contribution's type");
    check(coordinator.endIntent(reengaged) && !coordinator.hasIntents(operation)
              && coordinator.finishLocalIntent(operation),
          "local operation completion becomes possible after every contribution ends");
    check(coordinator.acquire(competitor, 103).refusal == TxCoordinator::Refusal::Busy,
          "ending all producer intents is still not radio-idle handoff evidence");
    const auto next = coordinator.acquire(actor, 104).operation;
    const auto nextIntent = coordinator.beginIntent(next, reengaged, Activity::Mox);
    check(!coordinator.endIntent(first) && nextIntent.permitsDispatch(105),
          "an old producer callback cannot affect the next operation");
    coordinator.reset();
    check(!nextIntent.pending() && !nextIntent.permitsDispatch(106)
              && !coordinator.hasIntents(next) && coordinator.activeActivities(next) == 0,
          "session reset retires every producer contribution");
}

void intentBoundaries()
{
    TxCoordinator coordinator([](const auto&, auto) {});
    TxCoordinator other([](const auto&, auto) {});
    const auto actor = coordinator.registerActor({true, 20});
    const auto operation = coordinator.acquire(actor, 0).operation;
    const auto otherOperation = other.acquire(other.registerActor({true, 0}), 0).operation;
    const auto foreign = other.beginIntent(otherOperation, {}, Activity::Mox);
    check(!coordinator.beginIntent(otherOperation, {}, Activity::Mox).pending()
              && !coordinator.beginIntent(operation, foreign, Activity::Mox).pending()
              && !coordinator.endIntent(foreign)
              && !coordinator.requestIntentEnd(foreign),
          "foreign operations and producer handles cannot cross coordinators");
    check(!coordinator.beginIntent(coordinator.cleanupFence(), {}, Activity::Mox).pending(),
          "a cleanup-only fence cannot create a producer intent");
    std::vector<TxCoordinator::Intent> intents;
    for (int i = 0; i < TxCoordinator::kMaximumIntents; ++i) {
        intents.push_back(coordinator.beginIntent(operation, {}, Activity::Mox));
        check(intents.back().pending(), "producer intent fits within bounded registry");
    }
    check(!coordinator.beginIntent(operation, {}, Activity::Mox).pending(), "producer registry refuses overflow");
    check(coordinator.beginIntent(operation, intents.front(), Activity::Mox).sameIntent(intents.front()),
          "a repeated intent still succeeds at registry capacity");
    check(coordinator.endIntent(intents.front())
              && coordinator.beginIntent(operation, {}, Activity::Mox).pending(),
          "finished producer releases its bounded registry slot");
    const auto live = intents.back();
    bool threadChecks = false;
    std::unique_ptr<QThread> worker(QThread::create([&] {
        threadChecks = !coordinator.beginIntent(operation, {}, Activity::Mox).pending()
            && !coordinator.requestIntentEnd(live) && !coordinator.endIntent(live)
            && live.permitsDispatch(19) && !live.permitsDispatch(20);
    }));
    worker->start();
    worker->wait();
    check(threadChecks, "worker may inspect fences but cannot mutate producer ownership");
    coordinator.expire(20);
    check(!live.pending() && !coordinator.hasIntents(operation),
          "expiry retires producers without depending on their release callbacks");
    TxCoordinator::Intent orphan;
    {
        TxCoordinator shortLived([](const auto&, auto) {});
        const auto temporary = shortLived.acquire(shortLived.registerActor({true, 0}), 0).operation;
        orphan = shortLived.beginIntent(temporary, {}, Activity::Mox);
    }
    check(!orphan.pending() && !orphan.permitsDispatch(1),
          "coordinator destruction invalidates copied producer handles");
}
void producerMediaContexts()
{
    TxCoordinator coordinator([](const auto&, auto) {});
    TxCoordinator foreign([](const auto&, auto) {});
    const auto actor = coordinator.registerActor({true, 20});
    const auto producer = coordinator.registerProducer();
    const auto mic = coordinator.registerProducer(true);
    const auto microphone = coordinator.mediaContext(mic);
    check(!TxCoordinator::Context{}.permitsDispatch(0)
              && !coordinator.mediaContext(producer).permitsDispatch(0),
          "ordinary media needs an original admitted operation");
    check(microphone.permitsDispatch(0) && !microphone.permitsDispatch(-1),
          "explicit continuous microphone media permits RX, not invalid clock values");
    auto micWrite = microphone.beginDispatch(0);
    const auto first = coordinator.acquire(actor, 0).operation;
    check(first.permitsDispatch(0) && bool(micWrite),
          "an entered RX microphone write never refuses a fresh PTT intent");
    const auto media = coordinator.mediaContext(producer, first);
    check(media.permitsDispatch(19) && !media.permitsDispatch(20),
          "media retains its admitted operation's exact deadline");
    check(!foreign.mediaContext(producer, first).permitsDispatch(1)
              && !coordinator.mediaContext(foreign.registerProducer(), first).permitsDispatch(1),
          "producer and operation provenance cannot cross coordinators");
    check(media.sameContext(coordinator.mediaContext(producer, first))
              && !media.sameContext(coordinator.mediaContext(coordinator.registerProducer(), first)),
          "contexts distinguish producers even under the shared actor");
    const auto copy = producer;
    copy.invalidate();
    check(!producer.valid() && !media.permitsDispatch(1) && !media.beginDispatch(1),
          "producer teardown fences copies and queued media without keying or stopping");
    check(first.permitsDispatch(1), "producer invalidation does not cancel the shared operation");
    coordinator.reset();
    check(!microphone.permitsDispatch(1) && !microphone.beginDispatch(1)
              && !coordinator.mediaContext(mic).permitsDispatch(1),
          "connection reset fences queued continuous media and recovery cannot remint it");
    check(!coordinator.acknowledgeStopped(first),
          "transport teardown accounts for an already entered continuous write");
    micWrite = {};
    check(coordinator.acknowledgeStopped(first), "teardown may complete after the continuous writer returns");
    const auto next = coordinator.acquire(actor, 2).operation;
    check(next.permitsDispatch(2) && !microphone.permitsDispatch(2)
              && coordinator.mediaContext(mic).permitsDispatch(2),
          "a new session never adopts old microphone media");
    bool workerRefused = false;
    std::unique_ptr<QThread> worker(QThread::create([&] {
        workerRefused = !coordinator.registerProducer().valid()
            && !coordinator.mediaContext(mic).permitsDispatch(2);
    }));
    worker->start();
    worker->wait();
    check(workerRefused, "worker threads cannot mint media authority");
    TxCoordinator::Context orphan;
    TxCoordinator::Dispatch entered;
    {
        TxCoordinator temporary([](const auto&, auto) {});
        orphan = temporary.mediaContext(temporary.registerProducer(true));
        entered = orphan.beginDispatch(0);
    }
    check(entered && !orphan.permitsDispatch(1) && !orphan.beginDispatch(1),
          "a surviving entered guard cannot keep a destroyed coordinator's media alive");
    std::vector<TxCoordinator::Producer> registrations;
    for (int i = 0; i < TxCoordinator::kMaximumProducers - 1; ++i) {
        registrations.push_back(coordinator.registerProducer());
    }
    check(registrations.back().valid() && !coordinator.registerProducer().valid(),
          "producer registration is bounded even when sessions retain handles");
    registrations.front().invalidate();
    check(coordinator.registerProducer().valid(), "invalidated producer slots can be reclaimed");
}
void capturedProducerRequests()
{
    TxCoordinator coordinator([](const auto&, auto) {});
    TxCoordinator foreign([](const auto&, auto) {});
    const auto actor = coordinator.registerActor({true, 0});
    const auto first = coordinator.registerProducer();
    const auto second = coordinator.registerProducer();
    const auto a = first.request();
    const auto b = second.request();
    const auto operation = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    const auto aIntent = coordinator.beginRequest(a, operation, TxCoordinator::Activity::Mox);
    const auto bIntent = coordinator.beginRequest(b, operation, TxCoordinator::Activity::Mox);
    const auto aOperation = coordinator.requestOperation(a);
    const auto bOperation = coordinator.requestOperation(b);
    const auto aMedia = coordinator.mediaContext(a);
    check(aIntent.pending() && bIntent.pending()
              && coordinator.beginRequest(a, operation, TxCoordinator::Activity::Mox).sameIntent(aIntent),
          "each captured request binds exactly once; repeated on reuses only its own intent");
    check(aOperation.sameOperation(bOperation) && !aOperation.sameAuthority(bOperation),
          "compatible producer authority is distinct under the same desktop operation");
    check(!foreign.acceptsRequest(a) && !foreign.requestIntent(a).pending(),
          "requests cannot cross coordinator identities");
    check(coordinator.closeRequest(a).sameIntent(aIntent) && !a.valid()
              && !coordinator.beginRequest(a, operation, TxCoordinator::Activity::Mox).pending()
              && aOperation.permitsDispatch(TxCoordinator::monotonicMs()),
          "normal release closes new admission while retaining its original queued tail");
    check(coordinator.hasOtherIntents(operation, aIntent) && coordinator.endIntent(aIntent)
              && !aOperation.permitsDispatch(TxCoordinator::monotonicMs())
              && !aMedia.permitsDispatch(TxCoordinator::monotonicMs())
              && bOperation.permitsDispatch(TxCoordinator::monotonicMs()),
          "one producer's consumed release fences its own commands and media only");
    const auto fresh = first.request();
    const auto freshIntent = coordinator.beginRequest(fresh, operation, TxCoordinator::Activity::Mox);
    check(freshIntent.pending() && !aMedia.sameContext(coordinator.mediaContext(fresh)),
          "fresh request from the same producer cannot inherit a partial media buffer");
    first.invalidate();
    check(!coordinator.requestOperation(fresh).permitsDispatch(TxCoordinator::monotonicMs())
              && bOperation.permitsDispatch(TxCoordinator::monotonicMs())
              && coordinator.closeRequest(fresh).sameIntent(freshIntent),
          "producer destruction fences key-on immediately but permits cleanup of its admitted intent");
    (void)coordinator.endIntent(freshIntent);
    const auto beforeReset = second.request();
    const auto reversed = second.request();
    (void)coordinator.closeRequest(reversed);
    check(!coordinator.beginRequest(reversed, operation, TxCoordinator::Activity::Mox).pending(),
          "an off consumed before its queued on cannot later acquire transmit");
    coordinator.reset();
    check(!beforeReset.valid() && !b.valid() && coordinator.acknowledgeStopped(operation),
          "connection reset fences both admitted and not-yet-admitted input requests");
    const auto next = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
    check(!coordinator.beginRequest(beforeReset, next, TxCoordinator::Activity::Mox).pending(),
          "a queued prior-session request never adopts the new connection's operation");
    TxCoordinator::Request workerRequest;
    bool workerCannotAdmit = false;
    std::unique_ptr<QThread> worker(QThread::create([&] {
        workerRequest = second.request();
        workerCannotAdmit = !coordinator.beginRequest(workerRequest, next, TxCoordinator::Activity::Mox).pending();
    }));
    worker->start();
    worker->wait();
    check(workerCannotAdmit && coordinator.beginRequest(workerRequest, next, TxCoordinator::Activity::Mox).pending(),
          "worker may capture input identity, but admission remains on the engine owner thread");
    TxCoordinator bounded([](const auto&, auto) {});
    const auto boundedProducer = bounded.registerProducer();
    std::vector<TxCoordinator::Request> requests;
    for (int i = 0; i < TxCoordinator::kMaximumRequests; ++i) {
        requests.push_back(boundedProducer.request());
    }
    check(requests.back().valid() && !boundedProducer.request().valid(),
          "pending input request handles have a coordinator-wide bound");
    requests.pop_back();
    check(boundedProducer.request().valid(), "request capacity is reclaimed after its final queued copy dies");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ownershipAndRecovery();
    expiryAndRevocation();
    unconfirmedCompletion();
    unconfirmedStopSources();
    unconfirmedDeadline();
    lifetimeAndIdentity();
    limitsAndThread();
    acknowledgedCallbackCannotReenter();
    stopOnlyFences();
    producerIntents();
    terminalDispatchBarrier();
    intentBoundaries();
    producerMediaContexts();
    capturedProducerRequests();
    return failures ? 1 : 0;
}
