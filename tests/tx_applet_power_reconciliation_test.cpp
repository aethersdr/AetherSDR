#include "TestSettingsProfile.h"
#include "core/backends/TransmitDelta.h"
#include "gui/HGauge.h"
#include "gui/TxApplet.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QSignalSpy>
#include <QPushButton>
#include <QSlider>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-52s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                qPrintable(detail));
    if (!ok) {
        ++g_failed;
    }
}

QSlider* powerSlider(TxApplet& applet, const QString& accessibleName)
{
    const QList<QSlider*> sliders = applet.findChildren<QSlider*>();
    for (QSlider* slider : sliders) {
        if (slider->accessibleName() == accessibleName) {
            return slider;
        }
    }
    return nullptr;
}

QWidget* namedWidget(TxApplet& applet, const QString& accessibleName)
{
    const QList<QWidget*> widgets = applet.findChildren<QWidget*>();
    for (QWidget* widget : widgets) {
        if (widget->accessibleName() == accessibleName)
            return widget;
    }
    return nullptr;
}

void testReleaseReconcilesAuthoritativePower(const QString& accessibleName,
                                             bool rfPower)
{
    TransmitModel model;
    TxApplet applet;
    applet.setTransmitModel(&model);

    QSlider* slider = powerSlider(applet, accessibleName);
    const QByteArray sliderName = accessibleName.toUtf8();
    report(qPrintable(sliderName + " slider exists"), slider != nullptr);
    if (!slider) {
        return;
    }

    QSignalSpy commandSpy(&model, &TransmitModel::commandReady);

    slider->setSliderDown(true);
    slider->setValue(66);
    report(qPrintable(sliderName + " drag updates model"),
           rfPower ? model.rfPower() == 66 : model.tunePower() == 66);

    TransmitDelta clamp;
    if (rfPower) {
        clamp.rfPower = 40;
    } else {
        clamp.tunePower = 40;
    }
    model.applyChanges(clamp);

    report(qPrintable(sliderName + " defers clamp during drag"),
           slider->value() == 66,
           QString::number(slider->value()));
    commandSpy.clear();

    slider->setSliderDown(false);

    report(qPrintable(sliderName + " reconciles on release"),
           slider->value() == 40,
           QString::number(slider->value()));
    report(qPrintable(sliderName + " release sends no command"),
           commandSpy.isEmpty(),
           QString::number(commandSpy.count()));
}

void testTxMetersAreLiveOnly()
{
    TxApplet applet;
    QWidget* power = namedWidget(applet, QStringLiteral("Forward power gauge"));
    QWidget* swr = namedWidget(applet, QStringLiteral("SWR gauge"));
    report("TX gauges exist", power && swr);
    if (!power || !swr)
        return;

    applet.updateMeters(37.0f, 1.7f, true);
    auto* powerGauge = static_cast<HGauge*>(power);
    auto* swrGauge = static_cast<HGauge*>(swr);
    report("startup ignores a stale RF power sample", powerGauge->value() == 0.0f);

    applet.setTransmitting(true);
    applet.updateMeters(37.0f, 1.7f, true);
    report("active TX displays RF power",
           powerGauge->value() == 37.0f);
    report("active TX displays SWR",
           swrGauge->value() == 1.7f);

    applet.setTransmitting(false);
    report("un-key clears RF power immediately",
           powerGauge->value() == 0.0f);
    report("un-key parks SWR immediately",
           swrGauge->value() == 1.0f);

    applet.updateMeters(52.0f, 2.1f, true); // reply already in flight at un-key
    applet.updatePeakPower(60.0f);
    report("late meter replies cannot repaint idle RF power",
           powerGauge->value() == 0.0f);
}

void testAtuSuccessTogglesToBypass()
{
    TransmitModel model;
    TxApplet applet;
    applet.setTransmitModel(&model);
    auto* atu = qobject_cast<QPushButton*>(
        namedWidget(applet, QStringLiteral("ATU tune")));
    report("ATU button exists", atu != nullptr);
    if (!atu)
        return;

    QSignalSpy commandSpy(&model, &TransmitModel::commandReady);
    TransmitDelta matched;
    matched.transmitFreq = 14.100;
    matched.atuEnabled = true;
    matched.atuStatusRaw = QStringLiteral("TUNE_SUCCESSFUL");
    model.applyChanges(matched);
    atu->click();
    report("successful same-frequency ATU click requests bypass",
           !commandSpy.isEmpty()
               && commandSpy.takeLast().at(0).toString() == QStringLiteral("atu bypass"));

    TransmitDelta bypassed;
    bypassed.atuEnabled = false;
    bypassed.atuStatusRaw = QStringLiteral("TUNE_BYPASS");
    model.applyChanges(bypassed);
    commandSpy.clear();
    atu->click();
    report("bypassed ATU click starts a fresh tune",
           !commandSpy.isEmpty()
               && commandSpy.takeLast().at(0).toString() == QStringLiteral("atu start"));
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-tx-applet-power-reconciliation-test"));
    if (!settingsProfile.isValid()) {
        std::printf("[FAIL] create temporary home\n");
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);

    std::printf("TxApplet power reconciliation test harness\n\n");

    testReleaseReconcilesAuthoritativePower(QStringLiteral("RF power"), true);
    testReleaseReconcilesAuthoritativePower(QStringLiteral("Tune power"), false);
    testTxMetersAreLiveOnly();
    testAtuSuccessTogglesToBypass();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : qPrintable(QStringLiteral("%1 test(s) failed.").arg(g_failed)));
    return g_failed == 0 ? 0 : 1;
}
