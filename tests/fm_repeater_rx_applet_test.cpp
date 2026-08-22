#include "TestSettingsProfile.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/SliceDelta.h"
#include "gui/RxApplet.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>

#include <cstdio>

using namespace AetherSDR;

namespace {

int gFailures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++gFailures;
    }
}

template<typename T>
T* child(RxApplet& applet, const char* name)
{
    T* widget = applet.findChild<T*>(QString::fromLatin1(name));
    check(widget != nullptr, name);
    return widget;
}

void process()
{
    QApplication::processEvents(QEventLoop::AllEvents, 20);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-fm-repeater-rx-applet-test"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication application(argc, argv);

    SliceModel slice(0);
    SliceDelta initial;
    initial.mode = QStringLiteral("FM");
    initial.frequency = 144.2;
    initial.repeaterOffsetDir = QStringLiteral("up");
    initial.fmRepeaterOffsetFreq = 0.6;
    slice.applyChanges(initial);

    RxApplet applet;
    applet.resize(420, 900);
    applet.setSlice(&slice);
    applet.setFmRepeaterCapabilities(FmRepeaterPresentation::Legacy, {}, false, false);
    applet.show();
    process();

    QWidget* container = child<QWidget>(applet, "FmRepeaterContainer");
    QComboBox* access = child<QComboBox>(applet, "FmRepeaterAccessMode");
    QComboBox* txTone = child<QComboBox>(applet, "FmRepeaterTxCtcss");
    QComboBox* rxTone = child<QComboBox>(applet, "FmRepeaterRxCtcss");
    QComboBox* dtcs = child<QComboBox>(applet, "FmRepeaterDtcsCode");
    QDoubleSpinBox* offset = child<QDoubleSpinBox>(applet, "FmRepeaterOffset");
    QPushButton* down = child<QPushButton>(applet, "FmRepeaterDuplexDown");
    QPushButton* simplex = child<QPushButton>(applet, "FmRepeaterSimplex");
    QPushButton* up = child<QPushButton>(applet, "FmRepeaterDuplexUp");
    QPushButton* reverse = child<QPushButton>(applet, "FmRepeaterReverse");
    if (gFailures != 0) {
        return gFailures;
    }

    check(container->isVisible(), "Flex FM repeater surface remains visible");
    check(access->count() == 2
              && access->itemData(0).toString() == QStringLiteral("off")
              && access->itemData(1).toString() == QStringLiteral("ctcss_tx"),
          "Flex exposes exactly Off and CTCSS TX");
    check(txTone->count() == 41 && txTone->itemText(0) == QStringLiteral("1 XZ 67.0")
              && txTone->itemText(40) == QStringLiteral("41 0Z 254.1"),
          "Flex keeps the canonical 41-entry CTCSS presentation");
    check(!rxTone->isVisible() && !dtcs->isVisible(),
          "Flex does not render the extended Icom fields");
    check(down->text() == QString::fromUtf8("\xe2\x88\x92")
              && simplex->text() == QStringLiteral("Simplex")
              && up->text() == QStringLiteral("+")
              && reverse->text() == QStringLiteral("REV")
              && down->x() < simplex->x() && simplex->x() < up->x()
              && up->x() < reverse->x(),
          "Flex direction controls retain exact labels and visual order");

    QSignalSpy commandSpy(&slice, &SliceModel::commandReady);
    reverse->click();
    process();
    check(!commandSpy.isEmpty()
              && commandSpy.last().at(0).toString()
                  == QStringLiteral("slice set 0 tx_offset_freq=-0.600000"),
          "the real Flex REV handler retains its sign-flip wire behavior");

    SliceDelta pttTruth;
    pttTruth.txOffsetFreq = 0.6;
    slice.applyChanges(pttTruth);
    process();
    QLabel* frequency = nullptr;
    for (QLabel* label : applet.findChildren<QLabel*>()) {
        if (label->accessibleName() == QStringLiteral("Frequency display")) {
            frequency = label;
            break;
        }
    }
    check(frequency && frequency->text() == QStringLiteral("144.200.000")
              && slice.txOffsetFreq() == 0.6,
          "confirmed TX offset does not corrupt the RX-frequency presentation");

    applet.setFmRepeaterCapabilities(FmRepeaterPresentation::Hidden, {}, false, false);
    process();
    check(!container->isVisible(), "an unattested Icom repeater surface is hidden");

    applet.setFmRepeaterCapabilities(
        FmRepeaterPresentation::Extended,
        {QStringLiteral("off"), QStringLiteral("ctcss_tx"),
         QStringLiteral("ctcss_txrx"), QStringLiteral("dtcs_txrx")},
        true, true);
    SliceDelta simplexTruth;
    simplexTruth.repeaterOffsetDir = QStringLiteral("simplex");
    slice.applyChanges(simplexTruth);
    process();
    check(container->isVisible() && access->count() == 4,
          "the evidenced IC-9700 profile renders only its declared access modes");
    check(reverse->isVisible() && !reverse->isEnabled(),
          "IC-9700 REV stays visible but inapplicable while simplex is authoritative");
    check(offset->maximum() == 99.999 && !offset->keyboardTracking(),
          "IC-9700 offset entry matches the wire limit and commits once per edit");

    QSignalSpy offsetRequest(&slice, &SliceModel::fmRepeaterOffsetCommandIssued);
    down->click();
    process();
    check(offsetRequest.count() == 1
              && offsetRequest.first().at(0).toString() == QStringLiteral("down"),
          "direction click emits one IC-9700 offset request");
    check(simplex->isChecked() && !down->isChecked(),
          "direction buttons retain radio-authoritative truth until readback");

    return gFailures == 0 ? 0 : 1;
}
