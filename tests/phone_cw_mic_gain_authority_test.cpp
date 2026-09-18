#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/TransmitDelta.h"
#include "gui/PhoneCwApplet.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QAccessible>
#include <QFile>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;
QList<QObject*> g_nameChangedObjects;
QList<QObject*> g_descriptionChangedObjects;

void captureAccessibilityUpdate(QAccessibleEvent* event)
{
    if (event->type() == QAccessible::NameChanged) {
        g_nameChangedObjects.append(event->object());
    } else if (event->type() == QAccessible::DescriptionChanged) {
        g_descriptionChangedObjects.append(event->object());
    }
}

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

QSlider* processorSlider(PhoneCwApplet& applet)
{
    for (QSlider* slider : applet.findChildren<QSlider*>()) {
        if (slider->accessibleName() == QLatin1String("Processor level"))
            return slider;
    }
    return nullptr;
}

QPushButton* processorButton(PhoneCwApplet& applet)
{
    for (QPushButton* button : applet.findChildren<QPushButton*>()) {
        if (button->accessibleName() == QLatin1String("Speech processor"))
            return button;
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
    QSlider* procSlider = processorSlider(applet);
    QPushButton* procButton = processorButton(applet);
    check(slider != nullptr, "microphone gain slider exists");
    if (!slider)
        return 1;
    check(procSlider != nullptr, "speech processor slider exists");
    if (!procSlider)
        return 1;
    check(procButton != nullptr, "speech processor button exists");
    if (!procButton)
        return 1;

    check(procSlider->maximum() == 2,
          "default processor surface preserves Flex NOR/DX/DX+ range");
    QSignalSpy flexCommands(&model, &TransmitModel::commandReady);
    procSlider->setValue(2);
    check(flexCommands.count() == 1
              && flexCommands.takeFirst().at(0).toString()
                     == QStringLiteral("transmit set speech_processor_level=2"),
          "Flex DX+ retains its existing level-2 command");
    const QAccessible::UpdateHandler previousHandler =
        QAccessible::installUpdateHandler(captureAccessibilityUpdate);
    const bool wasAccessible = QAccessible::isActive();
    QAccessible::setActive(true);
    g_nameChangedObjects.clear();
    g_descriptionChangedObjects.clear();
    applet.setSpeechProcessorPresentation(QStringLiteral("COMP"), 100);
    model.setSpeechProcessorLevelMaximum(100);
    check(procSlider->maximum() == 100 && procSlider->pageStep() == 10,
          "continuous processor capability exposes the full 0..100 range");
    check(procSlider->accessibleDescription().contains(QLatin1String("compressor")),
          "IC-9700 presentation identifies the continuous control as compressor level");
    check(procButton->text() == QLatin1String("COMP")
              && procButton->accessibleName() == QLatin1String("Speech compressor"),
          "IC-9700 presentation uses the radio-native COMP label and accessible name");
    check(procButton->width() == 54
              && procButton->property("continuousCompressor").toBool(),
          "IC-9700 COMP label uses its legible model-gated presentation");
    if (QAccessible::isActive()) {
        check(g_nameChangedObjects.contains(procButton),
              "COMP selection emits NameChanged for the processor button");
        check(g_descriptionChangedObjects.contains(procButton)
                  && g_descriptionChangedObjects.contains(procSlider),
              "COMP selection emits DescriptionChanged for button and slider");
    }
    QAccessible::installUpdateHandler(previousHandler);
    QAccessible::setActive(wasAccessible);
    procSlider->setValue(50);
    check(model.speechProcessorLevel() == 50,
          "continuous processor midpoint reaches the transmit model unchanged");
    TransmitDelta procReport;
    procReport.speechProcLevel = 73;
    model.applyChanges(procReport);
    check(procSlider->value() == 73,
          "radio-reported continuous processor level returns to the slider");
    applet.setSpeechProcessorPresentation(QStringLiteral("PROC"), 2);
    model.setSpeechProcessorLevelMaximum(2);
    check(procSlider->maximum() == 2 && model.speechProcessorLevel() == 2
              && procButton->width() == 48
              && !procButton->property("continuousCompressor").toBool(),
          "three-position capability restores and bounds the legacy surface");

    QFile mainWindowSource(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow.cpp"));
    check(mainWindowSource.open(QIODevice::ReadOnly),
          "processor presentation test can inspect the shipping capability fan-out");
    const QByteArray mainWindowText = mainWindowSource.readAll();
    check(mainWindowText.contains(
              "connected ? caps.speechProcessorLabel : QStringLiteral(\"PROC\")")
              && mainWindowText.contains(
                  "connected ? caps.speechProcessorLevelMaximum : 2"),
          "MainWindow forwards the normalized processor label and range to P/CW");

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
