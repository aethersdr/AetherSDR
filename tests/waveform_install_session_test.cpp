// A Remote Access download or a picked waveform file installs only to the
// radio it was chosen for: radioStillChosen() refuses once the same
// RadioModel has moved to another control session (a reconnect, possibly to
// a different radio), has lost the radio, or is gone. Socket-free: a
// recording backend stands in for the connection.

#include "RadioSetupControlFixture.h"
#include "TestSettingsProfile.h"
#include "gui/WaveformInstallGate.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QPointer>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void check(bool ok, const char* label)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("waveform-install-session-test"));
    QCoreApplication app(argc, argv);

    auto radio = std::make_unique<RadioModel>();
    RadioSetupTestBackend* backend = installRadioSetupBackend(*radio);
    const QPointer<RadioModel> guard(radio.get());
    const RadioControlSession chosen = radio->controlSession();
    check(radioStillChosen(guard, chosen) == radio.get(),
          "same session: installs to the radio it was chosen for");

    RadioSetupControlTestAccess::replaceConnectionGeneration(*radio);
    check(radioStillChosen(guard, chosen) == nullptr,
          "after a reconnect the old download is refused");
    check(radioStillChosen(guard, radio->controlSession()) == radio.get(),
          "a download chosen in the new session installs");

    backend->live = false;
    check(radioStillChosen(guard, radio->controlSession()) == nullptr,
          "a disconnected radio is refused");
    backend->live = true;

    const RadioControlSession beforeRetire = radio->controlSession();
    RadioSetupControlTestAccess::retireBackend(*radio);
    if (!(radio->controlSession() == beforeRetire)) {
        check(radioStillChosen(guard, beforeRetire) == nullptr,
              "after a backend change the old download is refused");
    }

    const RadioControlSession last = radio->controlSession();
    radio.reset();
    check(radioStillChosen(guard, last) == nullptr, "a deleted model is refused");

    if (g_failed == 0) {
        std::printf("\nAll waveform-install-session tests passed.\n");
        return 0;
    }
    std::printf("\n%d waveform-install-session tests failed.\n", g_failed);
    return 1;
}
