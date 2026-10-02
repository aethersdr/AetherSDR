// Regression test for QtAudioBackendGuard: Qt 6.12's QtMultimedia segfaults in
// QPlatformAudioDevices::create() when it picks its PipeWire backend and cannot
// create a PipeWire client context. The guard must detect that before Qt does
// and select QT_AUDIO_BACKEND=pulseaudio, and must never override a backend the
// user chose.
//
// One executable, one scenario per process (QT_AUDIO_BACKEND and PipeWire's
// library state are both per-process), registered twice in tests.cmake:
//   qt_audio_backend_guard_no_config    PipeWire pointed at an empty config dir
//   qt_audio_backend_guard_user_choice  same, with QT_AUDIO_BACKEND preset
// Socket-free: no PipeWire daemon is contacted — context creation fails on the
// missing configuration before any connection. Exits 77 (skip) when libpipewire
// is not installed, since then there is no PipeWire path for Qt to crash on.
//
// Run: ./build/qt_audio_backend_guard_test no-config

#include "core/QtAudioBackendGuard.h"

#include <QAudioDevice>
#include <QCoreApplication>
#include <QMediaDevices>

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>

using AetherSDR::QtAudioBackendGuard;

namespace {

int g_failed = 0;

void check(bool ok, const std::string& what, const std::string& detail = {})
{
    std::printf("%s %-62s %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str(), detail.c_str());
    if (!ok)
        ++g_failed;
}

} // namespace

int main(int argc, char* argv[])
{
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode != "no-config" && mode != "user-choice") {
        std::fprintf(stderr, "usage: %s no-config|user-choice\n", argv[0]);
        return 2;
    }
    if (!dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL)) {
        std::printf("libpipewire not installed: nothing for Qt to crash on, skipping\n");
        return 77;
    }

    const QByteArray before = qgetenv("QT_AUDIO_BACKEND");
    QtAudioBackendGuard::applyAtStartup();
    const QByteArray after = qgetenv("QT_AUDIO_BACKEND");
    const QString summary = QtAudioBackendGuard::appliedSummary();

    if (mode == "no-config") {
        check(before.isEmpty(), "precondition: QT_AUDIO_BACKEND unset");
        check(after == "pulseaudio", "unusable PipeWire context selects pulseaudio",
              after.toStdString());
        check(summary.contains(QStringLiteral("could not be created")),
              "summary names the reason", summary.toStdString());
    } else {
        check(!before.isEmpty(), "precondition: QT_AUDIO_BACKEND preset");
        check(after == before, "a user-chosen backend is never overridden",
              after.toStdString());
        check(summary.contains(QStringLiteral("set by the user")),
              "summary says it was the user's choice", summary.toStdString());
    }

    // The point of the guard: with the environment it leaves behind, Qt's audio
    // device enumeration must survive. Without the guard this call segfaults
    // inside QtMultimedia in the no-config scenario.
    QCoreApplication app(argc, argv);
    const QAudioDevice out = QMediaDevices::defaultAudioOutput();
    check(true, "QMediaDevices::defaultAudioOutput() returned",
          out.isNull() ? "(no device)" : out.description().toStdString());

    std::printf("%d failure(s)\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
