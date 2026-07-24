#include "core/AutomationTxWatchdog.h"
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

using AetherSDR::AutomationTxWatchdog;
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

void testManualTransmitIsNeverClaimed()
{
    AutomationTxWatchdog watchdog;
    check(watchdog.poll(true, 0, 20'000, 125'000)
              == AutomationTxWatchdog::Decision::None,
          "manual key-up is ignored without an automation lease");
    check(watchdog.poll(true, 120'000, 20'000, 125'000)
              == AutomationTxWatchdog::Decision::None,
          "manual key-up remains ignored beyond the automation timeout");
    check(!watchdog.isArmed(),
          "observing manual transmit does not create an automation lease");
}

void testAutomationTransmitTimesOut()
{
    AutomationTxWatchdog watchdog;
    watchdog.arm(1'000);
    check(watchdog.poll(true, 1'500, 20'000, 125'000)
              == AutomationTxWatchdog::Decision::None,
          "first automated keyed observation starts the key timer");
    check(watchdog.poll(true, 21'500, 20'000, 125'000)
              == AutomationTxWatchdog::Decision::None,
          "automation transmit is allowed through the configured limit");
    check(watchdog.poll(true, 21'501, 20'000, 125'000)
              == AutomationTxWatchdog::Decision::ForceUnkey,
          "automation transmit is force-unkeyed beyond the configured limit");
    check(!watchdog.isArmed(), "timed-out automation lease is released");
}

void testLeaseEndsWithTransmit()
{
    AutomationTxWatchdog watchdog;
    watchdog.arm(0);
    watchdog.poll(true, 500, 20'000, 125'000);
    watchdog.poll(false, 2'000, 20'000, 125'000);
    check(!watchdog.isArmed(),
          "automation lease ends when its observed transmission unkeys");
    check(watchdog.poll(true, 30'000, 20'000, 125'000)
              == AutomationTxWatchdog::Decision::None,
          "later manual transmit is not inherited by a completed lease");
}

void testDelayedActionLease()
{
    AutomationTxWatchdog watchdog;
    watchdog.arm(0);
    watchdog.poll(false, 119'000, 20'000, 125'000);
    check(watchdog.isArmed(),
          "pending lease survives a full WSPR slot wait");
    watchdog.poll(true, 120'000, 20'000, 125'000);
    check(watchdog.poll(true, 140'001, 20'000, 125'000)
              == AutomationTxWatchdog::Decision::ForceUnkey,
          "delayed automation key-up still receives the safety timeout");

    watchdog.arm(0);
    watchdog.poll(false, 125'001, 20'000, 125'000);
    check(!watchdog.isArmed(),
          "an abandoned pending action cannot claim a future manual transmit");
}

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

    // Reproduce the reported condition: TX permission remains enabled, but a
    // local feature (WSPR in production) keys without a bridge command.
    radio.transmitModel().setTransmitting(true);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 1'200) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    server.setTxAllowed(false);

    const bool forceUnkeySent =
        std::ranges::any_of(commands, [](const QString& command) {
            return command == QStringLiteral("xmit 0")
                || command == QStringLiteral("transmit tune 0");
        });
    check(!forceUnkeySent,
          "enabled bridge does not force-unkey manual TX without a lease");

}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testManualTransmitIsNeverClaimed();
    testAutomationTransmitTimesOut();
    testLeaseEndsWithTransmit();
    testDelayedActionLease();
    testEnabledBridgeDoesNotUnkeyManualTransmit();
    if (failures == 0) {
        std::puts("Automation TX watchdog tests passed");
    }
    return failures == 0 ? 0 : 1;
}
