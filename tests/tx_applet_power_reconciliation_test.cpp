#include "TestSettingsProfile.h"
#include "core/backends/TransmitDelta.h"
#include "gui/HGauge.h"
#include "gui/TxApplet.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QSignalSpy>
#include <QPushButton>
#include <QSlider>

#include <cmath>
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

void testCapabilityPowerScaleHonoursBandCeiling()
{
    RadioModel radio;
    TxApplet applet;
    applet.setRadioModel(&radio);
    auto* powerGauge = static_cast<HGauge*>(
        namedWidget(applet, QStringLiteral("Forward power gauge")));
    report("forward-power gauge exists for scale test", powerGauge != nullptr);
    if (!powerGauge) {
        return;
    }

    applet.setPowerScale(75, false);
    report("unverified lower-power radio preserves established face",
           powerGauge->property("gaugeMax").toFloat() == 120.0f
               && powerGauge->property("gaugeRedStart").toFloat() == 100.0f);

    RadioCapabilities caps;
    caps.txPowerBands.append(TxPowerBand{430'000'000.0, 450'000'000.0, 75.0});
    emit radio.capabilitiesChanged(true, caps);
    applet.setPowerScale(75, false);
    report("75 W capability sets 90 W face",
           powerGauge->property("gaugeMax").toFloat() == 90.0f);
    report("75 W capability sets red threshold at rating",
           powerGauge->property("gaugeRedStart").toFloat() == 75.0f);

    applet.setPowerScale(10, false);
    report("10 W capability sets 12 W face",
           powerGauge->property("gaugeMax").toFloat() == 12.0f);
    report("10 W capability sets red threshold at rating",
           powerGauge->property("gaugeRedStart").toFloat() == 10.0f);

    applet.setPowerScale(100, false);
    report("100 W capability preserves established Flex face",
           powerGauge->property("gaugeMax").toFloat() == 120.0f
               && powerGauge->property("gaugeRedStart").toFloat() == 100.0f);

    applet.setPowerScale(0, false);
    report("unknown power ceiling preserves safe established face",
           powerGauge->property("gaugeMax").toFloat() == 120.0f
               && powerGauge->property("gaugeRedStart").toFloat() == 100.0f);
}

void testForwardPowerResponseCapabilityIsConsumed()
{
    report("default capability preserves established power smoothing",
           !RadioCapabilities{}.forwardPowerRequiresSmoothing
               && RadioCapabilities{}.txPowerBands.isEmpty());

    RadioModel radio;
    TxApplet applet;
    applet.setRadioModel(&radio);
    auto* powerGauge = static_cast<HGauge*>(
        namedWidget(applet, QStringLiteral("Forward power gauge")));
    report("forward-power gauge exists for response test", powerGauge != nullptr);
    if (!powerGauge) {
        return;
    }

    RadioCapabilities caps;
    caps.forwardPowerRequiresSmoothing = false;
    emit radio.capabilitiesChanged(true, caps);
    applet.setTransmitting(true);
    applet.updateMeters(60.0f, 1.0f, true);
    report("backend response capability snaps the displayed power sample",
           std::fabs(powerGauge->filledFraction() - 0.5f) < 0.001f);
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

void testAtuCapabilityControlsVisibility()
{
    TransmitModel model;
    TxApplet applet;
    applet.setTransmitModel(&model);

    QWidget* atu = namedWidget(applet, QStringLiteral("ATU tune"));
    QWidget* mem = namedWidget(applet, QStringLiteral("ATU memories"));
    QWidget* indicators = namedWidget(applet, QStringLiteral("ATU status indicators"));
    QWidget* profile = namedWidget(applet, QStringLiteral("TX profile"));
    QWidget* tune = namedWidget(applet, QStringLiteral("Tune"));
    QWidget* mox = namedWidget(applet, QStringLiteral("MOX transmit"));
    report("ATU capability widgets exist",
           atu && mem && indicators && profile && tune && mox);
    if (!atu || !mem || !indicators || !profile || !tune || !mox) {
        return;
    }

    applet.resize(520, applet.sizeHint().height());
    applet.show();
    QApplication::processEvents();
    const int profileWidth = profile->width();
    const int tuneWidth = tune->width();
    const int moxWidth = mox->width();

    model.setHasTuner(false);
    QApplication::processEvents();
    report("ordinary absent tuner keeps ATU button visible", !atu->isHidden());
    report("ordinary absent tuner disables ATU button", !atu->isEnabled());
    report("ordinary absent tuner keeps explanatory tooltip reachable",
           !atu->toolTip().isEmpty());

    model.setHideUnavailableTunerControls(true);
    QApplication::processEvents();
    report("absent tuner hides ATU button", atu->isHidden());
    report("absent tuner hides MEM button", mem->isHidden());
    report("absent tuner hides ATU status indicators", indicators->isHidden());
    report("absent tuner preserves TUNE", !tune->isHidden());
    report("absent tuner preserves MOX", !mox->isHidden());
    report("absent tuner preserves profile dropdown width",
           profile->width() == profileWidth);
    report("absent tuner preserves TUNE button width", tune->width() == tuneWidth);
    report("absent tuner preserves MOX button width", mox->width() == moxWidth);

    model.setHasTuner(true);
    QApplication::processEvents();
    report("present tuner restores ATU button", !atu->isHidden());
    report("present tuner restores MEM button", !mem->isHidden());
    report("present tuner restores ATU status indicators", !indicators->isHidden());
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
    qputenv("AETHER_AUTOMATION", "1");

    QApplication app(argc, argv);

    std::printf("TxApplet power reconciliation test harness\n\n");

    testReleaseReconcilesAuthoritativePower(QStringLiteral("RF power"), true);
    testReleaseReconcilesAuthoritativePower(QStringLiteral("Tune power"), false);
    testTxMetersAreLiveOnly();
    testCapabilityPowerScaleHonoursBandCeiling();
    testForwardPowerResponseCapabilityIsConsumed();
    testAtuSuccessTogglesToBypass();
    testAtuCapabilityControlsVisibility();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : qPrintable(QStringLiteral("%1 test(s) failed.").arg(g_failed)));
    return g_failed == 0 ? 0 : 1;
}
