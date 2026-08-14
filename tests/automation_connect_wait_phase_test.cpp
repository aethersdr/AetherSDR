// `connect wait` has to be able to say "still working" — and a deferred
// connect failure has to reach the client that asked (#4912).
//
// Two defects, one reply shape:
//
//   1. The wait bound exactly one edge, RadioModel::connectionStateChanged, so
//      it had two outcomes: connected, or "timed out waiting for radio
//      connection". Nothing distinguished a connect that had died from one
//      still in flight. The HL2 makes the second case ordinary —
//      Hl2Backend::connectRadio() parks the request behind the DSP open and
//      re-drives it from finishDspSetup(), emitting nothing in between — so a
//      wait can expire mid-queue on a connect that then succeeds. Polling
//      `get radio → .radio.connected` was the only reliable route.
//
//   2. Every connect verb replies {ok:true, deferred:true} before its real work
//      runs, and a failure afterwards reached only qCWarning. So a connect that
//      was rejected outright still looked accepted, and the wait that followed
//      burned its whole timeout before reporting a timeout — the wrong
//      diagnosis for something that had already failed.
//
// The fake connection here refuses on demand, which is what makes case 2
// deterministic: no radio, no network, no timing window.

#include "TestSettingsProfile.h"
// AutomationServer holds QPointers to both; the complete types are needed for
// its members to instantiate, as in the sibling automation tests.
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "core/IConnectionAutomation.h"
#include "core/RadioDiscovery.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>

#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR {

// noteConnectFailure() is private; the header already befriends this name for
// the other automation tests.
class AutomationServerTestAccess
{
public:
    static void noteFailure(AutomationServer& server, const QString& what,
                            const QString& error, bool answerPendingWaits)
    {
        server.noteConnectFailure(what, error, answerPendingWaits);
    }
};

} // namespace AetherSDR

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

// One socket per outstanding request: `connect wait` is deferred, so the socket
// that issued it is still waiting for its reply while the next verb is sent.
class BridgeClient
{
public:
    bool connectToServer(const QString& serverName)
    {
        m_socket.connectToServer(serverName);
        return m_socket.waitForConnected(1000);
    }

    void send(const QJsonObject& request)
    {
        QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
        payload.append('\n');
        m_socket.write(payload);
        m_socket.flush();
    }

    QJsonObject readReply(int timeoutMs)
    {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&m_socket, &QLocalSocket::readyRead, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(timeoutMs);
        while (!m_socket.canReadLine() && timeout.isActive()) {
            loop.exec();
        }
        if (!m_socket.canReadLine()) {
            return {};
        }
        return QJsonDocument::fromJson(m_socket.readLine()).object();
    }

    QJsonObject request(const QJsonObject& r, int timeoutMs = 2000)
    {
        send(r);
        return readReply(timeoutMs);
    }

private:
    QLocalSocket m_socket;
};

QJsonObject connectVerb(const QString& action, const QString& value = QString())
{
    QJsonObject o{
        {QStringLiteral("cmd"), QStringLiteral("connect")},
        {QStringLiteral("action"), action},
    };
    if (!value.isEmpty()) {
        o.insert(QStringLiteral("value"), value);
    }
    return o;
}

// Refuses connect-by-ip when armed, so the deferred-failure path can be driven
// without a radio.
class RefusingConnection : public QObject, public IConnectionAutomation
{
public:
    QList<RadioInfo> automationLocalRadios() const override { return {}; }

    bool automationConnectLocalSerial(const QString&, QString* = nullptr) override
    {
        return true;
    }

    bool automationConnectByIp(const QString&,
                               const QString& = QString(),
                               QString* error = nullptr) override
    {
        if (refuse) {
            if (error) {
                *error = QStringLiteral("no route to radio");
            }
            return false;
        }
        return true;
    }

    bool automationDisconnect(QString* = nullptr) override { return true; }
    bool automationDialogVisible() const override { return false; }
    void automationSetDialogVisible(bool) override {}
    QObject* asQObject() override { return this; }

    bool refuse = true;
};

void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-automation-connect-wait-phase-test"));
    check(settingsProfile.isValid(), "isolated settings profile is available");

    QCoreApplication app(argc, argv);

    RadioModel radio;
    AutomationServer server;
    server.setRadioModel(&radio);

    RefusingConnection conn;
    server.setConnectionAutomation(&conn);

    const QString serverName = QStringLiteral("aether-connect-wait-phase-test-%1")
                                   .arg(QCoreApplication::applicationPid());
    check(server.start(serverName), "automation server starts");

    BridgeClient waiter;
    BridgeClient driver;
    check(waiter.connectToServer(serverName), "waiter client connects");
    check(driver.connectToServer(serverName), "driver client connects");

    // ── A wait with nothing in flight says so ───────────────────────────────
    {
        const QJsonObject reply =
            waiter.request(connectVerb(QStringLiteral("wait"), QStringLiteral("60")),
                           3000);
        check(reply.value(QStringLiteral("timeout")).toBool(),
              "an idle wait still times out");
        check(reply.value(QStringLiteral("phase")).toString() == QLatin1String("idle"),
              "a timeout with no attempt in flight reports phase=idle");
    }

    // ── A deferred connect failure answers the wait immediately ─────────────
    //
    // The wait asks for 5 s. The refused `connect ip` must cut it short well
    // inside that, carrying the connection layer's own message — pre-fix the
    // refusal reached only qCWarning and this wait ran the full 5 s.
    {
        conn.refuse = true;
        waiter.send(connectVerb(QStringLiteral("wait"), QStringLiteral("5000")));
        pump(50);  // let the wait register before the failure is provoked

        QElapsedTimer elapsed;
        elapsed.start();
        const QJsonObject accepted =
            driver.request(connectVerb(QStringLiteral("ip"),
                                       QStringLiteral("192.0.2.99")));
        check(accepted.value(QStringLiteral("ok")).toBool(),
              "connect ip is still accepted up front (the failure is deferred)");

        const QJsonObject reply = waiter.readReply(3000);
        const qint64 tookMs = elapsed.elapsed();
        check(!reply.isEmpty(), "the outstanding wait is answered");
        check(tookMs < 2000, "the wait is answered without burning its 5 s timeout");
        check(!reply.value(QStringLiteral("timeout")).toBool(),
              "the answer is a failure, not a timeout");
        check(reply.value(QStringLiteral("error")).toString()
                  .contains(QLatin1String("no route to radio")),
              "the connection layer's own message reaches the client");
    }

    // ── And the failure is still readable afterwards ────────────────────────
    {
        const QJsonObject reply =
            waiter.request(connectVerb(QStringLiteral("wait"), QStringLiteral("60")),
                           3000);
        check(reply.value(QStringLiteral("lastError")).toString()
                  .contains(QLatin1String("no route to radio")),
              "a later wait still reports the last deferred connect failure");
        check(reply.contains(QStringLiteral("lastErrorAgeMs")),
              "the last failure is aged, so a stale one is distinguishable");
    }

    // ── A fresh attempt retires the previous failure ────────────────────────
    //
    // #4918 review: `m_lastConnectError` was set and never cleared, so one
    // failure rode along on every later non-connected wait for the life of the
    // process — and the field's presence reads as "this attempt failed".
    {
        conn.refuse = false;                       // this one is accepted
        driver.request(connectVerb(QStringLiteral("ip"), QStringLiteral("192.0.2.98")));
        const QJsonObject reply =
            waiter.request(connectVerb(QStringLiteral("wait"), QStringLiteral("60")),
                           3000);
        check(!reply.contains(QStringLiteral("lastError")),
              "scheduling a fresh connect retires the previous failure");
        check(!reply.contains(QStringLiteral("lastErrorAgeMs")),
              "and its age goes with it");
    }

    // ── A failed DISCONNECT does not answer a connect wait ──────────────────
    //
    // #4918 review: noteConnectFailure() completed EVERY pending wait, so a
    // disconnect failure handed its message to an unrelated connect still
    // legitimately in flight.
    //
    // Driven through the verb's failure hook rather than the `disconnect` verb
    // itself: doDisconnect() refuses up front unless the radio is connected, so
    // the deferred path this guards is unreachable from a socket in a test with
    // no radio. Calling the hook directly is what makes the guard testable at
    // all — and the guard, not the route to it, is the thing under test.
    {
        conn.refuse = true;
        waiter.send(connectVerb(QStringLiteral("wait"), QStringLiteral("700")));
        pump(80);

        AutomationServerTestAccess::noteFailure(
            server, QStringLiteral("disconnect"), QStringLiteral("port busy"),
            /*answerPendingWaits=*/false);
        pump(120);

        const QJsonObject reply = waiter.readReply(3000);
        check(!reply.isEmpty(), "the wait still ends, on its own timeout");
        check(reply.value(QStringLiteral("timeout")).toBool(),
              "a failed disconnect does not answer an in-flight connect wait");
        check(!reply.value(QStringLiteral("error")).toString()
                   .contains(QLatin1String("port busy")),
              "and the disconnect's message is never reported as the wait's error");
        check(reply.value(QStringLiteral("lastError")).toString()
                  .contains(QLatin1String("port busy")),
              "it is still RECORDED though — a deferred failure a caller can see");
    }

    // ── The in-flight flag itself ───────────────────────────────────────────
    //
    // Last, because it leaves an attempt on the model. Read synchronously: the
    // point is that connectToRadio() marks the attempt the moment it is asked
    // for, which is the whole window the wait previously could not see. Family
    // "sim" keeps this off the network entirely.
    {
        check(!radio.isConnectAttemptInFlight(),
              "a fresh model reports no connect attempt in flight");

        RadioInfo info;
        info.family = QStringLiteral("sim");
        info.name = QStringLiteral("Demo");
        info.serial = QStringLiteral("SIM-0001");
        info.address = QHostAddress(QStringLiteral("127.0.0.1"));
        radio.connectToRadio(info);
        check(radio.isConnectAttemptInFlight(),
              "connectToRadio() marks the attempt in flight immediately");

        radio.disconnectFromRadio();
        check(!radio.isConnectAttemptInFlight(),
              "abandoning the connect clears the in-flight mark");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
