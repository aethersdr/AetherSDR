#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/SerialOpenError.h"
#include "core/SerialPortController.h"

#include <QCoreApplication>
#include <QDir>
#include <QString>

#include <cstdio>
#include <vector>

using namespace AetherSDR;

namespace {

struct State {
    bool open;
    QString device;
    QString error;
    int kind;
};

int failures = 0;

void check(bool passed, const char* message)
{
    if (!passed) {
        std::fprintf(stderr, "serial_open_state_test: %s\n", message);
        ++failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    const TestSettingsProfile profile(QStringLiteral("serial-open-state"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "serial_open_state_test: could not isolate settings\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    const QString missing = QDir(profile.path()).filePath(QStringLiteral("missing/ttyUSB99"));
    std::vector<State> observed;
    SerialPortController controller;
    QObject::connect(&controller, &SerialPortController::connectionStateChanged, &app,
                     [&observed](bool open, const QString& device, const QString& error,
                                 int, int kind, const QString&) {
        observed.push_back({open, device, error, kind});
    });

    auto& settings = AppSettings::instance();
    settings.setValue("SerialPortName", missing);
    settings.setValue("SerialAutoOpen", "True");
    settings.setValue("SerialPortOpen", "False");
    check(controller.loadSettings(), "Auto-open alone must not attempt an open");
    check(observed.empty(), "Auto-open alone must leave the controller closed");

    settings.setValue("SerialPortOpen", "True");
    check(!controller.loadSettings(), "a missing port must fail the session request");
    check(observed.size() == 1, "one failed open must publish one state update");
    if (observed.size() == 1) {
        const State& state = observed.front();
        check(!state.open && !controller.isOpen(), "a failed port must be closed");
        check(state.device == missing, "the failure must identify the full device path");
        check(!state.error.isEmpty(), "the failure must carry the OS error");
        check(state.kind == static_cast<int>(SerialOpenFailureKind::NotFound),
              "a missing device must be classified as NotFound");
    }

    return failures == 0 ? 0 : 1;
}
