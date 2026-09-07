// DEXP is a radio-side compander control, so the Phone applet may surface it
// only when the connected backend declares an authoritative command path.

#include "TestSettingsProfile.h"
#include "gui/PhoneApplet.h"
#include "models/TransmitModel.h"
#include <QSlider>
#include <QSignalSpy>

#include <QApplication>
#include <QFile>
#include <QWidget>

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
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-phone-applet-dexp-visibility-test"));
    QApplication app(argc, argv);
    PhoneApplet applet;

    QWidget* dexpRow =
        applet.findChild<QWidget*>(QStringLiteral("phoneDexpRow"));
    check(dexpRow != nullptr, "the Phone applet exposes one DEXP row");
    if (!dexpRow) {
        return failures;
    }

    check(!dexpRow->isHidden(),
          "the disconnected applet keeps the permissive DEXP surface");

    applet.setDexpVisible(false);
    check(dexpRow->isHidden(),
          "an unsupported connected backend hides the complete DEXP row");

    applet.setDexpVisible(true);
    check(!dexpRow->isHidden(),
          "a supporting backend restores the existing DEXP row");

    TransmitModel tx;
    applet.setTransmitModel(&tx);
    QSignalSpy commands(&tx, &TransmitModel::commandReady);
    QSlider* carrier = nullptr;
    QSlider* delay = nullptr;
    for (QSlider* slider : applet.findChildren<QSlider*>()) {
        if (slider->accessibleName() == QStringLiteral("AM carrier level")) {
            carrier = slider;
        } else if (slider->accessibleName() == QStringLiteral("VOX delay")) {
            delay = slider;
        }
    }
    check(carrier && delay, "AM carrier and VOX delay have accessible controls");
    if (!carrier || !delay) {
        return 1;
    }
    applet.setAmCarrierAvailable(false);
    applet.setVoxDelayAvailable(false);
    check(!carrier->isEnabled() && !delay->isEnabled(), "unsupported controls are disabled");
    carrier->setValue(37);
    delay->setValue(43);
    check(commands.isEmpty(), "programmatic changes under gates emit no command");
    applet.setAmCarrierAvailable(true);
    applet.setVoxDelayAvailable(true);
    check(carrier->isEnabled() && delay->isEnabled(), "capable session restores both controls");
    carrier->setValue(38);
    delay->setValue(44);
    check(commands.count() == 2, "restored controls each emit their command");

    // Pin the production edge as well as the widget setter. This deliberately
    // reads the owning fan-out because this small target does not link the
    // whole MainWindow; deleting the capability-to-widget call must still fail
    // a test rather than leaving only physical-radio coverage.
    QFile mainWindowSource(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow.cpp"));
    check(mainWindowSource.open(QIODevice::ReadOnly),
          "the capability test can inspect MainWindow's shipping fan-out");
    const QByteArray wiring = mainWindowSource.readAll();
    check(wiring.contains(
              "phone->setDexpVisible(!connected || caps.hasDownwardExpander);"),
          "MainWindow wires hasDownwardExpander to the complete DEXP row");

    return failures == 0 ? 0 : 1;
}
