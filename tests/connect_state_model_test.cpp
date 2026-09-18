// radio.connectState through the production model mapping and bridge snapshot.
// The policy-only test cannot catch RadioModel feeding the policy a constant
// false or a DSP-only input instead of the attempt lifecycle (#5416 review).
//
// Inject the attempt input through RadioModel's existing private-state test
// friendship, remove its unused backend, and read the real getters/dispatcher.
// Cancellation and failure use the production handlers. No connectToRadio(),
// network address, socket, listener, discovery, radio peer, or event-loop pump.
// This covers the mapping and end edges; it does not claim to exercise the
// request-edge assignment inside connectToRadio().
#include "TestSettingsProfile.h"
// AutomationServer's QPointer members require these complete types, even
// though this test neither constructs an audio engine nor a recorder.
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

namespace AetherSDR {

class AutomationServerTestAccess
{
public:
    static QJsonObject handleLine(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};

// Existing RadioModel test friendship, also used by icom_identity_test.
struct RadioModelWakeTestAccess
{
    static void removeTransport(RadioModel& radio)
    {
        radio.teardownBackend();
    }
    static void beginAttempt(RadioModel& radio)
    {
        radio.m_connectAttemptActive = true;
    }
    static void failAttempt(RadioModel& radio)
    {
        radio.onConnectionError(QStringLiteral("injected connect failure"));
    }
    static bool hasTransportOrRetry(const RadioModel& radio)
    {
        return radio.m_backend || radio.m_connection || radio.m_wanConn
            || radio.m_reconnectTimer.isActive();
    }
};

} // namespace AetherSDR

using namespace AetherSDR;

namespace {
int failures = 0;

void check(bool ok, const char* message)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", message);
    if (!ok) {
        ++failures;
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-connect-state-model-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    RadioModel radio;
    RadioModelWakeTestAccess::removeTransport(radio);
    AutomationServer server;
    server.setRadioModel(&radio);

    const auto fromBridge = [&server] {
        const QJsonObject request{{QStringLiteral("cmd"), QStringLiteral("get")},
                                 {QStringLiteral("model"), QStringLiteral("radio")}};
        const QJsonObject reply = AutomationServerTestAccess::handleLine(
            server, QJsonDocument(request).toJson(QJsonDocument::Compact));
        check(reply.value(QStringLiteral("ok")).toBool(), "bridge accepts direct radio get");
        return reply.value(QStringLiteral("radio")).toObject();
    };
    const auto expectState = [&](const char* expected, bool attempt) {
        check(!radio.isConnected(), "no live connection is created");
        check(radio.isConnectAttemptInFlight() == attempt, "attempt input has expected state");
        check(radio.connectState() == QLatin1String(expected), "model maps the attempt state");
        const QJsonObject snapshot = fromBridge();
        check(snapshot.contains(QStringLiteral("connected"))
                  && !snapshot.value(QStringLiteral("connected")).toBool(),
              "bridge preserves the existing disconnected bool");
        check(snapshot.value(QStringLiteral("connectState")).toString() == QLatin1String(expected),
              "bridge exposes the model's connect state");
        check(!RadioModelWakeTestAccess::hasTransportOrRetry(radio),
              "no backend, connection or reconnect timer exists");
    };

    expectState("idle", false);

    // Model state injection reaches the actual production mapping. In
    // particular, an attempt remains connecting without a DSP phase or link.
    RadioModelWakeTestAccess::beginAttempt(radio);
    expectState("connecting", true);

    radio.disconnectFromRadio();
    expectState("idle", false);

    // A failed attempt also ends without a link. Calling its production error
    // handler catches a lost clear; an empty endpoint cannot schedule a retry.
    RadioModelWakeTestAccess::beginAttempt(radio);
    expectState("connecting", true);
    RadioModelWakeTestAccess::failAttempt(radio);
    expectState("idle", false);

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
