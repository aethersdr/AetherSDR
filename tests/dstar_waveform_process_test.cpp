// Standalone test harness for DStarWaveformProcess lifecycle helpers.
//
// Build: produced by CMake as `dstar_waveform_process_test`.
// Run:   ./build/dstar_waveform_process_test
// Exit:  0 = pass, 1 = fail.

#include "core/DStarWaveformProcess.h"
#include "core/DStarWaveformSettings.h"

#include <QCoreApplication>
#include <QHostAddress>

#include <cstdio>

using AetherSDR::DStarWaveformProcess;
using AetherSDR::DStarWaveformSettings;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = {})
{
    std::printf("%s %-60s %s\n", ok ? "[ OK ]" : "[FAIL]", name,
                detail.toUtf8().constData());
    if (!ok) {
        ++g_failed;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    report("stateName: stopped",
           DStarWaveformProcess::stateName(DStarWaveformProcess::State::Stopped)
               == QStringLiteral("Stopped"));
    report("stateName: failed",
           DStarWaveformProcess::stateName(DStarWaveformProcess::State::Failed)
               == QStringLiteral("Failed"));

    report("resolveExecutablePath: trims configured path",
           DStarWaveformProcess::resolveExecutablePath(QStringLiteral("  /tmp/aether-dstar-waveform  "))
               == QStringLiteral("/tmp/aether-dstar-waveform"));
    report("resolveExecutablePath: empty falls back to default",
           !DStarWaveformProcess::resolveExecutablePath(QString()).isEmpty());

    report("backendFromString: empty uses ThumbDV backend",
           DStarWaveformSettings::backendFromString(QString())
               == DStarWaveformSettings::Backend::ThumbDv);
    report("backendFromString: ThumbDV maps to serial backend",
           DStarWaveformSettings::backendFromString(QStringLiteral("ThumbDV"))
               == DStarWaveformSettings::Backend::ThumbDv);
    report("backendRequiresSerial: ThumbDV requires serial",
           DStarWaveformSettings::backendRequiresSerial(
               DStarWaveformSettings::Backend::ThumbDv));
    report("backendArgument: ThumbDV CLI argument",
           DStarWaveformSettings::backendArgument(
               DStarWaveformSettings::Backend::ThumbDv)
               == QStringLiteral("thumbdv"));

    DStarWaveformProcess& process = DStarWaveformProcess::instance();
    process.stop();
    const bool started = process.startForRadio(QHostAddress());
    report("startForRadio: null radio fails", !started);
    report("startForRadio: null radio sets failed state",
           process.state() == DStarWaveformProcess::State::Failed);
    report("startForRadio: null radio stores useful error",
           process.lastError().contains(QStringLiteral("No connected radio address")));
    process.stop();
    report("stop: failed/not-running resets state",
           process.state() == DStarWaveformProcess::State::Stopped);

    if (g_failed == 0) {
        std::printf("\nAll DStarWaveformProcess tests passed.\n");
    } else {
        std::printf("\n%d test(s) failed.\n", g_failed);
    }
    return g_failed == 0 ? 0 : 1;
}
