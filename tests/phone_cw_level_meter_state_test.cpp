// The Phone/CW microphone Level gauge must remain empty until live telemetry
// arrives and must not carry a prior radio's reading across disconnect.

#include "TestSettingsProfile.h"
#include "gui/HGauge.h"
#include "gui/PhoneCwApplet.h"

#include <QApplication>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("AETHER_AUTOMATION", "1");
    TestSettingsProfile profile(QStringLiteral("phone-cw-level-meter-state-test"));
    QApplication app(argc, argv);
    PhoneCwApplet applet;

    QWidget* levelWidget =
        applet.findChild<QWidget*>(QStringLiteral("phoneMicLevelGauge"));
    check(levelWidget != nullptr, "Phone/CW exposes the microphone Level gauge");
    if (!levelWidget) {
        return failures;
    }
    auto* levelGauge = static_cast<HGauge*>(levelWidget);

    check(levelGauge->value() == -40.0f,
          "the disconnected Level gauge starts at its floor");
    check(levelGauge->filledFraction() == 0.0f,
          "the disconnected Level gauge starts visually empty");

    applet.updateMeters(-12.0f, 0.0f, -8.0f, 0.0f);
    check(levelGauge->value() == -12.0f,
          "a live microphone sample reaches the Level gauge");

    applet.setMicLevelMeterState(MicMeterSessionState::Disconnected, false);
    check(!levelGauge->isHidden(),
          "disconnect keeps the permissive Level surface visible");
    check(levelGauge->value() == -40.0f,
          "disconnect clears the previous radio's Level reading");
    check(levelGauge->filledFraction() == 0.0f,
          "disconnect snaps the Level gauge back to empty");

    applet.setMicLevelMeterState(MicMeterSessionState::Connected, true);
    check(!levelGauge->isHidden(),
          "a later capable radio exposes the Level gauge");
    check(levelGauge->value() == -40.0f,
          "a later capable radio waits for its own microphone sample");

    applet.updateMeters(-18.0f, 0.0f, -14.0f, 0.0f);
    check(levelGauge->value() == -18.0f,
          "the later radio's own sample updates the Level gauge");

    applet.setMicLevelMeterState(MicMeterSessionState::Connected, false);
    check(levelGauge->isHidden(),
          "a connected radio without a microphone meter hides the gauge");
    check(levelGauge->value() == -40.0f,
          "hiding an unsupported gauge also clears its stale reading");

    applet.setMicLevelMeterState(MicMeterSessionState::Disconnected, false);
    applet.updateMeters(-16.0f, 0.0f, -12.0f, 0.0f);
    applet.setMicLevelMeterState(MicMeterSessionState::Disconnected, false);
    check(levelGauge->value() == -16.0f,
          "a repeated disconnected state preserves live PC-mic telemetry");

    return failures == 0 ? 0 : 1;
}
