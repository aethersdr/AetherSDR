#include "core/TxGrantManager.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <limits>
#include <memory>

using AetherSDR::TxCoordinator;
using AetherSDR::TxGrantManager;
using Activity = TxCoordinator::Activity;
constexpr unsigned kMox = static_cast<unsigned>(Activity::Mox);

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}

struct Harness {
    std::atomic<qint64> now{100};
    int stops{0};
    TxCoordinator::Operation stopped;
    TxCoordinator::StopRequest confirmation;
    TxCoordinator coordinator{[this](const auto& operation, auto) {
        ++stops;
        stopped = operation;
        confirmation = coordinator.requestStopConfirmation(operation);
    }, [this] { return now.load(); }};
    // Injected qualification for state-machine coverage ONLY. No radio/backend
    // or fake firmware is used and no production support is claimed here.
    TxGrantManager grants{coordinator, [] { return kMox; }};
    TxGrantManager::Client a{grants.registerClient(QStringLiteral("verified-A"))};
    TxGrantManager::Client b{grants.registerClient(QStringLiteral("verified-B"))};
    const TxGrantManager::Policy policy{1000, 10000, 2000, kMox};
};

void handoffAndReplay()
{
    Harness h;
    const auto ga = h.grants.issue(h.a, h.policy);
    const auto gb = h.grants.issue(h.b, h.policy);
    check(ga.accepted() && gb.accepted(), "trusted clients receive distinct bounded grants");
    const auto a = h.grants.acquire(h.a, ga.grant, 1, Activity::Mox);
    check(a.accepted() && a.input.valid() && a.operation.permitsDispatch(h.now),
          "A acquires an actor-bound explicit intent");
    check(h.grants.acquire(h.b, gb.grant, 1, Activity::Mox).refusal == TxCoordinator::Refusal::Busy,
          "B is refused while A owns TX");
    check(!h.grants.release(h.b, ga.grant, a.operation) && !h.grants.cancel(h.b, ga.grant, a.operation),
          "another client cannot release or cancel A's handle");
    check(!h.grants.acquire(h.b, ga.grant, 2, Activity::Mox).accepted(),
          "another client cannot acquire using A's grant");
    check(h.grants.release(h.a, ga.grant, a.operation) && h.stops == 1,
          "A's release requests stop exactly once");
    check(!a.operation.permitsDispatch(h.now) && h.confirmation.valid(),
          "release fences old work before stop confirmation");
    check(h.grants.acquire(h.b, gb.grant, 2, Activity::Mox).refusal == TxCoordinator::Refusal::Recovering,
          "B remains refused after unkey request but before qualified evidence");
    check(h.coordinator.confirmStopped(h.confirmation), "injected qualified evidence releases ownership");
    check(!h.coordinator.confirmStopped(h.confirmation), "duplicate confirmation is inert");
    check(h.grants.acquire(h.b, gb.grant, 1, Activity::Mox).error == TxGrantManager::Error::Replay
          && h.grants.acquire(h.b, gb.grant, 2, Activity::Mox).error == TxGrantManager::Error::Replay,
          "busy/recovering requests cannot become delayed acquisition on retry");
    const auto b = h.grants.acquire(h.b, gb.grant, 3, Activity::Mox);
    check(b.accepted(), "fresh B intent acquires after qualified handoff");
    check(!h.grants.release(h.a, ga.grant, a.operation) && b.operation.permitsDispatch(h.now),
          "late A release cannot unkey B");
    h.grants.disconnectClient(h.a);
    check(b.operation.permitsDispatch(h.now), "A disconnect is owner-scoped");
    h.grants.disconnectClient(h.b);
    check(!b.operation.permitsDispatch(h.now) && h.stops == 2,
          "B disconnect fences its operation synchronously");
}

void absoluteAndOperationDeadlines()
{
    Harness h;
    const auto issued = h.grants.issue(h.a, {20, 50, 50, kMox});
    const auto a = h.grants.acquire(h.a, issued.grant, 1, Activity::Mox);
    h.now = 119;
    check(h.grants.keepAlive(h.a, issued.grant), "keepalive before operation deadline accepted");
    h.now = 120;
    check(!a.operation.permitsDispatch(h.now), "worker fences exact operation deadline before timer");
    h.grants.processDeadlines();
    check(h.stops == 1 && h.coordinator.confirmStopped(h.confirmation),
          "operation timeout stops and remains recoverable");
    const auto next = h.grants.acquire(h.a, issued.grant, 2, Activity::Mox);
    check(next.accepted(), "new explicit operation can use remaining grant lifetime");
    check(!h.grants.release(h.a, issued.grant, a.operation)
          && !h.grants.cancel(h.a, issued.grant, a.operation)
          && next.operation.permitsDispatch(h.now),
          "late same-client stop cannot cancel its newer operation under the same grant");
    h.now = 130;
    check(h.grants.release(h.a, issued.grant, next.operation), "burst completes inside authorization lifetime");
    check(h.coordinator.confirmStopped(h.confirmation), "burst completion acknowledged");
    h.now = 135;
    const auto third = h.grants.acquire(h.a, issued.grant, 3, Activity::Mox);
    h.now = 149;
    check(third.accepted() && third.operation.permitsDispatch(h.now), "later burst remains bounded");
    check(h.grants.keepAlive(h.a, issued.grant), "healthy transport does not imply grant renewal");
    h.now = 150;
    check(!third.operation.permitsDispatch(h.now), "absolute deadline fences burst before its own duration renews");
    h.grants.processDeadlines();
    check(h.grants.grantCount() == 0 && !h.grants.keepAlive(h.a, issued.grant),
          "expired grant cannot be revived by keepalive");
    check(!h.grants.acquire(h.a, issued.grant, 4, Activity::Mox).accepted(),
          "fresh serial does not renew expired operator authorization");

    Harness idle;
    const auto idleGrant = idle.grants.issue(idle.a, {20, 30, 40, kMox});
    idle.now = 130;
    idle.grants.processDeadlines();
    check(idleGrant.accepted() && idle.grants.grantCount() == 0 && idle.stops == 0,
          "idle authorization expires without an operation or stop callback");

    // Isolate the immutable actor deadline from the manager's independent
    // keepalive fence, so removing either guard has its own failing assertion.
    std::atomic<qint64> now{100};
    TxCoordinator coordinator([](const auto&, auto) {}, [&] { return now.load(); });
    const auto actor = coordinator.registerActor({true, 1000, 150, kMox, true});
    const auto producer = coordinator.registerProducer(actor);
    const auto operation = coordinator.acquire(actor, now).operation;
    const auto input = producer.request();
    const auto intent = coordinator.beginRequest(input, operation, Activity::Mox);
    now = 149;
    check(intent.pending() && operation.permitsDispatch(now),
          "actor deadline permits work strictly before expiration");
    now = 150;
    check(!operation.permitsDispatch(now) && !input.valid(),
          "absolute actor deadline alone fences worker dispatch and retained input");
}

void livenessAndRadioLifetime()
{
    Harness h;
    const auto ga = h.grants.issue(h.a, {1000, 10000, 20, kMox});
    const auto a = h.grants.acquire(h.a, ga.grant, 1, Activity::Mox);
    h.now = 120;
    check(!a.operation.permitsDispatch(h.now), "worker enforces keepalive deadline even if engine timer is late");
    check(!h.grants.keepAlive(h.a, ga.grant) && h.stops == 1,
          "late keepalive cancels instead of resurrecting expired liveness");
    check(h.coordinator.confirmStopped(h.confirmation), "liveness stop uses matching evidence");
    const auto renewed = h.grants.issue(h.a, h.policy);
    check(renewed.accepted(), "explicit trusted issuance creates a new grant epoch");
    check(!h.grants.acquire(h.a, ga.grant, 2, Activity::Mox).accepted(),
          "renewal cannot restore an old grant handle");
    const auto active = h.grants.acquire(h.a, renewed.grant, 1, Activity::Mox);
    h.coordinator.reset();
    check(!active.input.valid() && !active.operation.permitsDispatch(h.now),
          "radio reset independently invalidates bound actor and captured request");
    h.grants.processDeadlines();
    check(h.grants.grantCount() == 0, "idle/current manager grants retire after coordinator reset");
    check(!h.coordinator.confirmStopped(h.confirmation), "readback from old radio cannot acknowledge teardown");
    check(h.coordinator.acknowledgeStopped(h.stopped), "qualified transport teardown uses distinct recovery path");
    h.grants.disconnectClient(h.a);
    const auto reconnected = h.grants.registerClient(QStringLiteral("verified-A"));
    check(!h.grants.acquire(reconnected, renewed.grant, 2, Activity::Mox).accepted(),
          "same authenticated principal on a new connection inherits no grant");
}

void actorBindingAndMicrophone()
{
    std::atomic<qint64> now{100};
    TxCoordinator coordinator([](const auto&, auto) {}, [&] { return now.load(); });
    const auto desktop = coordinator.registerActor({true, 0});
    const auto independent = coordinator.registerActor({true, 100, 1000, kMox, true});
    const auto other = coordinator.registerActor({true, 100, 1000, kMox, true});
    const auto producer = coordinator.registerProducer(independent);
    const auto wrong = coordinator.registerProducer(other);
    const auto legacy = coordinator.registerProducer();
    const auto microphone = coordinator.registerProducer(desktop, true);
    const auto media = coordinator.mediaContext(microphone);
    check(!coordinator.registerProducer(independent, true).valid(),
          "independent actor cannot request compatibility continuous microphone privilege");
    auto writer = media.beginDispatch(now);
    check(bool(writer) && coordinator.acquire(independent, now).refusal == TxCoordinator::Refusal::Recovering,
          "entered desktop microphone writer prevents independent ownership admission");
    writer = {};
    const auto op = coordinator.acquire(independent, now).operation;
    check(!media.permitsDispatch(now) && !bool(media.beginDispatch(now)),
          "captured desktop microphone cannot feed independent operation");
    check(!coordinator.beginRequest(wrong.request(), op, Activity::Mox).pending()
          && !coordinator.beginRequest(legacy.request(), op, Activity::Mox).pending(),
          "neither foreign actor nor unbound legacy producer can borrow independent operation");
    check(!coordinator.beginIntent(op, {}, Activity::Mox).pending(),
          "legacy actorless intent API cannot bypass independent input binding");
    check(!coordinator.mediaContext(wrong, op).permitsDispatch(now)
          && !coordinator.mediaContext(legacy, op).permitsDispatch(now),
          "cross-actor audio provenance is refused");
    const auto request = producer.request();
    check(!coordinator.beginRequest(request, op, Activity::Tune).pending(),
          "PTT-only grant cannot authorize TUNE");
    const auto intent = coordinator.beginRequest(request, op, Activity::Mox);
    check(intent.pending() && coordinator.mediaContext(request).permitsDispatch(now),
          "correct actor-bound input and media work");
    (void)coordinator.endIntent(intent);
    check(coordinator.finishLocalIntent(op), "independent local completion retains ownership");
    check(coordinator.acquire(independent, now).refusal == TxCoordinator::Refusal::Recovering,
          "independent actor cannot use desktop unconfirmed reengagement exception");
    const auto stop = coordinator.requestStopConfirmation(op);
    check(coordinator.confirmStopped(stop) && media.permitsDispatch(now),
          "qualified completion restores desktop microphone compatibility");
}

void stopAttemptsAndWriters()
{
    Harness h;
    const auto ga = h.grants.issue(h.a, h.policy);
    const auto gb = h.grants.issue(h.b, h.policy);
    const auto a = h.grants.acquire(h.a, ga.grant, 1, Activity::Mox);
    check(!h.coordinator.requestStopConfirmation(a.operation).valid(),
          "active operation cannot request an idle certificate");
    auto entered = a.operation.beginDispatch(h.now);
    check(bool(entered) && h.grants.cancel(h.a, ga.grant, a.operation), "stop begins while terminal writer is entered");
    const auto old = h.confirmation;
    const auto current = h.coordinator.requestStopConfirmation(h.stopped);
    check(!old.valid() && !h.coordinator.confirmStopped(old), "superseded stop attempt is rejected");
    check(!h.coordinator.confirmStopped(current) && current.valid(),
          "qualified evidence alone cannot release an entered writer");
    entered = {};
    check(!h.coordinator.confirmStopped(old) && current.valid(),
          "superseded evidence remains rejected after all writers exit");
    check(h.coordinator.confirmStopped(current), "retained evidence releases only after writer exits");
    const auto b = h.grants.acquire(h.b, gb.grant, 1, Activity::Mox);
    check(b.accepted() && !h.coordinator.confirmStopped(current)
          && b.operation.permitsDispatch(h.now), "stale evidence cannot affect new owner");
    TxCoordinator unrelated([](const auto&, auto) {});
    check(!unrelated.confirmStopped(current), "cross-coordinator proof cannot release ownership");
}

void limitsAndReentrancy()
{
    Harness h;
    check(!h.grants.issue(h.a, {}).accepted(), "missing bounded policy is refused");
    check(h.grants.issue(h.a, {1, 1, 1, static_cast<unsigned>(Activity::Cwx)}).error
          == TxGrantManager::Error::Unsupported, "unsupported activity has no grant");
    const auto grant = h.grants.issue(h.a, h.policy);
    check(h.grants.issue(h.a, h.policy).error == TxGrantManager::Error::Conflict,
          "second grant cannot silently renew first authorization");
    bool churnAccepted = true;
    for (int i = 0; i < 100; ++i) {
        const auto client = h.grants.registerClient(QStringLiteral("churn"));
        const auto transient = h.grants.issue(client, h.policy);
        churnAccepted = churnAccepted && transient.accepted();
        h.grants.disconnectClient(client);
    }
    check(churnAccepted, "disconnect churn reclaims actor and client capacity");
    check(h.grants.clientCount() == 2 && h.grants.grantCount() == 1,
          "churn leaves no registered grants/clients behind");
    h.now = std::numeric_limits<qint64>::max();
    h.grants.processDeadlines();
    check(h.grants.issue(h.a, h.policy).error == TxGrantManager::Error::InvalidPolicy,
          "overflowing absolute deadline refused");
    check(!h.grants.revoke(grant.grant), "retired grant cannot be revoked twice");

    std::atomic<qint64> now{100};
    TxGrantManager* manager = nullptr;
    TxGrantManager::Client client;
    bool reentrant = true;
    TxCoordinator coordinator([&](const auto&, auto) {
        reentrant = manager->issue(client, {100, 200, 200, kMox}).accepted();
    }, [&] { return now.load(); });
    TxGrantManager grants(coordinator, [] { return kMox; });
    manager = &grants;
    client = grants.registerClient(QStringLiteral("verified"));
    const auto issued = grants.issue(client, {100, 200, 200, kMox});
    const auto active = grants.acquire(client, issued.grant, 1, Activity::Mox);
    grants.invalidateRadio();
    check(active.accepted() && !reentrant && !active.operation.permitsDispatch(now),
          "radio invalidation fences authority before callbacks and refuses reentrant renewal");
}

void unsupportedCapacityAndResetChurn()
{
    Harness h;
    TxGrantManager unsupported(h.coordinator, {});
    const auto unsupportedClient = unsupported.registerClient(QStringLiteral("verified"));
    check(unsupported.issue(unsupportedClient, h.policy).error == TxGrantManager::Error::Unsupported,
          "missing qualified stop path fails closed before issuing an actor");
    check(!h.grants.issue(unsupportedClient, h.policy).accepted(),
          "another manager's client handle cannot issue authority");
    std::vector<TxGrantManager::Client> clients;
    for (int i = 2; i < TxGrantManager::kMaximumClients; ++i) {
        clients.push_back(h.grants.registerClient(QStringLiteral("verified")));
    }
    const auto excess = h.grants.registerClient(QStringLiteral("verified"));
    check(!h.grants.issue(excess, h.policy).accepted() && h.grants.clientCount() == 8,
          "client registration limit is enforced");
    h.grants.disconnectClient(clients.back());
    check(h.grants.issue(h.grants.registerClient(QStringLiteral("replacement")), h.policy).accepted(),
          "disconnect reclaims a client slot");

    Harness churn;
    std::vector<TxGrantManager::Grant> retained;
    bool accepted = true;
    for (int i = 0; i < TxCoordinator::kMaximumActors + 10; ++i) {
        const auto grant = churn.grants.issue(churn.a, churn.policy);
        accepted = accepted && grant.accepted();
        retained.push_back(grant.grant);
        churn.coordinator.reset();
        churn.grants.processDeadlines();
    }
    check(accepted && churn.grants.grantCount() == 0,
          "retained stale handles cannot exhaust registrations across radio resets");

    Harness foreignThread;
    const auto grant = foreignThread.grants.issue(foreignThread.a, foreignThread.policy);
    bool refused = false;
    std::unique_ptr<QThread> worker(QThread::create([&] {
        refused = foreignThread.grants.acquire(foreignThread.a, grant.grant, 1, Activity::Mox).error
            == TxGrantManager::Error::WrongThread;
        refused = refused && !foreignThread.grants.revoke(grant.grant);
    }));
    worker->start();
    worker->wait();
    check(refused && foreignThread.grants.acquire(foreignThread.a, grant.grant, 1, Activity::Mox).accepted(),
          "off-thread authority changes fail without consuming owner-thread intent");
}

void deadlineSurvivesRescheduling()
{
    for (const bool localCompletion : {false, true}) {
        std::atomic<qint64> now{100};
        std::atomic<bool> crossDeadline{false};
        int stops = 0;
        TxCoordinator coordinator([&](const auto&, auto) { ++stops; }, [&] {
            // Make expire() see 119 and the subsequent schedule() see 120.
            return crossDeadline.load() ? now.exchange(120) : now.load();
        });
        TxGrantManager grants(coordinator, [] { return kMox; });
        const auto client = grants.registerClient(QStringLiteral("deadline"));
        const auto grant = grants.issue(client, {20, 10000, 10000, kMox});
        const auto active = grants.acquire(client, grant.grant, 1, Activity::Mox);
        check(active.accepted(), "rescheduling fixture acquires bounded ownership");
        if (localCompletion) {
            (void)coordinator.endIntent(coordinator.closeRequest(active.input));
            check(coordinator.finishLocalIntent(active.operation),
                  "local completion retains unconfirmed ownership");
            now = 110;
            check(grants.keepAlive(client, grant.grant), "keepalive reschedules unconfirmed work");
            now = 120;
        } else {
            now = 119;
            crossDeadline = true;
            grants.processDeadlines();
        }
        QElapsedTimer watchdog;
        watchdog.start();
        while (stops == 0 && watchdog.elapsed() < 1000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        check(stops == 1, localCompletion
            ? "unconfirmed operation keeps its stop deadline after keepalive"
            : "crossing expiry during scheduling cannot defer stop until keepalive");
        check(coordinator.recovering(), "deadline stop retains ownership until qualified evidence");
    }
}

void realTimerWithoutRequests()
{
    int stops = 0;
    TxCoordinator coordinator([&](const auto&, auto) { ++stops; });
    TxGrantManager grants(coordinator, [] { return kMox; });
    const auto client = grants.registerClient(QStringLiteral("verified"));
    const auto grant = grants.issue(client, {30, 5000, 5000, kMox});
    const auto active = grants.acquire(client, grant.grant, 1, Activity::Mox);
    QElapsedTimer watchdog;
    watchdog.start();
    while (stops == 0 && watchdog.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    check(active.accepted() && stops == 1 && !active.operation.permitsDispatch(TxCoordinator::monotonicMs()),
          "engine timer stops active operation with no subsequent protocol request");
}

void failedRequestCanRetry()
{
    qint64 now = 100;
    bool crossDeadline = false;
    TxCoordinator* current = nullptr;
    TxCoordinator::StopRequest stop;
    TxCoordinator coordinator([&](const auto& operation, auto) {
        stop = current->requestStopConfirmation(operation);
    }, [&] {
        // Admission uses the earlier clock snapshot; beginRequest checks again
        // after ownership exists. Expire only the operation, not the grant.
        if (crossDeadline && current->hasOwnership()) {
            now += 20;
            crossDeadline = false;
        }
        return now;
    });
    current = &coordinator;
    TxGrantManager grants(coordinator, [] { return kMox; });
    const auto client = grants.registerClient(QStringLiteral("request-retry"));
    const auto issued = grants.issue(client, {20, 10000, 5000, kMox});
    check(issued.accepted(), "request-failure fixture issues a bounded grant");
    crossDeadline = true;
    const auto failed = grants.acquire(client, issued.grant, 1, Activity::Mox);
    check(failed.error == TxGrantManager::Error::Capacity && !failed.input.valid()
          && coordinator.recovering() && grants.isLive(client, issued.grant),
          "deadline crossed at request binding cancels only the operation");
    check(coordinator.confirmStopped(stop), "failed unbound request still requires exact stop confirmation");
    check(grants.acquire(client, issued.grant, 1, Activity::Mox).error == TxGrantManager::Error::Replay,
          "failed admission still consumes its original intent serial");
    const auto retry = grants.acquire(client, issued.grant, 2, Activity::Mox);
    check(retry.accepted() && retry.input.valid() && retry.operation.permitsDispatch(now),
          "fresh intent reuses the live grant after a failed request without reissuing it");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    handoffAndReplay();
    absoluteAndOperationDeadlines();
    livenessAndRadioLifetime();
    actorBindingAndMicrophone();
    stopAttemptsAndWriters();
    limitsAndReentrancy();
    unsupportedCapacityAndResetChurn();
    deadlineSurvivesRescheduling();
    realTimerWithoutRequests();
    failedRequestCanRetry();
    return failures == 0 ? 0 : 1;
}
