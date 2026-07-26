// Ownership rules for the automation TX watchdog (#3646).
//
// The watchdog is a runaway-script backstop, not an operator transmit limit, so
// it must police only transmissions the bridge itself started. Two ways that
// goes wrong, one test each:
//
//   1. It polls the transmit model, so with the bridge enabled — a persisted
//      setting, therefore on in ordinary sessions — the operator's own MOX and
//      TUNE were force-unkeyed at exactly 20 s, mid-sentence. Fixed on main by
//      m_txBridgeInitiated.
//   2. A TX-capable bridge action that runs while an unrelated transmission is
//      already up claims that transmission, because the claim is made after the
//      action was issued and the key verbs update TransmitModel optimistically.
//      Fixed here by sampling the transmitter before dispatch (#4435 review).
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStringList>

#include <algorithm>
#include <cstdio>

using AetherSDR::AutomationServer;
using AetherSDR::RadioModel;

namespace AetherSDR {

class AutomationServerTestAccess
{
public:
    static void setMaxKeyMs(AutomationServer& server, int value)
    {
        server.m_txMaxKeyMs = value;
    }
    // Stand in for handleLine()'s pre-dispatch sample, so a test can express
    // "a bridge request arrived while the radio was already keyed" without a
    // socket round-trip.
    static void setKeyedAtRequestStart(AutomationServer& server, bool keyed)
    {
        server.m_txKeyedAtRequestStart = keyed;
    }
    static void markTxBridgeInitiated(AutomationServer& server)
    {
        server.markTxBridgeInitiated();
    }
    static bool bridgeInitiated(const AutomationServer& server)
    {
        return server.m_txBridgeInitiated;
    }
};

} // namespace AetherSDR

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

// Pump the event loop so the 500 ms watchdog timer gets to run.
void pump(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

bool sawForceUnkey(const QStringList& commands)
{
    return std::ranges::any_of(commands, [](const QString& command) {
        return command == QStringLiteral("xmit 0")
            || command == QStringLiteral("transmit tune 0");
    });
}

// (1) TX permission enabled, but a local feature (WSPR in production) keys
// without any bridge command. The bridge must leave it alone.
void testEnabledBridgeDoesNotUnkeyManualTransmit()
{
    RadioModel radio;
    QStringList commands;
    QObject::connect(&radio.transmitModel(),
                     &AetherSDR::TransmitModel::commandReady,
                     [&commands](const QString& command) {
                         commands.push_back(command);
                     });

    AutomationServer server;
    server.setRadioModel(&radio);
    AetherSDR::AutomationServerTestAccess::setMaxKeyMs(server, 100);
    server.setTxAllowed(true);

    radio.transmitModel().setTransmitting(true);
    pump(1'200);
    server.setTxAllowed(false);

    check(!sawForceUnkey(commands),
          "enabled bridge does not force-unkey manual TX it never started");
}

// (2) A TX-capable bridge action arms while an unrelated transmission is
// already up. It must not adopt it — otherwise the operator's (or the WSPR
// beacon's) transmission dies at m_txMaxKeyMs.
void testBridgeActionDoesNotAdoptPreExistingTransmit()
{
    RadioModel radio;
    QStringList commands;
    QObject::connect(&radio.transmitModel(),
                     &AetherSDR::TransmitModel::commandReady,
                     [&commands](const QString& command) {
                         commands.push_back(command);
                     });

    AutomationServer server;
    server.setRadioModel(&radio);
    AetherSDR::AutomationServerTestAccess::setMaxKeyMs(server, 100);
    server.setTxAllowed(true);

    // Something else is already transmitting when the request arrives.
    radio.transmitModel().setTransmitting(true);
    AetherSDR::AutomationServerTestAccess::setKeyedAtRequestStart(server, true);
    AetherSDR::AutomationServerTestAccess::markTxBridgeInitiated(server);

    check(!AetherSDR::AutomationServerTestAccess::bridgeInitiated(server),
          "a transmission that predates the request is not claimed");

    commands.clear();
    pump(1'200);   // well past the 100 ms limit
    check(!sawForceUnkey(commands),
          "the unclaimed transmission is not force-unkeyed at the limit");

    server.setTxAllowed(false);
    check(!sawForceUnkey(commands),
          "revoking TX permission does not end a transmission the bridge "
          "never started");
}

// The ordinary case must still be policed: an idle radio, then a bridge action
// that keys it.
void testBridgeActionOnIdleRadioIsPoliced()
{
    RadioModel radio;
    QStringList commands;
    QObject::connect(&radio.transmitModel(),
                     &AetherSDR::TransmitModel::commandReady,
                     [&commands](const QString& command) {
                         commands.push_back(command);
                     });

    AutomationServer server;
    server.setRadioModel(&radio);
    AetherSDR::AutomationServerTestAccess::setMaxKeyMs(server, 100);
    server.setTxAllowed(true);

    // Idle at request time; the action itself keys the radio.
    AetherSDR::AutomationServerTestAccess::setKeyedAtRequestStart(server, false);
    radio.transmitModel().setTransmitting(true);
    AetherSDR::AutomationServerTestAccess::markTxBridgeInitiated(server);

    check(AetherSDR::AutomationServerTestAccess::bridgeInitiated(server),
          "a transmission this request started is claimed");

    commands.clear();
    pump(1'200);
    check(sawForceUnkey(commands),
          "a bridge-started transmission is force-unkeyed past the limit");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testEnabledBridgeDoesNotUnkeyManualTransmit();
    testBridgeActionDoesNotAdoptPreExistingTransmit();
    testBridgeActionOnIdleRadioIsPoliced();
    if (failures == 0) {
        std::puts("Automation TX watchdog tests passed");
    }
    return failures == 0 ? 0 : 1;
}
