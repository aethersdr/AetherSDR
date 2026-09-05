// `radio.connectState`, through the MODEL and the BRIDGE — not through the
// policy header (#5416 review).
//
// The policy in ConnectStatePolicy.h is pure and covered by
// connect_state_policy_test. That test cannot fail for the defect that mattered:
// the reviewer mutated RadioModel's production wiring so the input was never
// set, and every policy check stayed green. A pure function's test proves the
// decision, never that the caller feeds it the right lifecycle.
//
// So this one asserts the mapping RadioModel actually performs, and that its
// answer reaches `radioSnapshot()`. The scenario is the whole point of the
// field (#5413 item 3): after `connectToRadio()` and before any link,
// `connected` is false — exactly as it is when nothing is happening at all —
// and only `connectState` separates the two.
//
// WHAT THIS OPENS, stated exactly rather than as "socket-free". The bridge half
// really is: the line dispatcher is called directly through the existing test
// friend, so no QLocalServer is created and nothing binds. The model half is
// not. connectToRadio() builds the family's backend, and that backend opens an
// outbound TCP connection — the first draft of this file pointed it at
// 127.0.0.1:4992 and the log showed it dialling, which on a developer's machine
// could have reached a real SmartSDR listener.
//
// So the target is 192.0.2.1, TEST-NET-1 (RFC 5737), which is reserved for
// documentation and is not routable. The connect attempt cannot reach anything,
// on this machine or off it. Nothing is bound, no discovery runs, no radio is
// contacted, and nothing here can key a transmitter.
//
// The attempt flag is set synchronously inside connectToRadio(), before any of
// that I/O could succeed, which is why an unreachable target is enough.

#include "TestSettingsProfile.h"
// AutomationServer holds QPointers to these; the complete types are needed for
// its members to instantiate, as in the sibling automation tests.
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

namespace AetherSDR {

// handleLine() is private; the header already befriends this name for the
// sibling automation tests.
class AutomationServerTestAccess
{
public:
    static QJsonObject handleLine(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};

}  // namespace AetherSDR

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-connect-state-model-test"));

    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);

    check(profile.isValid(), "isolated settings profile is available");

    AudioEngine engine;
    RadioModel radio;
    AutomationServer server;
    server.setAudioEngine(&engine);
    server.setRadioModel(&radio);

    // What the wire says, read the way a client reads it.
    const auto fromBridge = [&server] {
        const QJsonObject request{{QStringLiteral("cmd"), QStringLiteral("get")},
                                  {QStringLiteral("model"), QStringLiteral("radio")}};
        const QJsonObject reply = AutomationServerTestAccess::handleLine(
            server, QJsonDocument(request).toJson(QJsonDocument::Compact));
        return reply.value(QStringLiteral("radio"))
            .toObject()
            .value(QStringLiteral("connectState"))
            .toString();
    };

    // ---- Nothing happening ------------------------------------------------
    check(!radio.isConnected() && !radio.isConnectAttemptInFlight(),
          "a fresh model has no link and no attempt");
    check(radio.connectState() == QLatin1String("idle"),
          "and reports idle");
    check(fromBridge() == QLatin1String("idle"),
          "the bridge snapshot carries the field — it exists on the wire");

    // ---- THE WINDOW THE FIELD EXISTS FOR ----------------------------------
    //
    // `connected` is false here and false above. Nothing else in the snapshot
    // separates a connect that is working from one that is not happening, which
    // is the whole of #5413 item 3.
    {
        RadioInfo info;
        info.family = QStringLiteral("sim");
        info.name = QStringLiteral("Demo");
        info.serial = QStringLiteral("SIM-0001");
        info.address = QHostAddress(QStringLiteral("192.0.2.1"));   // TEST-NET-1
        radio.connectToRadio(info);

        check(radio.isConnectAttemptInFlight(),
              "connectToRadio() marks the attempt at the request edge (#4912)");
        check(radio.connectState() == QLatin1String("connecting"),
              "an attempt in flight reads connecting, NOT idle");
        check(fromBridge() == QLatin1String("connecting"),
              "and connecting is what the bridge reports");
    }

    // ---- Cancellation returns to idle, immediately -------------------------
    //
    // The inverted edge the review found in the first version: a DSP-only flag
    // could stay true after the operator had left, because the build it tracked
    // cannot be cancelled and the reset it relied on was not guaranteed to run.
    // The attempt flag is cleared by disconnectFromRadio() itself.
    {
        radio.disconnectFromRadio();
        check(!radio.isConnectAttemptInFlight(),
              "abandoning the connect clears the attempt");
        check(radio.connectState() == QLatin1String("idle"),
              "and the state returns to idle rather than sticking at connecting");
        check(fromBridge() == QLatin1String("idle"),
              "the bridge agrees — no stale connecting for a session nobody is in");
    }

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
