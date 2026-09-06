#include "core/control/ControlService.h"

#include <QCoreApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

#include <cstdio>
#include <memory>

using namespace AetherSDR::control;

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

QString errorCode(const ServiceReply& reply)
{
    return reply.message.value(QStringLiteral("error")).toObject()
        .value(QStringLiteral("code")).toString();
}

ServiceReply invoke(ControlService& service, ControlSession& session,
                    const QString& method, const QJsonObject& params = {})
{
    QJsonObject request{{QStringLiteral("v"), 1},
                        {QStringLiteral("id"), QStringLiteral("test")},
                        {QStringLiteral("method"), method},
                        {QStringLiteral("params"), params}};
    if (method != QStringLiteral("hello")) {
        request.insert(QStringLiteral("sessionId"), session.sessionId());
    }
    return service.handle(QJsonDocument(request).toJson(QJsonDocument::Compact), &session);
}

ServiceReply hello(ControlService& service, ControlSession& session)
{
    return invoke(service, session, QStringLiteral("hello"),
                  {{QStringLiteral("versions"), QJsonArray{1}}});
}

const ResourceAddress kServer{QStringLiteral("server"), {}, {}};
const QList<ResourceSelector> kSelectors{{QStringLiteral("server"), {}, {}}};

bool testAuthenticationAndGrants()
{
    ControlResourceStore store;
    store.upsert(kServer, {{QStringLiteral("health"), QStringLiteral("ok")}});
    ControlService service(&store);
    ControlSession untrusted(&store, 4096);
    const ServiceReply denied = hello(service, untrusted);
    if (!check(errorCode(denied) == QStringLiteral("auth.required")
                   && denied.closeAfterWrite && !untrusted.isNegotiated(),
               "a session without trusted transport context must fail hello closed")) {
        return false;
    }

    ControlSession observer(&store, 4096, SessionAuthorization::Observer);
    QJsonObject directResult;
    const std::optional<ProtocolError> premature = observer.subscribe(kSelectors, &directResult);
    if (!check(premature && premature->code == QStringLiteral("session.invalid")
                   && directResult.isEmpty(),
               "transport authorization alone must not bypass negotiation")) {
        return false;
    }
    const QJsonObject welcome = hello(service, observer).message
        .value(QStringLiteral("result")).toObject();
    if (!check(welcome.value(QStringLiteral("grants")).toArray()
                   == QJsonArray{QStringLiteral("observe")}
                   && welcome.value(QStringLiteral("capabilities")).toArray().size() == 7
                   && observer.canObserve(),
               "the current-user observer must keep its seven read capabilities")) {
        return false;
    }

    ControlSession noGrants(&store, 4096, SessionAuthorization::AuthenticatedWithoutGrants);
    const QJsonObject restricted = hello(service, noGrants).message
        .value(QStringLiteral("result")).toObject();
    if (!check(noGrants.isNegotiated() && noGrants.isAuthenticated() && !noGrants.canObserve()
                   && restricted.value(QStringLiteral("grants")).toArray().isEmpty()
                   && restricted.value(QStringLiteral("capabilities")).toArray().isEmpty(),
               "authentication must not imply the observe grant or advertise its methods")) {
        return false;
    }
    const QJsonObject capabilities = invoke(service, noGrants, QStringLiteral("capabilities.get"))
        .message.value(QStringLiteral("result")).toObject();
    if (!check(capabilities == restricted,
               "capability refresh must retain the authenticated client's grant filter")) {
        return false;
    }
    // Deliberately invalid params: authorization precedes schema processing or
    // resource lookup, so denied callers learn no resource/subscription state.
    for (const QString& method : {QStringLiteral("resource.get"),
                                  QStringLiteral("resource.subscribe"),
                                  QStringLiteral("resource.unsubscribe")}) {
        const ServiceReply reply = invoke(service, noGrants, method);
        if (!check(errorCode(reply) == QStringLiteral("auth.grant_denied")
                       && !reply.closeAfterWrite,
                   "every resource method must check observe before processing params")) {
            return false;
        }
    }
    const std::optional<ProtocolError> directDenied = noGrants.subscribe(kSelectors, &directResult);
    if (!check(directDenied && directDenied->code == QStringLiteral("auth.grant_denied")
                   && directResult.isEmpty(),
               "direct subscription calls must enforce the same grant boundary")) {
        return false;
    }
    const ServiceReply read = invoke(service, observer, QStringLiteral("resource.get"),
                                    {{QStringLiteral("resource"), kServer.toJson()}});
    if (!check(read.message.value(QStringLiteral("result")).toObject()
                   .value(QStringLiteral("value")).toObject()
                   .value(QStringLiteral("health")) == QStringLiteral("ok"),
               "an authorized observer must read the real resource")) {
        return false;
    }
    for (const QString& method : {QStringLiteral("slice.setFrequency"),
                                  QStringLiteral("transmit.acquire"),
                                  QStringLiteral("transmit.setMox")}) {
        if (!check(errorCode(invoke(service, observer, method))
                       == QStringLiteral("request.unknown_method"),
                   "authorization groundwork must not expose control or TX methods")) {
            return false;
        }
    }
    // A session id identifies one connection; it is not a bearer credential.
    const QJsonObject stolenId{{QStringLiteral("v"), 1},
                               {QStringLiteral("id"), QStringLiteral("stolen")},
                               {QStringLiteral("sessionId"), observer.sessionId()},
                               {QStringLiteral("method"), QStringLiteral("resource.get")},
                               {QStringLiteral("params"), QJsonObject{
                                    {QStringLiteral("resource"), kServer.toJson()}}}};
    return check(errorCode(service.handle(QJsonDocument(stolenId).toJson(), &noGrants))
                     == QStringLiteral("session.invalid"),
                 "another client's session id must not transfer its permissions");
}

bool testUntrustedHelloCannotGrantAccess()
{
    ControlResourceStore store;
    ControlService service(&store);
    for (const SessionAuthorization context : {SessionAuthorization::Unauthenticated,
                                               SessionAuthorization::Observer}) {
        for (const QJsonObject& claim : {
                 QJsonObject{{QStringLiteral("auth"), QJsonObject{
                     {QStringLiteral("scheme"), QStringLiteral("bearer")},
                     {QStringLiteral("token"), QStringLiteral("private-test-token")}}}},
                 QJsonObject{{QStringLiteral("grants"), QJsonArray{QStringLiteral("transmit")}}}}) {
            ControlSession session(&store, 4096, context);
            QJsonObject params = claim;
            params.insert(QStringLiteral("versions"), QJsonArray{1});
            const ServiceReply reply = invoke(service, session, QStringLiteral("hello"), params);
            if (!check(!errorCode(reply).isEmpty() && reply.closeAfterWrite
                           && !session.isNegotiated()
                           && !QJsonDocument(reply.message).toJson().contains("private-test-token"),
                       "hello must reject credentials/grant claims without leaking their value")) {
                return false;
            }
        }
    }
    return true;
}

bool testHelloAuthorizationPrecedesParams()
{
    ControlResourceStore store;
    ControlService service(&store);
    for (const QJsonObject& params : {
             QJsonObject{},
             QJsonObject{{QStringLiteral("versions"), QJsonArray{}}},
             QJsonObject{{QStringLiteral("versions"), QJsonArray{2}}},
             QJsonObject{{QStringLiteral("versions"), QJsonArray{1}}}}) {
        ControlSession session(&store, 4096);
        const ServiceReply reply = invoke(service, session, QStringLiteral("hello"), params);
        if (!check(errorCode(reply) == QStringLiteral("auth.required")
                       && reply.closeAfterWrite && !session.isNegotiated()
                       && reply.message.value(QStringLiteral("error")).toObject()
                           .value(QStringLiteral("details")).toObject().isEmpty(),
                   "unauthenticated hello must not disclose parameter/version negotiation")) {
            return false;
        }
    }
    ControlSession observer(&store, 4096, SessionAuthorization::Observer);
    return check(errorCode(invoke(service, observer, QStringLiteral("hello"),
                                 {{QStringLiteral("versions"), QJsonArray{2}}}))
                     == QStringLiteral("protocol.version_unsupported"),
                 "trusted observers must retain version negotiation errors");
}

bool testOutputBindingContract()
{
    ControlResourceStore store;
    ControlService service(&store);
    ControlSession session(&store, 4096, SessionAuthorization::Observer);
    QObject transport;
    int writes = 0;
    int aborts = 0;
    int rejectedCallbacks = 0;
    const auto rejectedWrite = [&](const QByteArray&) { ++rejectedCallbacks; return true; };
    const auto rejectedAbort = [&] { ++rejectedCallbacks; };

    // These must be rejected without consuming the one permitted binding.
    session.bindOutputTransport(nullptr, rejectedWrite, rejectedAbort);
    session.bindOutputTransport(&transport, {}, rejectedAbort);
    session.bindOutputTransport(&transport, rejectedWrite, {});

    QThread worker;
    QObject foreignTransport;
    foreignTransport.moveToThread(&worker);
    worker.start();
    session.bindOutputTransport(&foreignTransport, rejectedWrite, rejectedAbort);
    // Same-affinity endpoints are insufficient if the call itself is off-thread.
    QMetaObject::invokeMethod(&foreignTransport, [&] {
        session.bindOutputTransport(&transport, rejectedWrite, rejectedAbort);
        foreignTransport.moveToThread(transport.thread());
    }, Qt::BlockingQueuedConnection);
    worker.quit();
    worker.wait();

    session.bindOutputTransport(&transport, [&](const QByteArray&) {
        ++writes;
        return true;
    }, [&] { ++aborts; });
    // Reject rebinds to both the same context and a different live context.
    session.bindOutputTransport(&transport, rejectedWrite, rejectedAbort);
    QObject duplicateTransport;
    session.bindOutputTransport(&duplicateTransport, rejectedWrite, rejectedAbort);
    hello(service, session);
    QJsonObject baseline;
    if (!check(!session.subscribe(kSelectors, &baseline), "binding observer must subscribe")) {
        return false;
    }
    store.upsert(kServer, {{QStringLiteral("health"), QStringLiteral("ok")}});
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    session.revokeAuthorization();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    return check(writes == 1 && aborts == 1 && rejectedCallbacks == 0,
                 "invalid and repeated bindings must install no callbacks in release builds");
}

bool testRevocationStopsDelivery()
{
    ControlResourceStore store;
    ControlService service(&store);
    ControlSession first(&store, 4096, SessionAuthorization::Observer);
    ControlSession second(&store, 4096, SessionAuthorization::Observer);
    hello(service, first);
    hello(service, second);
    QJsonObject firstBaseline;
    QJsonObject secondBaseline;
    if (!check(!first.subscribe(kSelectors, &firstBaseline)
                   && !second.subscribe(kSelectors, &secondBaseline),
               "both observers must install independent subscriptions")) {
        return false;
    }

    // Exercise the production binding with a socket-free output sink. Revoke
    // after enqueue, before the queued drain gets its event-loop turn.
    QObject transport;
    QList<QByteArray> delivered;
    int revokedCount = 0;
    bool emptyAtRevocation = false;
    first.bindOutputTransport(&transport, [&](const QByteArray& frame) {
        delivered.append(frame);
        return true;
    }, [&] {
        ++revokedCount;
        emptyAtRevocation = first.takePendingFrames().isEmpty() && !first.canObserve();
    });
    store.upsert(kServer, {{QStringLiteral("health"), QStringLiteral("before-revoke")}});
    if (!check(first.sequence() == 1, "the revoked client must have had a pending event")) {
        return false;
    }
    first.revokeAuthorization();
    first.revokeAuthorization();
    QCoreApplication::processEvents();
    store.upsert(kServer, {{QStringLiteral("health"), QStringLiteral("after-revoke")}});
    store.remove(kServer);
    QCoreApplication::processEvents();
    if (!check(revokedCount == 1 && emptyAtRevocation && delivered.isEmpty()
                   && first.sequence() == 1 && first.takePendingFrames().isEmpty()
                   && second.sequence() == 3 && !second.takePendingFrames().isEmpty(),
               "revocation must stop queued/new delivery without affecting another client")) {
        return false;
    }
    QJsonObject result;
    const std::optional<ProtocolError> resubscribe = first.subscribe(kSelectors, &result);
    const std::optional<ProtocolError> unsubscribe = first.unsubscribe(
        firstBaseline.value(QStringLiteral("subscription")).toString(), &result);
    if (!check(resubscribe && unsubscribe
                   && resubscribe->code == QStringLiteral("auth.invalid")
                   && unsubscribe->code == QStringLiteral("auth.invalid") && result.isEmpty(),
               "direct session operations must remain denied after revocation")) {
        return false;
    }
    for (const QString& method : {QStringLiteral("hello"), QStringLiteral("capabilities.get"),
                                  QStringLiteral("resource.get"), QStringLiteral("resource.subscribe")}) {
        const ServiceReply reply = invoke(service, first, method);
        if (!check(errorCode(reply) == QStringLiteral("auth.invalid") && reply.closeAfterWrite,
                   "revocation must be terminal for every method including renegotiation")) {
            return false;
        }
    }
    ControlSession fresh(&store, 4096, SessionAuthorization::Observer);
    return check(hello(service, fresh).message.contains(QStringLiteral("result"))
                     && fresh.sessionId() != first.sessionId(),
                 "a new verified connection must get a fresh independent session");
}

bool testRevokeBeforeHelloAndDuringResync()
{
    ControlResourceStore store;
    ControlService service(&store);
    ControlSession beforeHello(&store, 4096, SessionAuthorization::Observer);
    beforeHello.revokeAuthorization();
    if (!check(errorCode(hello(service, beforeHello)) == QStringLiteral("auth.invalid")
                   && !beforeHello.isNegotiated(),
               "revocation before hello must not be undone by negotiation")) {
        return false;
    }
    ControlSession overflowing(&store, 360, SessionAuthorization::Observer);
    hello(service, overflowing);
    QJsonObject baseline;
    if (!check(!overflowing.subscribe(kSelectors, &baseline),
               "resync test observer must subscribe")) {
        return false;
    }
    store.upsert(kServer, {{QStringLiteral("data"), QString(1024, QLatin1Char('x'))}});
    if (!check(overflowing.sequence() == 2, "overflow must enqueue a resync notice")) {
        return false;
    }
    overflowing.revokeAuthorization();
    return check(overflowing.takePendingFrames().isEmpty()
                     && errorCode(hello(service, overflowing)) == QStringLiteral("auth.invalid"),
                 "revocation must discard resync notices and forbid recovery on that session");
}

// An injected transport, not a radio peer. The session cannot reach or clear
// buffered bytes except through the same abort callback the local socket uses.
class BufferedTransport final : public QObject {
public:
    void bind(ControlSession& session)
    {
        session.bindOutputTransport(this, [this](const QByteArray& frame) {
            if (aborted) {
                return false;
            }
            buffered.append(frame);
            return true;
        }, [this] {
            ++abortCount;
            aborted = true;
            buffered.clear();
        });
    }

    void queueFlush()
    {
        QMetaObject::invokeMethod(this, [this] {
            // Do not consult the session or suppress flushing after revocation:
            // only an actual transport abort can purge this independent queue.
            delivered.append(buffered);
            buffered.clear();
        }, Qt::QueuedConnection);
    }

    QList<QByteArray> buffered;
    QList<QByteArray> delivered;
    int abortCount{0};
    bool aborted{false};
};

bool testBufferedTransportRevocation()
{
    ControlResourceStore store;
    ControlService service(&store);
    ControlSession first(&store, 4096, SessionAuthorization::Observer);
    ControlSession second(&store, 4096, SessionAuthorization::Observer);
    BufferedTransport firstTransport;
    BufferedTransport secondTransport;
    firstTransport.bind(first);
    secondTransport.bind(second);
    hello(service, first);
    hello(service, second);
    QJsonObject baseline;
    if (!check(!first.subscribe(kSelectors, &baseline)
                   && !second.subscribe(kSelectors, &baseline),
               "buffered observers must subscribe")) {
        return false;
    }
    store.upsert(kServer, {{QStringLiteral("health"), QStringLiteral("buffered")}});
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    if (!check(firstTransport.buffered.size() == 1 && secondTransport.buffered.size() == 1
                   && firstTransport.delivered.isEmpty() && secondTransport.delivered.isEmpty()
                   && first.takePendingFrames().isEmpty(),
               "production binding must hand pending observations to transport-owned buffers")) {
        return false;
    }

    // Both flushes predate revocation. Also leave a new session drain queued.
    // A queued abort would lose to the first flush and leak the buffered frame.
    firstTransport.queueFlush();
    secondTransport.queueFlush();
    store.upsert(kServer, {{QStringLiteral("health"), QStringLiteral("pending")}});
    first.revokeAuthorization();
    first.revokeAuthorization();
    if (!check(firstTransport.abortCount == 1 && firstTransport.aborted
                   && firstTransport.buffered.isEmpty()
                   && secondTransport.abortCount == 0 && secondTransport.buffered.size() == 1,
               "revocation must synchronously abort and purge only its transport")) {
        return false;
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    store.remove(kServer);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    firstTransport.queueFlush();
    secondTransport.queueFlush();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    return check(firstTransport.buffered.isEmpty() && firstTransport.delivered.isEmpty()
                     && secondTransport.delivered.size() == 3,
                 "queued flush/drain and later events must not deliver revoked output");
}

bool testOutputTransportLifetime()
{
    ControlResourceStore store;
    ControlService service(&store);
    auto session = std::make_unique<ControlSession>(
        &store, 4096, SessionAuthorization::Observer);
    BufferedTransport transport;
    transport.bind(*session);
    hello(service, *session);
    QJsonObject baseline;
    if (!check(!session->subscribe(kSelectors, &baseline), "lifetime observer must subscribe")) {
        return false;
    }
    store.upsert(kServer, {{QStringLiteral("health"), QStringLiteral("pending")}});
    session.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    if (!check(transport.buffered.isEmpty(),
               "a queued drain must not dereference a destroyed session")) {
        return false;
    }
    ControlSession revoked(&store, 4096, SessionAuthorization::Observer);
    revoked.revokeAuthorization();
    BufferedTransport lateTransport;
    lateTransport.bind(revoked);
    return check(lateTransport.aborted && lateTransport.abortCount == 1,
                 "binding after revocation must abort immediately, not miss the signal");
}

bool testOutputCallbackStopsBatch()
{
    enum class Stop { Revoke, DestroySession, DestroyTransport, WriteFailure };
    for (const Stop stop : {Stop::Revoke, Stop::DestroySession,
                            Stop::DestroyTransport, Stop::WriteFailure}) {
        ControlResourceStore store;
        ControlService service(&store);
        auto session = std::make_unique<ControlSession>(
            &store, 4096, SessionAuthorization::Observer);
        auto transport = std::make_unique<QObject>();
        int writes = 0;
        session->bindOutputTransport(transport.get(), [&](const QByteArray&) {
            ++writes;
            switch (stop) {
            case Stop::Revoke: session->revokeAuthorization(); break;
            case Stop::DestroySession: session.reset(); break;
            case Stop::DestroyTransport: transport.reset(); break;
            case Stop::WriteFailure: return false;
            }
            return true;
        }, [] {});
        hello(service, *session);
        QJsonObject baseline;
        if (!check(!session->subscribe({{QStringLiteral("server"), {}, {}},
                                        {QStringLiteral("radioSession"), {}, {}}}, &baseline),
                   "batch observer must subscribe to both resources")) {
            return false;
        }
        store.upsert(kServer, {{QStringLiteral("health"), QStringLiteral("ok")}});
        store.upsert({QStringLiteral("radioSession"), {}, QStringLiteral("radio-1")}, {});
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        if (!check(writes == 1, "a stopped transport callback must prevent the next batch write")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    return testAuthenticationAndGrants()
        && testUntrustedHelloCannotGrantAccess()
        && testHelloAuthorizationPrecedesParams()
        && testOutputBindingContract()
        && testBufferedTransportRevocation()
        && testRevocationStopsDelivery()
        && testOutputTransportLifetime()
        && testOutputCallbackStopsBatch()
        && testRevokeBeforeHelloAndDuringResync() ? 0 : 1;
}
