#include "core/TxCoordinator.h"

#include <QCoreApplication>
#include <QThread>

#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

using AetherSDR::TxCoordinator;

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
    check(!coordinator.complete({}), "invalid handle cannot finish another operation");
    check(coordinator.cancel(owner, first.operation) && stops == 1,
          "owner cancellation dispatches one stop");
    check(!coordinator.cancel(owner, first.operation) && stops == 1,
          "duplicate cancellation is inert");
    check(!coordinator.acknowledgeStopped({}), "unrelated stop acknowledgment cannot clear recovery");
    check(coordinator.acknowledgeStopped(stopped), "matching qualified stop acknowledgment clears recovery");
    const TxCoordinator::Admission next = coordinator.acquire(competitor, 101);
    check(next.accepted(), "next actor can acquire after recovery");
    check(!coordinator.complete(first.operation), "late old completion cannot stop new owner");
    check(!coordinator.acknowledgeStopped(stopped), "late old acknowledgment is inert");
    check(next.operation.permitsDispatch(102), "new owner survives old callbacks");
    check(coordinator.complete(next.operation), "already-stopped operation completes normally");
    check(!next.operation.permitsDispatch(103), "normal completion fences queued work");
    check(next.operation.permitsCleanup(), "normal completion retains queued key-up cleanup");
    check(coordinator.acquire(owner, 104).accepted(), "new owner starts after normal completion");
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
    check(!coordinator.complete(fence) && !coordinator.acknowledgeStopped(fence),
          "cleanup fence grants no ownership or recovery authority");
    const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
    check(coordinator.acquire(actor, 0).accepted(), "owner starts after idle cleanup fence");
    check(!fence.permitsCleanup(), "idle queued key-up cannot affect a newer operation");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ownershipAndRecovery();
    expiryAndRevocation();
    lifetimeAndIdentity();
    limitsAndThread();
    acknowledgedCallbackCannotReenter();
    stopOnlyFences();
    return failures ? 1 : 0;
}
