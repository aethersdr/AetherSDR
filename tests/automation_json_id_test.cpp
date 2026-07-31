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

QJsonObject tuneValueRequest(const QString& value)
{
    return QJsonObject{
        {QStringLiteral("cmd"), QStringLiteral("tune")},
        {QStringLiteral("value"), value},
    };
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

    // ---- #4550: Hz passed to an MHz verb is refused, not silently no-opped ----
    //
    // A tune handler IS installed above, exactly as MainWindow_Session.cpp's
    // setTuneHandler(...) call does in the shipping app. That matters: the guard
    // is only worth anything if it runs BEFORE the m_tuneHandler dispatch, so
    // these cases also pin that ordering — expectRejected asserts the handler
    // was never reached.
    auto expectTuneValueRejected = [&](const QString& value, const char* description) {
        const int callsBefore = handlerCalls;
        const QJsonObject response = client.request(tuneValueRequest(value));
        const QString error = response.value(QStringLiteral("error")).toString();
        check(!response.value(QStringLiteral("ok")).toBool()
                  && error.startsWith(QStringLiteral("tune takes MHz, not Hz"))
                  && handlerCalls == callsBefore,
              description);
    };
    expectTuneValueRejected(QStringLiteral("14200000"),
                            "#4550: 20m expressed in Hz is refused, not tuned");
    // The case a 1 THz threshold let through: below the old guard, so it fell
    // past it and became a 500000 MHz request — the silent no-op this refuses.
    expectTuneValueRejected(QStringLiteral("500000"),
                            "#4550: 500 kHz expressed in Hz is refused too");

    // Non-finite input. QString::toDouble() happily parses "nan" and "inf",
    // and NaN in particular fails EVERY comparison (NaN <= 0 and NaN > ceiling
    // are both false), so without an explicit isfinite check it sailed through
    // both guards and reached the radio. These must be refused by the
    // positive-finite check — with its message, not a bogus unit diagnosis —
    // and must never reach the handler.
    auto expectNonFiniteRejected = [&](const QString& value, const char* description) {
        const int callsBefore = handlerCalls;
        const QJsonObject response = client.request(tuneValueRequest(value));
        const QString error = response.value(QStringLiteral("error")).toString();
        check(!response.value(QStringLiteral("ok")).toBool()
                  && error == QStringLiteral("tune requires a positive finite frequency in MHz")
                  && handlerCalls == callsBefore,
              description);
    };
    expectNonFiniteRejected(QStringLiteral("nan"), "nan is refused, never tuned");
    expectNonFiniteRejected(QStringLiteral("NaN"), "NaN is refused, never tuned");
    expectNonFiniteRejected(QStringLiteral("inf"), "inf is refused by the finite check");
    expectNonFiniteRejected(QStringLiteral("-inf"), "-inf is refused by the finite check");

    // The ambiguous window (ceiling..300 GHz): could be an Hz mistake OR a real
    // millimetre-wave MHz value (122.25 GHz allocation). Refused, but the
    // message must present both readings instead of confidently asserting the
    // wrong one ("did you mean 0.122250?" to a 122 GHz transverter user).
    {
        const int callsBefore = handlerCalls;
        const QJsonObject response =
            client.request(tuneValueRequest(QStringLiteral("122250")));
        const QString error = response.value(QStringLiteral("error")).toString();
        check(!response.value(QStringLiteral("ok")).toBool()
                  && !error.contains(QStringLiteral("did you mean"))
                  && error.contains(QStringLiteral("millimetre-wave"))
                  && handlerCalls == callsBefore,
              "122.25 GHz in MHz is refused without misdiagnosing it as Hz");
    }

    // A rejected value must be quoted back as sent. Rounding it to whole MHz
    // made everything in (105000, 105000.5) report as "got 105000, above the
    // 105000 MHz ceiling" — a message that contradicts itself, in the branch
    // that exists to stop this guard asserting things that aren't true.
    {
        const int callsBefore = handlerCalls;
        const QJsonObject response =
            client.request(tuneValueRequest(QStringLiteral("105000.4")));
        const QString error = response.value(QStringLiteral("error")).toString();
        check(!response.value(QStringLiteral("ok")).toBool()
                  && error.contains(QStringLiteral("got 105000.4,"))
                  && handlerCalls == callsBefore,
              "a value just over the ceiling is quoted back unrounded");
    }

    // Floor. Nothing below what any supported radio tunes should reach
    // setFrequency(); 0.001 matches the floor typed frequency entry applies to
    // the same value (VfoWidget / RxApplet), so the two paths agree.
    {
        const int callsBefore = handlerCalls;
        const QJsonObject response =
            client.request(tuneValueRequest(QStringLiteral("1e-300")));
        const QString error = response.value(QStringLiteral("error")).toString();
        check(!response.value(QStringLiteral("ok")).toBool()
                  && error == QStringLiteral("tune requires at least 0.001 MHz — got 1e-300")
                  && handlerCalls == callsBefore,
              "a sub-floor frequency is refused, never tuned");
    }
    // ...and the floor itself must not refuse anything real. 0.001 MHz is 1 kHz,
    // below every amateur allocation including the VLF experiments at ~8.3 kHz.
    expectAccepted(tuneValueRequest(QStringLiteral("0.001")), -1,
                   "the floor value itself still tunes");

    // The ceiling must not refuse anything real. 10368.1 is the 3cm calling
    // frequency, the top of the band table, and it has to keep working.
    expectAccepted(tuneValueRequest(QStringLiteral("10368.1")), -1,
                   "#4550: the top of the band table still tunes");
    // kHz-for-MHz (14200 = 20m in kHz, or 14.2 GHz in MHz) deliberately passes:
    // the value is indistinguishable from a legitimate transverter request in
    // the headroom the ceiling protects. Pinned as a DECISION, not an omission
    // — if this ever starts rejecting, the transverter range broke.
    expectAccepted(tuneValueRequest(QStringLiteral("14200")), -1,
                   "kHz-for-MHz passes: indistinguishable from a 14.2 GHz request");

    server.stop();
    return failures == 0 ? 0 : 1;
}
