#include "core/control/ControlInputPump.h"
#include "core/TxGrantManager.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <memory>
#include <thread>

using namespace AetherSDR;
using namespace AetherSDR::control;

namespace {
int failures = 0;
constexpr unsigned kMox = static_cast<unsigned>(TxCoordinator::Activity::Mox);

void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}

QByteArray frame(const QString& method, const QString& session = {}, const QJsonObject& params = {})
{
    QJsonObject request{{"v", 1}, {"id", "request"}, {"method", method}, {"params", params}};
    if (!session.isEmpty()) { request.insert("sessionId", session); }
    return QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
}

struct Harness {
    ControlResourceStore store;
    ControlService service{&store};
    int stops{0};
    TxCoordinator coordinator{[this](const auto&, auto) { ++stops; }};
    // Injected capability qualifies only the coordinator state machine. No
    // backend, socket, transport peer, radio command or RF exists in this test.
    TxGrantManager grants{coordinator, [] { return kMox; }};
    TxGrantManager::Client client{grants.registerClient(QStringLiteral("verified-client"))};
    TxGrantManager::Issuance grant{grants.issue(client, {10000, 20000, 20000, kMox})};
    TxGrantManager::Admission operation{grants.acquire(client, grant.grant, 1, TxCoordinator::Activity::Mox)};
    std::unique_ptr<ControlSession> session{std::make_unique<ControlSession>(
        &store, 4096, SessionAuthorization::ObserverController, nullptr, [] { return qint64{0}; })};
    QByteArray incoming;
    QList<QJsonObject> replies;
    qint64 maximumRead{0};
    int closes{0};
    bool writeFailure{false};
    bool fencedAtClose{false};
    bool fencedAtTerminalReply{false};
    bool abortClose{false};
    std::function<void()> onWrite;
    std::unique_ptr<ControlInputPump> pump{std::make_unique<ControlInputPump>(service, *session,
        [this](qint64 maximum) {
            maximumRead = std::max(maximumRead, maximum);
            const QByteArray bytes = incoming.left(maximum);
            incoming.remove(0, bytes.size());
            return bytes;
        }, [this](const QJsonObject& reply) {
            replies.append(reply);
            if (session && session->isRevoked()) {
                fencedAtTerminalReply = !operation.operation.permitsDispatch(coordinator.currentTimeMs());
            }
            if (onWrite) { onWrite(); }
            return !writeFailure;
        }, [this](bool abort) {
            ++closes;
            abortClose = abort;
            fencedAtClose = !operation.operation.permitsDispatch(coordinator.currentTimeMs());
        })};

    Harness()
    {
        check(operation.accepted() && session->bindAuthorityLifetime(&grants, [this] {
            grants.disconnectClient(client);
        }), "trusted session lifetime binds the existing independent actor");
    }

    void hello()
    {
        incoming += frame(QStringLiteral("hello"), {}, {{"versions", QJsonArray{1}}});
        pump->readAvailable();
        check(session->canControl(), "current transport negotiates normally");
    }

    void capabilities(int count)
    {
        for (int i = 0; i < count; ++i) {
            incoming += frame(QStringLiteral("capabilities.get"), session->sessionId());
        }
    }
};

void credentialRetirementFencesWorkersBeforeCallbacks()
{
    ControlResourceStore store;
    ControlService service(&store);
    TxCoordinator::Operation operation;
    TxCoordinator::Context media;
    bool fencedBeforeSessionCallback = false;
    ControlCredentials credentials;
    const auto record = ControlCredentials::generate(ControlCredentials::Role::Client);
    check(credentials.replace({record}) && service.bindCredentials(&credentials),
          "trusted credential verifier binds before transport starts");
    // Deliberately precede the session's revocation listener. Captured worker
    // authority must already be dead even if an earlier observer re-enters.
    QObject::connect(&credentials, &ControlCredentials::changed, &store, [&] {
        std::thread worker([&] {
            const qint64 now = TxCoordinator::monotonicMs();
            fencedBeforeSessionCallback = !operation.permitsDispatch(now)
                && !media.permitsDispatch(now) && !operation.beginDispatch(now);
        });
        worker.join();
    });
    ControlSession session(&store, 4096, SessionAuthorization::ObserverController);
    const QJsonObject auth{{"scheme", "bearer"}, {"token", QString::fromLatin1(record.secret.toHex())}};
    const ServiceReply negotiated = service.handle(frame("hello", {}, {{"versions", QJsonArray{1}}, {"auth", auth}}), &session);
    check(negotiated.message.contains("result") && session.principalId() == record.id,
          "real hello verifier supplies the principal, not the client's label");
    int stops = 0;
    TxCoordinator coordinator([&](const auto&, auto) { ++stops; });
    TxGrantManager grants(coordinator, [] { return kMox; });
    const auto client = grants.registerClient(session.principalId(), session.credentialFence());
    const auto issued = grants.issue(client, {10000, 20000, 20000, kMox});
    const auto admitted = grants.acquire(client, issued.grant, 1, TxCoordinator::Activity::Mox);
    operation = admitted.operation;
    media = coordinator.mediaContext(admitted.input);
    check(admitted.accepted() && media.permitsDispatch(TxCoordinator::monotonicMs())
              && session.bindAuthorityLifetime(&grants, [&] { grants.disconnectClient(client); }),
          "explicit injected grant propagates the verified credential to command/media authority");
    check(credentials.revoke(record.id) && fencedBeforeSessionCallback && stops == 1
              && session.isRevoked() && grants.clientCount() == 0 && grants.grantCount() == 0,
          "credential revocation fences terminal workers before callbacks, then retires the grant synchronously");
    check(!grants.issue(client, {10000, 20000, 20000, kMox}).accepted(),
          "revoked credential cannot mint a replacement grant");
}

void boundedTurnsAndDisconnect()
{
    Harness h;
    h.hello();
    h.capabilities(ControlInputPump::kFramesPerTurn * 3);
    const qsizetype before = h.replies.size();
    h.pump->readAvailable();
    check(h.replies.size() - before == ControlInputPump::kFramesPerTurn,
          "one readable callback dispatches only its bounded frame allowance");
    h.pump->readAvailable();
    check(h.replies.size() - before == ControlInputPump::kFramesPerTurn,
          "a second readable callback cannot bypass the already scheduled yield");
    h.pump->finish();
    check(!h.operation.operation.permitsDispatch(h.coordinator.currentTimeMs()) && h.stops == 1,
          "disconnect fences retained TX before deferred client destruction");
    QCoreApplication::processEvents();
    check(h.replies.size() - before == ControlInputPump::kFramesPerTurn
          && h.session->isRevoked(), "queued continuation cannot dispatch after disconnect");
}

void timerGetsATurn()
{
    Harness h;
    h.hello();
    h.capabilities(ControlInputPump::kFramesPerTurn * 3);
    qsizetype atTimer = -1;
    QTimer::singleShot(0, [&] { atTimer = h.replies.size(); });
    h.pump->readAvailable();
    QElapsedTimer watchdog;
    watchdog.start();
    while (atTimer < 0 && watchdog.elapsed() < 2000) {
        QCoreApplication::processEvents();
    }
    check(atTimer >= 1 && atTimer < 1 + ControlInputPump::kFramesPerTurn * 3,
          "engine timer executes before a pipelined client burst fully drains");
}

void fragmentedAndLargeFrames()
{
    Harness fragmented;
    const QByteArray hello = frame(QStringLiteral("hello"), {}, {{"versions", QJsonArray{1}}});
    fragmented.incoming = hello.left(hello.size() / 2);
    fragmented.pump->readAvailable();
    check(fragmented.replies.isEmpty() && !fragmented.session->isNegotiated(),
          "incomplete input is retained without dispatching a partial request");
    fragmented.incoming = hello.mid(hello.size() / 2);
    fragmented.pump->readAvailable();
    check(fragmented.replies.size() == 1 && fragmented.session->canControl(),
          "the next readable callback completes the original fragmented request once");

    Harness large;
    large.hello();
    QByteArray request = frame(QStringLiteral("capabilities.get"), large.session->sessionId());
    request.chop(1);
    // Whitespace is legal JSON and lets this test reach the frame-size bound
    // without introducing unknown parameters or exceeding a value's own limit.
    large.incoming = QByteArray(ProtocolLimits::kMaxMessageBytes - request.size(), ' ')
        + request + '\n';
    large.pump->readAvailable();
    check(large.replies.size() == 1
          && large.incoming.size() == ProtocolLimits::kMaxMessageBytes + 1
              - ControlInputPump::kReadBytesPerTurn,
          "one turn reads only its byte budget even for a valid maximum-size frame");
    QElapsedTimer watchdog;
    watchdog.start();
    while (large.replies.size() == 1 && watchdog.elapsed() < 2000) {
        QCoreApplication::processEvents();
    }
    check(large.replies.size() == 2 && large.replies.last().contains("result")
          && !large.pump->isFinished() && large.incoming.isEmpty(),
          "scheduled continuations accept a valid maximum-size frame without truncation");
}

void terminalFrames()
{
    Harness malformed;
    malformed.incoming = "not-json\n";
    malformed.pump->readAvailable();
    check(malformed.session->isRevoked() && malformed.fencedAtTerminalReply
          && malformed.fencedAtClose && !malformed.abortClose && malformed.closes == 1,
          "fatal handshake input retires authority before final error delivery");

    Harness oversized;
    oversized.hello();
    oversized.incoming = QByteArray(ProtocolLimits::kMaxMessageBytes + 1, 'x');
    oversized.pump->readAvailable();
    check(oversized.maximumRead <= ControlInputPump::kReadBytesPerTurn,
          "each transport read is bounded independently of maximum frame size");
    QElapsedTimer watchdog;
    watchdog.start();
    while (!oversized.pump->isFinished() && watchdog.elapsed() < 2000) {
        QCoreApplication::processEvents();
    }
    check(oversized.fencedAtTerminalReply && oversized.fencedAtClose && oversized.closes == 1,
          "oversized incomplete frame closes with synchronous authority retirement");

    Harness budget;
    budget.hello();
    budget.capabilities(ControlSession::kRequestBurst + 1);
    budget.pump->readAvailable();
    watchdog.restart();
    while (!budget.pump->isFinished() && watchdog.elapsed() < 2000) {
        QCoreApplication::processEvents();
    }
    check(budget.closes == 1 && budget.fencedAtTerminalReply && budget.fencedAtClose
          && budget.replies.last().value("error").toObject().value("code") == "transport.limit_exceeded",
          "request-budget closure retires authority before its terminal reply");

    Harness recoverable;
    recoverable.hello();
    recoverable.incoming = "not-json\n";
    recoverable.pump->readAvailable();
    check(recoverable.closes == 0 && !recoverable.session->isRevoked()
          && recoverable.operation.operation.permitsDispatch(recoverable.coordinator.currentTimeMs()),
          "recoverable post-handshake parse refusal does not masquerade as disconnect");
}

void outputFailuresAndLifetime()
{
    Harness failedReply;
    failedReply.writeFailure = true;
    failedReply.incoming = frame(QStringLiteral("hello"), {}, {{"versions", QJsonArray{1}}});
    failedReply.pump->readAvailable();
    check(failedReply.fencedAtClose && failedReply.abortClose && failedReply.closes == 1,
          "reply write failure fences authority before transport abort");

    Harness failedEvent;
    failedEvent.hello();
    QObject transport;
    bool fencedAtAbort = false;
    failedEvent.session->bindOutputTransport(&transport, [](const QByteArray&) { return false; }, [&] {
        fencedAtAbort = !failedEvent.operation.operation.permitsDispatch(failedEvent.coordinator.currentTimeMs());
    });
    QJsonObject baseline;
    check(!failedEvent.session->subscribe({{QStringLiteral("server"), {}, {}}}, &baseline),
          "event sink subscribes without a socket");
    failedEvent.store.upsert({QStringLiteral("server"), {}, {}}, {{"health", "ok"}});
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(fencedAtAbort && failedEvent.session->isRevoked(),
          "event write failure retires the same client before abort callback");

    Harness destroyed;
    destroyed.hello();
    destroyed.session.reset();
    check(!destroyed.operation.operation.permitsDispatch(destroyed.coordinator.currentTimeMs()),
          "session destruction retires authority even without a socket-disconnected signal");
    destroyed.pump->readAvailable();

    Harness lostTransport;
    lostTransport.hello();
    auto context = std::make_unique<QObject>();
    lostTransport.session->bindOutputTransport(context.get(), [](const QByteArray&) { return true; }, [] {});
    context.reset();
    check(!lostTransport.operation.operation.permitsDispatch(lostTransport.coordinator.currentTimeMs()),
          "destroyed transport context cannot leave a granted session armed");
}

void terminalOverflowAndScopedCleanup()
{
    Harness h;
    ControlSession tiny(&h.store, 1, SessionAuthorization::Observer);
    check(tiny.bindAuthorityLifetime(&h.grants, [&] { h.grants.disconnectClient(h.client); }),
          "overflow fixture binds authority");
    (void)h.service.handle(frame(QStringLiteral("hello"), {}, {{"versions", QJsonArray{1}}}).trimmed(), &tiny);
    QJsonObject baseline;
    check(!tiny.subscribe({{QStringLiteral("server"), {}, {}}}, &baseline), "tiny queue subscribes");
    h.store.upsert({QStringLiteral("server"), {}, {}}, {{"health", "changed"}});
    check(tiny.isRevoked() && !h.operation.operation.permitsDispatch(h.coordinator.currentTimeMs()),
          "undeliverable resync retires authority before any event-loop wake");

    Harness owner;
    const auto other = owner.grants.registerClient(QStringLiteral("other"));
    const auto grant = owner.grants.issue(other, {10000, 20000, 20000, kMox});
    ControlSession observer(&owner.store, 4096, SessionAuthorization::Observer);
    check(grant.accepted() && observer.bindAuthorityLifetime(&owner.grants, [&] {
        owner.grants.disconnectClient(other);
    }), "another session has its own lifetime binding");
    observer.endAuthorization();
    check(owner.operation.operation.permitsDispatch(owner.coordinator.currentTimeMs()) && owner.stops == 0,
          "another client's terminal event does not stop the current owner");
}

void reentrantAndLateBinding()
{
    Harness h;
    h.onWrite = [&] { h.pump->finish(); };
    h.incoming = frame(QStringLiteral("hello"), {}, {{"versions", QJsonArray{1}}});
    h.incoming += h.incoming;
    h.pump->readAvailable();
    check(h.replies.size() == 1 && !h.operation.operation.permitsDispatch(h.coordinator.currentTimeMs()),
          "synchronous disconnect during output prevents the next input dispatch");

    Harness destroyed;
    destroyed.onWrite = [&] { destroyed.pump.reset(); };
    destroyed.incoming = frame(QStringLiteral("hello"), {}, {{"versions", QJsonArray{1}}});
    destroyed.pump->readAvailable();
    check(!destroyed.pump && destroyed.stops == 1,
          "destroying input pump during output safely retires its authority");

    ControlResourceStore store;
    ControlSession ended(&store, 4096, SessionAuthorization::Observer);
    QObject owner;
    int calls = 0;
    ended.endAuthorization();
    check(ended.bindAuthorityLifetime(&owner, [&] { ++calls; }) && calls == 1,
          "late lifetime binding cannot miss prior termination");
    check(!ended.bindAuthorityLifetime(&owner, [&] { ++calls; }), "authority lifetime cannot be rebound");
    ended.endAuthorization();
    ended.revokeAuthorization();
    check(calls == 1, "repeated terminal paths retire the lifetime once");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    credentialRetirementFencesWorkersBeforeCallbacks();
    boundedTurnsAndDisconnect();
    timerGetsATurn();
    fragmentedAndLargeFrames();
    terminalFrames();
    outputFailuresAndLifetime();
    terminalOverflowAndScopedCleanup();
    reentrantAndLateBinding();
    return failures == 0 ? 0 : 1;
}
