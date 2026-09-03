#include "TestSettingsProfile.h"
#include "core/backends/TransmitDelta.h"
#include "gui/HGauge.h"
#include "gui/TxApplet.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QLabel>
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

QLabel* labelWithText(TxApplet& applet, const QString& text)
{
    const QList<QLabel*> labels = applet.findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->text() == text) {
            return label;
        }
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

void testAtuCapabilityUsesThreeVisibleStates()
{
    TransmitModel model;
    TxApplet applet;
    applet.setTransmitModel(&model);

    auto* atu = qobject_cast<QPushButton*>(
        namedWidget(applet, QStringLiteral("ATU tune")));
    auto* mem = qobject_cast<QPushButton*>(
        namedWidget(applet, QStringLiteral("ATU memories")));
    QLabel* success = labelWithText(applet, QStringLiteral("Success"));
    QLabel* bypass = labelWithText(applet, QStringLiteral("Byp"));
    QLabel* memory = labelWithText(applet, QStringLiteral("Mem"));
    report("ATU capability widgets exist",
           atu && mem && success && bypass && memory);
    if (!atu || !mem || !success || !bypass || !memory) {
        return;
    }
    const QString inactiveSuccessStyle = success->styleSheet();
    const QString inactiveBypassStyle = bypass->styleSheet();
    const QString inactiveMemoryStyle = memory->styleSheet();

    report("available inactive ATU controls remain enabled",
           atu->isEnabled() && mem->isEnabled()
               && !atu->isCheckable() && mem->isCheckable()
               && !mem->isChecked());
    report("available inactive indicators are greyed",
           success->isEnabled() && bypass->isEnabled() && memory->isEnabled()
               && !inactiveSuccessStyle.isEmpty()
               && inactiveSuccessStyle == inactiveBypassStyle
               && inactiveSuccessStyle == inactiveMemoryStyle);

    QSignalSpy commandSpy(&model, &TransmitModel::commandReady);
    model.setHasTuner(false);
    model.setHasTunerMemories(false);
    QApplication::processEvents();
    report("unavailable tuner controls remain visible",
           !atu->isHidden() && !mem->isHidden()
               && !success->isHidden() && !bypass->isHidden() && !memory->isHidden());
    report("unavailable tuner controls are dimmed and inert",
           !atu->isEnabled() && !mem->isEnabled());
    report("unavailable tuner indicators are dimmed",
           !success->isEnabled() && !bypass->isEnabled() && !memory->isEnabled()
               && success->styleSheet() != inactiveSuccessStyle
               && bypass->styleSheet() != inactiveBypassStyle
               && memory->styleSheet() != inactiveMemoryStyle);
    atu->click();
    mem->click();
    report("unavailable tuner controls emit no commands", commandSpy.isEmpty());
    report("unavailable tuner controls explain the state",
           atu->toolTip()
                   == QStringLiteral("Antenna tuner controls are unavailable for this radio")
               && mem->toolTip()
                   == QStringLiteral("ATU memory controls are unavailable for this radio"));

    model.setHasTuner(true);
    model.setHasTunerMemories(true);
    QApplication::processEvents();
    report("available tuner controls return to inactive state",
           atu->isEnabled() && mem->isEnabled()
               && !mem->isChecked()
               && success->styleSheet() == inactiveSuccessStyle
               && bypass->styleSheet() == inactiveBypassStyle
               && memory->styleSheet() == inactiveMemoryStyle);

    model.setHasTunerMemories(false);
    QApplication::processEvents();
    report("Icom-style tuner availability keeps memory surfaces unavailable",
           atu->isEnabled() && !mem->isEnabled()
               && atu->contextMenuPolicy() == Qt::NoContextMenu
               && success->isEnabled() && success->styleSheet() == inactiveSuccessStyle
               && !memory->isEnabled() && memory->styleSheet() != inactiveMemoryStyle);
    mem->click();
    report("unavailable tuner-memory control emits no command", commandSpy.isEmpty());

    model.setHasTunerMemories(true);
    QApplication::processEvents();
    report("tuner-memory capability restores memory-only menu actions",
           atu->contextMenuPolicy() == Qt::CustomContextMenu);

    TransmitDelta active;
    active.atuStatusRaw = QStringLiteral("TUNE_SUCCESSFUL");
    active.atuEnabled = true;
    active.memoriesEnabled = true;
    active.usingMemory = true;
    model.applyChanges(active);
    QApplication::processEvents();
    report("available active tuner indicators are enabled",
           success->isEnabled() && memory->isEnabled() && bypass->isEnabled()
               && success->styleSheet() != inactiveSuccessStyle
               && memory->styleSheet() != inactiveMemoryStyle
               && bypass->styleSheet() == inactiveBypassStyle);
    report("available active tuner memory control follows radio readback",
           !atu->isCheckable() && mem->isChecked());
    const QString activeSuccessStyle = success->styleSheet();
    const QString activeMemoryStyle = memory->styleSheet();

    model.setHasTuner(false);
    model.setHasTunerMemories(false);
    QApplication::processEvents();
    report("active tuner controls dim when capability disappears",
           mem->isChecked()
               && !atu->isEnabled() && !mem->isEnabled()
               && !success->isEnabled() && !memory->isEnabled()
               && success->styleSheet() != inactiveSuccessStyle
               && memory->styleSheet() != inactiveMemoryStyle
               && success->styleSheet() != activeSuccessStyle
               && memory->styleSheet() != activeMemoryStyle);
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
    testAtuCapabilityUsesThreeVisibleStates();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : qPrintable(QStringLiteral("%1 test(s) failed.").arg(g_failed)));
    return g_failed == 0 ? 0 : 1;
}
