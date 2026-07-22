#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

class BridgeClient
{
public:
    bool connectToServer(const QString& serverName)
    {
        m_socket.connectToServer(serverName);
        return m_socket.waitForConnected(1000);
    }

    QJsonObject request(const QJsonObject& request)
    {
        QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
        payload.append('\n');

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&m_socket, &QLocalSocket::readyRead,
                         &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout,
                         &loop, &QEventLoop::quit);

        m_socket.write(payload);
        m_socket.flush();
        timeout.start(1000);
        if (!m_socket.canReadLine()) {
            loop.exec();
        }

        if (!m_socket.canReadLine()) {
            return {};
        }
        return QJsonDocument::fromJson(m_socket.readLine()).object();
    }

private:
    QLocalSocket m_socket;
};

QJsonObject tuneRequest(const QJsonValue& id = QJsonValue::Undefined)
{
    QJsonObject request{
        {QStringLiteral("cmd"), QStringLiteral("tune")},
        {QStringLiteral("value"), QStringLiteral("14.225")},
    };
    if (!id.isUndefined()) {
        request.insert(QStringLiteral("id"), id);
    }
    return request;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-automation-json-id-test"));
    check(settingsProfile.isValid(), "isolated settings profile is available");

    QCoreApplication app(argc, argv);
    RadioModel radio;
    AutomationServer server;
    server.setRadioModel(&radio);

    int handlerCalls = 0;
    int capturedSliceId = -99;
    server.setTuneHandler([&](double, int sliceId) {
        ++handlerCalls;
        capturedSliceId = sliceId;
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("sliceId"), sliceId},
        };
    });

    const QString serverName = QStringLiteral("aether-json-id-test-%1")
                                   .arg(QCoreApplication::applicationPid());
    check(server.start(serverName), "automation server starts");

    BridgeClient client;
    check(client.connectToServer(serverName), "client connects to automation server");

    auto expectAccepted = [&](const QJsonObject& request, int expectedSliceId,
                              const char* description) {
        const int callsBefore = handlerCalls;
        const QJsonObject response = client.request(request);
        check(response.value(QStringLiteral("ok")).toBool()
                  && handlerCalls == callsBefore + 1
                  && capturedSliceId == expectedSliceId,
              description);
    };
    auto expectRejected = [&](const QJsonObject& request, const QString& expectedError,
                              const char* description) {
        const int callsBefore = handlerCalls;
        const QJsonObject response = client.request(request);
        check(!response.value(QStringLiteral("ok")).toBool()
                  && response.value(QStringLiteral("error")).toString() == expectedError
                  && handlerCalls == callsBefore,
              description);
    };

    expectAccepted(tuneRequest(), -1, "omitted id keeps active-slice sentinel");
    expectAccepted(tuneRequest(QStringLiteral("1")), 1,
                   "string id reaches the requested slice");
    expectAccepted(tuneRequest(1), 1,
                   "integer JSON id reaches the requested slice");

    const QString typeError = QStringLiteral("id must be a string or number");
    expectRejected(tuneRequest(true), typeError,
                   "boolean id is rejected instead of selecting the active slice");
    expectRejected(tuneRequest(QJsonObject{{QStringLiteral("slice"), 1}}), typeError,
                   "object id is rejected instead of selecting the active slice");
    expectRejected(tuneRequest(QJsonArray{1}), typeError,
                   "array id is rejected instead of selecting the active slice");
    expectRejected(tuneRequest(QJsonValue::Null), typeError,
                   "null id is rejected instead of selecting the active slice");

    const QString integerError =
        QStringLiteral("tune: sliceId must be a non-negative integer");
    expectRejected(tuneRequest(1.5), integerError,
                   "fractional numeric id is rejected");
    expectRejected(tuneRequest(1.0000001), integerError,
                   "near-integer numeric id is not rounded up to a slice");
    expectRejected(tuneRequest(0.9999999), integerError,
                   "near-integer numeric id is not rounded down to a slice");

    server.stop();
    return failures == 0 ? 0 : 1;
}
