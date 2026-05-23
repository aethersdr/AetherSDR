// Standalone test harness for portable device diagnostics helpers.
// Run:   ./build/device_diagnostics_test

#include "core/DeviceDiagnostics.h"

#include <cstdio>
#include <string>

using MasterSDR::DeviceDiagnostics::inferAudioBusType;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-48s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

void expectBus(const char* name, const QString& description, const QByteArray& id, const QString& expected)
{
    const auto result = inferAudioBusType(description, id);
    report(name,
           result.type == expected,
           QString("got=%1 expected=%2 source=%3")
               .arg(result.type, expected, result.source)
               .toStdString());
}

} // namespace

int main()
{
    std::printf("Device diagnostics tests\n\n");

    expectBus("USB from device name",
              QStringLiteral("USB Audio CODEC"),
              QByteArray(),
              QStringLiteral("USB"));
    expectBus("USB from opaque id",
              QStringLiteral("External Speaker"),
              QByteArray("swdevice\\mmdevapi\\usb#vid_1234&pid_abcd"),
              QStringLiteral("USB"));
    expectBus("Bluetooth from device name",
              QStringLiteral("AirPods Pro"),
              QByteArray(),
              QStringLiteral("Bluetooth"));
    expectBus("Bluetooth from BT token",
              QStringLiteral("JBL BT Speaker"),
              QByteArray(),
              QStringLiteral("Bluetooth"));
    expectBus("Bluetooth from id",
              QStringLiteral("Headphones"),
              QByteArray("BTHENUM\\Dev_123456"),
              QStringLiteral("Bluetooth"));
    expectBus("Unknown when Qt gives no transport hint",
              QStringLiteral("Built-in Output"),
              QByteArray("coreaudio-default-output"),
              QStringLiteral("Unknown"));

    std::printf("\n%s\n", g_failed == 0 ? "All tests passed." : "Some tests failed.");
    return g_failed == 0 ? 0 : 1;
}
