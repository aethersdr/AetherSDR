#include "core/control/LocalControlServer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QThread>
#include <QTextStream>
#include <QUuid>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#include <cstdio>
#include <functional>

using AetherSDR::control::LocalControlServer;
using AetherSDR::control::ProtocolLimits;

namespace {

bool check(bool condition, const char* message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "%s\n", message);
    return false;
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

QString uniqueName(const QString& prefix)
{
    return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool connectSocket(QLocalSocket* socket, const LocalControlServer& server)
{
    socket->connectToServer(server.fullServerName());
    return waitUntil([socket] {
        return socket->state() == QLocalSocket::ConnectedState;
    });
}

QJsonObject readResponse(QLocalSocket* socket)
{
    if (!waitUntil([socket] { return socket->canReadLine(); })) {
        return {};
    }
    QJsonParseError error;
    const QJsonDocument response = QJsonDocument::fromJson(socket->readLine(), &error);
    if (error.error != QJsonParseError::NoError || !response.isObject()) {
        return {};
    }
    return response.object();
}

QJsonObject exchange(QLocalSocket* socket, const QJsonObject& request)
{
    QByteArray bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    socket->write(bytes);
    socket->flush();
    return readResponse(socket);
}

QJsonObject helloRequest(QJsonObject extraParams = {})
{
    QJsonObject params{{QStringLiteral("versions"), QJsonArray{1}}};
    for (auto it = extraParams.constBegin(); it != extraParams.constEnd(); ++it) {
        params.insert(it.key(), it.value());
    }
    return {
        {QStringLiteral("v"), 1},
        {QStringLiteral("id"), QStringLiteral("hello-1")},
        {QStringLiteral("method"), QStringLiteral("hello")},
        {QStringLiteral("params"), params}
    };
}

QString errorCode(const QJsonObject& response)
{
    return response.value(QStringLiteral("error")).toObject()
        .value(QStringLiteral("code")).toString();
}

bool contains(const QJsonArray& values, const QString& expected)
{
    for (const QJsonValue& value : values) {
        if (value.toString() == expected) {
            return true;
        }
    }
    return false;
}

bool runProtocolTest()
{
    LocalControlServer server;
    if (!check(QString::fromLatin1(server.metaObject()->className())
                   == QStringLiteral("AetherSDR::control::LocalControlServer"),
               "local server must expose its own Qt meta-object")
        || !check(server.listen(uniqueName(QStringLiteral("aetherd-protocol-"))),
               "local server must listen")) {
        return false;
    }

#ifdef Q_OS_UNIX
    const QFileInfo runtimeDirectory(QFileInfo(server.fullServerName()).absolutePath());
    const QFileDevice::Permissions forbidden =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    if (!check(runtimeDirectory.ownerId() == static_cast<uint>(::getuid()),
               "runtime directory must belong to the current user")
        || !check((runtimeDirectory.permissions() & forbidden) == 0,
                  "runtime directory must reject group and other access")) {
        return false;
    }
#endif

    QLocalSocket socket;
    if (!check(connectSocket(&socket, server), "client must connect")) {
        return false;
    }

    const QJsonObject hello = exchange(&socket, helloRequest({
        {QStringLiteral("client"), QJsonObject{
             {QStringLiteral("name"), QStringLiteral("test-client")},
             {QStringLiteral("version"), QStringLiteral("1.0")}}}
    }));
    const QJsonObject welcome = hello.value(QStringLiteral("result")).toObject();
    const QString sessionId = welcome.value(QStringLiteral("sessionId")).toString();
    const QJsonArray grants = welcome.value(QStringLiteral("grants")).toArray();
    const QJsonArray capabilities = welcome.value(QStringLiteral("capabilities")).toArray();
    const QJsonObject limits = welcome.value(QStringLiteral("limits")).toObject();
    if (!check(!sessionId.isEmpty(), "hello must create a sessionId")
        || !check(grants.size() == 1 && contains(grants, QStringLiteral("observe")),
                  "server must grant observe only")
        || !check(capabilities.size() == 1
                      && contains(capabilities, QStringLiteral("server.read")),
                  "server must advertise server.read only")
        || !check(!contains(capabilities, QStringLiteral("transmit"))
                      && !contains(capabilities, QStringLiteral("control")),
                  "server must not advertise control or transmit")
        || !check(limits.size() == 1
                      && limits.value(QStringLiteral("maxMessageBytes")).toInteger()
                          == ProtocolLimits::kMaxMessageBytes,
                  "server must advertise only its currently enforced protocol limit")) {
        return false;
    }

    const QJsonObject capabilityReply = exchange(&socket, {
        {QStringLiteral("v"), 1},
        {QStringLiteral("id"), QStringLiteral("caps-1")},
        {QStringLiteral("sessionId"), sessionId},
        {QStringLiteral("method"), QStringLiteral("capabilities.get")},
        {QStringLiteral("params"), QJsonObject{}}
    });
    if (!check(capabilityReply.value(QStringLiteral("result")).isObject(),
               "capabilities.get must succeed for the negotiated session")) {
        return false;
    }

    const QJsonObject wrongSession = exchange(&socket, {
        {QStringLiteral("v"), 1},
        {QStringLiteral("id"), QStringLiteral("caps-2")},
        {QStringLiteral("sessionId"), QStringLiteral("not-this-session")},
        {QStringLiteral("method"), QStringLiteral("capabilities.get")},
        {QStringLiteral("params"), QJsonObject{}}
    });
    if (!check(errorCode(wrongSession) == QStringLiteral("session.invalid"),
               "a request from the wrong session must fail closed")) {
        return false;
    }

    QLocalSocket unnegotiated;
    if (!check(connectSocket(&unnegotiated, server), "second client must connect")) {
        return false;
    }
    const QJsonObject firstRequest = exchange(&unnegotiated, {
        {QStringLiteral("v"), 1},
        {QStringLiteral("id"), QStringLiteral("not-hello")},
        {QStringLiteral("sessionId"), QStringLiteral("invented")},
        {QStringLiteral("method"), QStringLiteral("capabilities.get")},
        {QStringLiteral("params"), QJsonObject{}}
    });
    return check(errorCode(firstRequest) == QStringLiteral("protocol.invalid_envelope"),
                 "the first request must be hello")
        && check(waitUntil([&unnegotiated] {
            return unnegotiated.state() == QLocalSocket::UnconnectedState;
        }), "a client that skips hello must be disconnected");
}

bool runHelloValidationTest()
{
    LocalControlServer server;
    if (!check(server.listen(uniqueName(QStringLiteral("aetherd-validation-"))),
               "validation server must listen")) {
        return false;
    }

    const auto rejectedHello = [&server](const QJsonObject& request,
                                         const QString& expectedCode) {
        QLocalSocket socket;
        if (!connectSocket(&socket, server)) {
            return false;
        }
        const QJsonObject response = exchange(&socket, request);
        return errorCode(response) == expectedCode
            && waitUntil([&socket] {
                return socket.state() == QLocalSocket::UnconnectedState;
            });
    };

    if (!check(rejectedHello(helloRequest({{QStringLiteral("typo"), true}}),
                                     QStringLiteral("request.invalid_params")),
               "unknown hello parameters must fail closed")) {
        return false;
    }
    if (!check(rejectedHello(helloRequest({
                                     {QStringLiteral("client"), QJsonObject{
                                          {QStringLiteral("name"), QStringLiteral("client")},
                                          {QStringLiteral("version"), QStringLiteral("1")},
                                          {QStringLiteral("typo"), true}}}}),
                                     QStringLiteral("request.invalid_params")),
               "unknown client parameters must fail closed")) {
        return false;
    }
    if (!check(rejectedHello(helloRequest({
                                     {QStringLiteral("auth"), QJsonObject{
                                          {QStringLiteral("scheme"), QStringLiteral("bearer")},
                                          {QStringLiteral("token"), QStringLiteral("invalid")}}}}),
                                     QStringLiteral("auth.invalid")),
               "unverified authentication material must be rejected")) {
        return false;
    }

    QJsonObject malformedVersions = helloRequest();
    QJsonObject params = malformedVersions.value(QStringLiteral("params")).toObject();
    params.insert(QStringLiteral("versions"), QJsonArray{1.5});
    malformedVersions.insert(QStringLiteral("params"), params);
    if (!check(rejectedHello(malformedVersions, QStringLiteral("request.invalid_params")),
               "malformed versions must not be treated as merely unsupported")) {
        return false;
    }

    QJsonObject unsupportedVersion = helloRequest();
    params = unsupportedVersion.value(QStringLiteral("params")).toObject();
    params.insert(QStringLiteral("versions"), QJsonArray{2});
    unsupportedVersion.insert(QStringLiteral("params"), params);
    return check(rejectedHello(unsupportedVersion,
                               QStringLiteral("protocol.version_unsupported")),
                 "a v1 bootstrap with no mutually supported version must be rejected");
}

bool runMalformedInputTest()
{
    LocalControlServer server;
    if (!check(server.listen(uniqueName(QStringLiteral("aetherd-invalid-json-"))),
               "invalid-JSON server must listen")) {
        return false;
    }
    QLocalSocket socket;
    if (!check(connectSocket(&socket, server), "invalid-JSON client must connect")) {
        return false;
    }
    socket.write(QByteArrayLiteral("{not-json}\n"));
    socket.flush();
    const QJsonObject response = readResponse(&socket);
    return check(errorCode(response) == QStringLiteral("protocol.invalid_json"),
                 "invalid JSON must receive the declared parse error")
        && check(!response.contains(QStringLiteral("id")),
                 "an uncorrelated parse error must omit the request id")
        && check(waitUntil([&socket] {
            return socket.state() == QLocalSocket::UnconnectedState;
        }), "invalid JSON before hello must disconnect the client");
}

bool runHandshakeTimeoutTest()
{
    LocalControlServer::Limits limits;
    limits.handshakeTimeoutMs = 25;
    LocalControlServer server(nullptr, limits);
    if (!check(server.listen(uniqueName(QStringLiteral("aetherd-timeout-"))),
               "timeout server must listen")) {
        return false;
    }
    QLocalSocket socket;
    if (!check(connectSocket(&socket, server), "timeout client must connect")) {
        return false;
    }
    const QJsonObject response = readResponse(&socket);
    return check(errorCode(response) == QStringLiteral("engine.timeout"),
                 "idle client must receive a handshake timeout")
        && check(!response.contains(QStringLiteral("id")),
                 "an uncorrelated handshake timeout must omit the request id")
        && check(waitUntil([&socket] {
            return socket.state() == QLocalSocket::UnconnectedState;
        }), "idle client must be disconnected after handshake timeout");
}

bool runHandshakeTimeoutBackpressureTest()
{
    LocalControlServer::Limits limits;
    limits.handshakeTimeoutMs = 25;
    limits.maxQueuedOutputBytes = 1;
    LocalControlServer server(nullptr, limits);
    if (!check(server.listen(uniqueName(QStringLiteral("aetherd-timeout-pressure-"))),
               "timeout/backpressure server must listen")) {
        return false;
    }
    QLocalSocket socket;
    if (!check(connectSocket(&socket, server),
               "timeout/backpressure client must connect")) {
        return false;
    }
    return check(waitUntil([&socket] {
        return socket.state() == QLocalSocket::UnconnectedState;
    }), "handshake timeout output overflow must defer client destruction safely");
}

bool runClientLimitTest()
{
    LocalControlServer::Limits limits;
    limits.maxClients = 2;
    LocalControlServer server(nullptr, limits);
    if (!check(server.listen(uniqueName(QStringLiteral("aetherd-clients-"))),
               "client-limit server must listen")) {
        return false;
    }
    QLocalSocket first;
    QLocalSocket second;
    QLocalSocket rejected;
    if (!check(connectSocket(&first, server) && connectSocket(&second, server),
               "allowed clients must connect")) {
        return false;
    }
    if (!check(exchange(&first, helloRequest()).value(QStringLiteral("result")).isObject()
                   && exchange(&second, helloRequest()).value(QStringLiteral("result")).isObject(),
               "allowed clients must be accepted before testing the cap")) {
        return false;
    }
    if (!check(connectSocket(&rejected, server), "excess client must reach rejection path")) {
        return false;
    }
    const QJsonObject response = readResponse(&rejected);
    return check(errorCode(response) == QStringLiteral("transport.limit_exceeded"),
                 "excess client must receive the declared limit error")
        && check(!response.contains(QStringLiteral("id")),
                 "an uncorrelated client-limit error must omit the request id")
        && check(waitUntil([&rejected] {
            return rejected.state() == QLocalSocket::UnconnectedState;
        }), "excess client must be disconnected");
}

bool runOversizedInputTest()
{
    LocalControlServer server;
    if (!check(server.listen(uniqueName(QStringLiteral("aetherd-input-limit-"))),
               "input-limit server must listen")) {
        return false;
    }
    QLocalSocket socket;
    if (!check(connectSocket(&socket, server), "input-limit client must connect")) {
        return false;
    }
    socket.write(QByteArray(ProtocolLimits::kMaxMessageBytes + 1, 'x'));
    socket.flush();
    const QJsonObject response = readResponse(&socket);
    return check(errorCode(response) == QStringLiteral("transport.limit_exceeded"),
                 "an oversized input frame must receive the declared limit error")
        && check(!response.contains(QStringLiteral("id")),
                 "an uncorrelated input-limit error must omit the request id")
        && check(waitUntil([&socket] {
            return socket.state() == QLocalSocket::UnconnectedState;
        }), "an oversized input frame must disconnect the client");
}

bool runBackpressureTest()
{
    LocalControlServer::Limits limits;
    limits.maxQueuedOutputBytes = 1;
    LocalControlServer server(nullptr, limits);
    if (!check(server.listen(uniqueName(QStringLiteral("aetherd-backpressure-"))),
               "backpressure server must listen")) {
        return false;
    }
    QLocalSocket socket;
    if (!check(connectSocket(&socket, server), "backpressure client must connect")) {
        return false;
    }
    QByteArray request = QJsonDocument(helloRequest()).toJson(QJsonDocument::Compact);
    request.append('\n');
    socket.write(request);
    socket.flush();
    return check(waitUntil([&socket] {
        return socket.state() == QLocalSocket::UnconnectedState;
    }), "output overflow must disconnect without reusing destroyed client state");
}

bool runStaleEndpointTest()
{
    const QString name = uniqueName(QStringLiteral("aetherd-stale-"));
    QString endpoint;
    {
        LocalControlServer first;
        if (!check(first.listen(name), "initial stale-endpoint server must listen")) {
            return false;
        }
        endpoint = first.fullServerName();
        LocalControlServer duplicate;
        if (!check(!duplicate.listen(name),
                   "a live endpoint lock must prevent stale cleanup")) {
            return false;
        }
        QLocalSocket probe;
        if (!check(connectSocket(&probe, first),
                   "failed duplicate listen must leave the live server reachable")) {
            return false;
        }
    }

#ifndef Q_OS_WIN
    QFile stale(endpoint);
    if (!check(stale.open(QIODevice::WriteOnly), "test must create a stale endpoint")) {
        return false;
    }
    stale.close();
#endif

    LocalControlServer restarted;
    return check(restarted.listen(name),
                 "server must recover a stale endpoint after acquiring its lock");
}

bool runCrashRecoveryTest()
{
    const QString name = uniqueName(QStringLiteral("aetherd-crash-"));
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--crash-server"), name});
    if (!check(child.waitForStarted(), "crash helper must start")
        || !check(child.waitForReadyRead(2000), "crash helper must publish its endpoint")) {
        return false;
    }
    const QString endpoint = QString::fromUtf8(child.readLine()).trimmed();
    if (!check(!endpoint.isEmpty(), "crash helper must listen before termination")) {
        return false;
    }
    child.kill();
    if (!check(child.waitForFinished(), "crash helper must terminate without cleanup")) {
        return false;
    }

    LocalControlServer restarted;
    return check(restarted.listen(name),
                 "server must recover the stale lock and endpoint left by a crash");
}

bool runEndpointValidationTest()
{
    LocalControlServer server;
    return check(!server.listen(QStringLiteral("../shared/socket")),
                 "logical endpoint names must reject paths and traversal");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3
        && app.arguments().at(1) == QStringLiteral("--crash-server")) {
        LocalControlServer server;
        if (!server.listen(app.arguments().at(2))) {
            return 2;
        }
        QTextStream(stdout) << server.fullServerName() << Qt::endl;
        return app.exec();
    }
    return runProtocolTest()
        && runHelloValidationTest()
        && runMalformedInputTest()
        && runHandshakeTimeoutTest()
        && runHandshakeTimeoutBackpressureTest()
        && runClientLimitTest()
        && runOversizedInputTest()
        && runBackpressureTest()
        && runStaleEndpointTest()
        && runCrashRecoveryTest()
        && runEndpointValidationTest() ? 0 : 1;
}
