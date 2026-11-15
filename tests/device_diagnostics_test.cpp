// Standalone test harness for portable device diagnostics helpers.
// Run:   ./build/device_diagnostics_test

#include "core/DeviceDiagnostics.h"

#include <cstdio>
#include <string>

using AetherSDR::DeviceDiagnostics::inferAudioBusType;
using AetherSDR::DeviceDiagnostics::TxAudioResamplingRoute;
using AetherSDR::DeviceDiagnostics::txAudioResamplingRoute;

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

    const TxAudioResamplingRoute nativeVoice = txAudioResamplingRoute(
        false, false, false, false);
    report("48 kHz voice reports its egress SRC",
           nativeVoice.active
               && !nativeVoice.voiceInputNormalizingTo48k
               && nativeVoice.voiceEgressResamplingTo24k
               && !nativeVoice.radeResamplingTo24k);

    const TxAudioResamplingRoute normalizedVoice = txAudioResamplingRoute(
        false, false, true, true);
    report("non-48 kHz voice reports both serial SRCs",
           normalizedVoice.active
               && normalizedVoice.voiceInputNormalizingTo48k
               && normalizedVoice.voiceEgressResamplingTo24k
               && !normalizedVoice.radeResamplingTo24k);

    const TxAudioResamplingRoute nativeRade = txAudioResamplingRoute(
        true, false, true, false);
    report("native-rate RADE reports no active SRC",
           !nativeRade.active
               && !nativeRade.voiceInputNormalizingTo48k
               && !nativeRade.voiceEgressResamplingTo24k
               && !nativeRade.radeResamplingTo24k);

    const TxAudioResamplingRoute resampledRade = txAudioResamplingRoute(
        true, true, true, true);
    report("RADE route priority reports only its active SRC",
           resampledRade.active
               && !resampledRade.voiceInputNormalizingTo48k
               && !resampledRade.voiceEgressResamplingTo24k
               && resampledRade.radeResamplingTo24k);

    const TxAudioResamplingRoute daxOnly = txAudioResamplingRoute(
        false, true, true, true);
    report("DAX mic bypass reports no SRC",
           !daxOnly.active
               && !daxOnly.voiceInputNormalizingTo48k
               && !daxOnly.voiceEgressResamplingTo24k
               && !daxOnly.radeResamplingTo24k);

    std::printf("\n%s\n", g_failed == 0 ? "All tests passed." : "Some tests failed.");
    return g_failed == 0 ? 0 : 1;
}
