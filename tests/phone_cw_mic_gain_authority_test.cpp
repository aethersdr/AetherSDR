#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/TransmitDelta.h"
#include "gui/PhoneCwApplet.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QSignalSpy>
#include <QSlider>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* label)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", label);
    if (!condition)
        ++failures;
}

QSlider* micSlider(PhoneCwApplet& applet)
{
    for (QSlider* slider : applet.findChildren<QSlider*>()) {
        if (slider->accessibleName() == QLatin1String("Microphone gain"))
            return slider;
    }
    return nullptr;
}

void reportMicLevel(TransmitModel& model, int level)
{
    TransmitDelta d;
    d.micLevel = level;
    model.applyChanges(d);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("phone-cw-mic-gain-authority-test"));
    QApplication app(argc, argv);
    AppSettings::instance().load();
    AppSettings::instance().setValue(QStringLiteral("PcMicGain"), 73);
    AppSettings::instance().save();

    TransmitModel model;
    PhoneCwApplet applet;
    applet.setTransmitModel(&model);
    QSlider* slider = micSlider(applet);
    check(slider != nullptr, "microphone gain slider exists");
    if (!slider)
        return 1;

    QSignalSpy clientGain(&applet, &PhoneCwApplet::micLevelChanged);

    // Flex: PC is a real selectable client-audio source. Radio mic_level must
    // not overwrite PcMicGain during sync, but an operator move must reach the
    // client gain path.
    applet.setSelectableMicInputs(true);
    model.applyMicSelectionState(QStringLiteral("PC"));
    reportMicLevel(model, 21);
    check(slider->value() == 73,
          "Flex PC displays persisted PcMicGain, not radio mic_level");
    check(clientGain.isEmpty(),
          "Flex PC model sync does not write PcMicGain back through the slider");
    slider->setValue(72);
    check(clientGain.count() == 1 && clientGain.takeFirst().at(0).toInt() == 72,
          "Flex PC operator move emits client gain intent");

    // Flex hardware input: the radio owns mic_level and a client-gain write
    // would corrupt the saved PC-mic setting.
    model.applyMicSelectionState(QStringLiteral("MIC"));
    reportMicLevel(model, 34);
    check(slider->value() == 34,
          "Flex hardware input adopts radio mic_level");
    clientGain.clear();
    slider->setValue(35);
    check(clientGain.isEmpty(),
          "Flex hardware-mic move does not overwrite client PcMicGain");

    // Icom/HL2: PC is a synthetic one-item label, not proof that the client
    // owns the gain. The backend-reported level remains authoritative.
    applet.setSelectableMicInputs(false);
    reportMicLevel(model, 41);
    check(slider->value() == 41,
          "synthetic PC input adopts backend mic level");
    clientGain.clear();
    slider->setValue(42);
    check(model.micLevel() == 42,
          "synthetic PC operator move reaches the radio model");
    check(clientGain.isEmpty(),
          "synthetic PC operator move does not alter client PcMicGain");

    return failures == 0 ? 0 : 1;
}
