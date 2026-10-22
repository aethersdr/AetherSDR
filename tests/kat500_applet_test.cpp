#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/Kat500Applet.h"
#include "gui/HGauge.h"
#include "models/Kat500Model.h"
#include <QApplication>
#include <QCheckBox>
#include <QFileDialog>
#include <QPointer>
#include <QPlainTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <cstdio>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                             \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (false)
namespace AetherSDR
{
class Kat500ModelTest
{
  public:
    static void ready(Kat500Model& model)
    {
        model.m_now = [] { return qint64(100); };
        model.m_connected = true;
        model.m_detected = true;
        model.m_lastReply = 100;
        model.m_status.identity = "KAT500";
        model.m_status.powerOn = true;
        model.m_status.tuning = false;
        model.m_status.forwardAdc = 0;
        model.m_status.band = 5;
        model.m_status.antenna = 1;
        model.m_status.mode = Kat500::Mode::Auto;
        model.m_status.capacitorMask = 4;
        model.m_status.frequencyKhz = 14096;
        model.m_status.countedFrequencyKhz = 14088;
        model.m_status.catFrequencyKhz = 14090;
        model.m_status.frequencyBin = "14080-14099";
        model.m_status.side = Kat500::CapacitorSide::Transmitter;
        model.m_status.inductorMask = 3;
        model.m_status.swr = 1.33;
        model.m_status.bypassSwr = 1.0;
        model.m_status.faultCode = 0;
        model.m_status.bypassed = false;
        model.m_status.attenuator = false;
        model.m_status.ampKeyInterrupted = false;
        model.m_received["TP"] = 100;
        model.m_received["VFWD"] = 100;
    }
    static void bypass(Kat500Model& model, Kat500::Mode mode, bool bypassed)
    {
        model.m_status.mode = mode;
        model.m_status.bypassed = bypassed;
        emit model.statusUpdated(model.m_status);
    }
    static void rf(Kat500Model& model, bool present)
    {
        model.m_status.forwardAdc = present ? 100 : 0;
        model.m_received["VSWR"] = model.m_received["VFWD"] = 100;
        emit model.statusUpdated(model.m_status);
    }
    static void interrupt(Kat500Model& model, bool blocked)
    {
        model.m_status.ampKeyInterrupted = blocked;
        emit model.statusUpdated(model.m_status);
    }
    static void antennas(Kat500Model& model)
    {
        for (int i = 1; i <= 3; ++i) {
            model.m_configuration[{Kat500::Setting::EnabledAntenna, 5, i}] = i == 2 ? 0 : 1;
        }
        emit model.configurationRead(model.m_configuration);
    }
    static void blocked(Kat500Model& model, int condition)
    {
        model.m_status.forwardAdc = condition == 0 ? 1 : 0;
        model.m_operation = condition == 1 ? QStringLiteral("Test maintenance") : QString();
        model.m_status.tuning = condition == 2;
        emit model.availabilityChanged();
    }
    static void stale(Kat500Model& model)
    {
        model.m_lastReply = -10000;
        emit model.availabilityChanged();
    }
    static int signalPathToggles()
    {
        Kat500Model model;
        ready(model);
        model.m_lastIo = 100;
        QList<QByteArray> wire;
        model.m_write = [&wire](const QByteArray& bytes) { wire.append(bytes); return true; };
        Kat500Applet widget;
        widget.setModel(&model);
        const char* names[]{"katBypassedStatus", "katAttenuatorStatus", "katAmpInterruptStatus"};
        const QByteArray settersOn[]{"BYPB;", "ATTN1;", "AMPI1;"};
        const QByteArray settersOff[]{"BYPN;", "ATTN0;", "AMPI0;"};
        const QByteArray getters[]{"BYP;", "ATTN;", "AMPI;"};
        std::optional<bool>* states[]{&model.m_status.bypassed, &model.m_status.attenuator,
                                     &model.m_status.ampKeyInterrupted};
        for (int i = 0; i < 3; ++i) {
            auto* control = widget.findChild<QPushButton*>(names[i]);
            CHECK(control && control->isCheckable() && control->isEnabled());
            CHECK(!control->isChecked() && !control->accessibleDescription().isEmpty());
            for (bool requested : {true, false}) {
                const bool confirmed = states[i]->value();
                control->click();
                CHECK(model.operationBusy() && !control->isEnabled());
                CHECK(control->isChecked() == confirmed);
                model.tick();
                const QByteArray setter = requested ? settersOn[i] : settersOff[i];
                CHECK(wire.last() == setter + getters[i]);
                model.receiveBytes(setter);
                CHECK(!model.operationBusy() && control->isEnabled());
                CHECK(control->isChecked() == requested && *states[i] == requested);
                CHECK(model.status().mode == Kat500::Mode::Auto);
            }
            states[i]->reset();
            emit model.statusUpdated(model.status());
            CHECK(!control->isEnabled() && control->accessibleDescription().contains("confirmed"));
            *states[i] = false;
            emit model.statusUpdated(model.status());
        }
        CHECK(wire.size() == 6);
        for (const char* name : {"katManualState0", "katManualState1", "katManualState2"}) {
            CHECK(!widget.findChild<QWidget*>(name));
        }
        for (int condition = 0; condition < 3; ++condition) {
            blocked(model, condition);
            for (const char* name : names) {
                auto* control = widget.findChild<QPushButton*>(name);
                CHECK(!control->isEnabled() && !control->accessibleDescription().isEmpty());
                control->click();
            }
            CHECK(wire.size() == 6);
        }
        return 0;
    }
    static int debugScrolling()
    {
        Kat500Model model;
        Kat500Applet widget;
        widget.setModel(&model);
        widget.findChild<QPushButton*>("katSettings")->setChecked(true);
        widget.findChild<QPushButton*>("katDebug")->setChecked(true);
        widget.resize(400, 1000);
        widget.show();
        QTest::qWait(20);
        auto* output = widget.findChild<QPlainTextEdit*>("katDebugOutput");
        CHECK(output);
        QScrollBar* scroll = output->verticalScrollBar();
        model.report(QString(80, QLatin1Char('x')));
        for (int i = 0; i < 80; ++i) {
            model.report(QStringLiteral("Diagnostic %1").arg(i));
        }
        CHECK(scroll->maximum() > 0);
        CHECK(scroll->value() == scroll->maximum());
        widget.resize(800, 1000);
        QTest::qWait(20);
        widget.resize(400, 1000);
        QTest::qWait(20);
        model.report(QStringLiteral("Newest entry after resizing"));
        CHECK(scroll->value() == scroll->maximum());
        scroll->setValue(10);
        model.report(QStringLiteral("New entry while reading older diagnostics"));
        CHECK(scroll->value() == 10);
        scroll->setValue(scroll->maximum());
        for (int i = 0; i < 1100; ++i) {
            model.report(QStringLiteral("Bounded diagnostic %1").arg(i));
        }
        CHECK(scroll->value() == scroll->maximum());
        CHECK(output->toPlainText().endsWith("Bounded diagnostic 1099"));
        QCheckBox* pause = nullptr;
        for (QCheckBox* control : widget.findChildren<QCheckBox*>()) {
            if (control->text() == "Pause display") {
                pause = control;
            }
        }
        CHECK(pause);
        pause->setChecked(true);
        const QString frozen = output->toPlainText();
        model.report(QStringLiteral("Captured while paused"));
        CHECK(output->toPlainText() == frozen);
        pause->setChecked(false);
        CHECK(output->toPlainText().endsWith("Captured while paused"));
        CHECK(scroll->value() == scroll->maximum());
        model.clearDebug();
        CHECK(output->toPlainText().isEmpty() && scroll->maximum() == 0);
        return 0;
    }
    static int historyResizing()
    {
        Kat500Applet widget;
        widget.setFloating(true);
        widget.findChild<QPushButton*>("katHistory")->setChecked(true);
        widget.findChild<QPushButton*>("katSettings")->setChecked(true);
        widget.findChild<QPushButton*>("katDebug")->setChecked(true);
        widget.resize(350, 500);
        widget.show();
        QTest::qWait(30);
        auto* graph = widget.findChild<QLabel*>("katHistoryGraph");
        auto* scroll = widget.findChild<QScrollArea*>("katScroll");
        CHECK(graph && scroll);
        const QSize compact = graph->size();
        for (int i = 0; i < 2; ++i) {
            widget.resize(700, 1800);
            QTest::qWait(30);
            CHECK(graph->height() > compact.height() && graph->width() > compact.width());
            widget.resize(350, 500);
            QTest::qWait(30);
            CHECK(graph->size() == compact);
            CHECK(scroll->verticalScrollBar()->maximum() > 0);
            CHECK(scroll->horizontalScrollBar()->maximum() == 0);
            CHECK(graph->pixmap().deviceIndependentSize().toSize() == graph->size());
        }
        widget.findChild<QPushButton*>("katSettings")->setChecked(false);
        QTest::qWait(30);
        CHECK(scroll->verticalScrollBar()->maximum() == 0);
        widget.findChild<QPushButton*>("katHistory")->setChecked(false);
        return 0;
    }
    static void off(Kat500Model& model)
    {
        model.m_lastReply = 100;
        model.m_status.powerOn = false;
        emit model.statusUpdated(model.m_status);
    }
};
} // namespace AetherSDR
int main(int argc, char** argv)
{
    TestSettingsProfile profile("kat500-applet-test");
    QApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();
    const int debugResult = AetherSDR::Kat500ModelTest::debugScrolling();
    const int historyResult = AetherSDR::Kat500ModelTest::historyResizing();
    CHECK(debugResult == 0 && historyResult == 0);
    CHECK(AetherSDR::Kat500ModelTest::signalPathToggles() == 0);
    AetherSDR::Kat500Applet widget;
    widget.resize(310, 300);
    widget.show();
    auto* mini = widget.findChild<QPushButton*>("katMini");
    CHECK(mini);
    auto* tune = widget.findChild<QPushButton*>("katTune");
    CHECK(tune && tune->property("aetherTxKeying").toBool());
    CHECK(!tune->isEnabled() && !tune->accessibleDescription().isEmpty());
    auto* follow = widget.findChild<QCheckBox*>("katFollowTx");
    CHECK(follow && !follow->isChecked());
    auto* scroll = widget.findChild<QScrollArea*>("katScroll");
    CHECK(scroll);
    for (const int width : {238, 248, 254, 300, 360}) {
        widget.resize(width, 170);
        CHECK(widget.width() == width);
        QTest::qWait(30);
        CHECK(scroll->verticalScrollBar()->maximum() == 0);
        CHECK(scroll->horizontalScrollBar()->maximum() == 0);
        for (QPushButton* button : widget.findChildren<QPushButton*>()) {
            if (button->isVisible()) {
                CHECK(button->height() <= 22);
                CHECK(button->font().pixelSize() <= 11);
            }
        }
        CHECK(!widget.findChild<QLabel*>("katAvailability")->isVisible());
        CHECK(!widget.findChild<QLabel*>("katOperationResult")->isVisible());
    }
    mini->setChecked(true);
    QTest::qWait(20);
    widget.resize(248, 98);
    QTest::qWait(20);
    CHECK(widget.height() == 98 && scroll->verticalScrollBar()->maximum() == 0);
    CHECK(scroll->horizontalScrollBar()->maximum() == 0);
    for (const char* name : {"katPower", "katTune", "katSwr", "katFault", "katMiniState", "katFrequency"}) {
        auto* control = widget.findChild<QWidget*>(name);
        CHECK(control && control->isVisible());
    }
    for (const char* name : {"katAuto", "katManual", "katBypass", "katAntenna1", "katAntenna2", "katAntenna3",
                             "katClearFault", "katPhysicalState", "katManualPane"}) {
        CHECK(!widget.findChild<QWidget*>(name)->isVisible());
    }
    mini->setChecked(false);
    widget.findChild<QPushButton*>("katManualToggle")->setChecked(true);
    widget.findChild<QPushButton*>("katSettings")->setChecked(true);
    widget.findChild<QPushButton*>("katDebug")->setChecked(true);
    QTest::qWait(20);
    CHECK(scroll && scroll->verticalScrollBar()->maximum() > 0);
    for (const int width : {238, 248, 254, 280}) {
        widget.resize(width, 300);
        QTest::qWait(20);
        CHECK(scroll->horizontalScrollBar()->maximum() == 0);
    }
    widget.resize(700, 1400);
    QTest::qWait(20);
    CHECK(scroll->horizontalScrollBar()->maximum() == 0);
    QSignalSpy advanced(&widget, &AetherSDR::Kat500Applet::advancedSettingsRequested);
    widget.findChild<QPushButton*>("katAdvancedSettings")->click();
    CHECK(advanced.size() == 1);
    AetherSDR::Kat500Model model;
    AetherSDR::Kat500ModelTest::ready(model);
    widget.setModel(&model);
    CHECK(widget.findChild<QPushButton*>("katPower")->text() == "ON" && tune->isEnabled());
    CHECK(widget.findChild<QLabel*>("katReadiness")->text() == "READY");
    auto* miniState = widget.findChild<QLabel*>("katMiniState");
    CHECK(miniState->property("katTone") == "ok");
    AetherSDR::Kat500ModelTest::bypass(model, AetherSDR::Kat500::Mode::Bypass, true);
    CHECK(miniState->text() == "BYPASS · ANT 1 · READY");
    CHECK(miniState->property("katTone") == "warn");
    AetherSDR::Kat500ModelTest::bypass(model, AetherSDR::Kat500::Mode::Auto, true);
    CHECK(miniState->property("katTone") == "warn");
    AetherSDR::Kat500ModelTest::bypass(model, AetherSDR::Kat500::Mode::Auto, false);
    CHECK(miniState->property("katTone") == "ok");
    auto* swr = widget.findChild<QPushButton*>("katSwr");
    CHECK(swr && swr->isEnabled());
    auto* gaugeWidget = widget.findChild<QWidget*>("katSwrGauge");
    CHECK(gaugeWidget);
    auto* swrGauge = static_cast<AetherSDR::HGauge*>(gaugeWidget);
    CHECK(swr->text() == "SWR 1.33");
    CHECK(swr->accessibleDescription().contains("no fresh RF indication"));
    CHECK(swr->width() == 62);
    QSignalSpy writes(&model, &AetherSDR::Kat500Model::operationBusyChanged);
    QTest::mousePress(swr, Qt::LeftButton);
    CHECK(swr->text() == "BYP 1.00" && swrGauge->value() == 1.0f);
    QTest::mouseRelease(swr, Qt::LeftButton);
    CHECK(swr->text() == "SWR 1.33" && qFuzzyCompare(swrGauge->value(), 1.33f));
    QTest::mousePress(swr, Qt::LeftButton);
    QFocusEvent focusOut(QEvent::FocusOut);
    QApplication::sendEvent(swr, &focusOut);
    CHECK(!swr->isDown() && swr->text() == "SWR 1.33");
    QTest::mouseRelease(swr, Qt::LeftButton);
    AetherSDR::Kat500ModelTest::rf(model, true);
    CHECK(swr->text() == "SWR 1.33");
    CHECK(!swr->accessibleDescription().contains("no fresh RF indication"));
    AetherSDR::Kat500ModelTest::rf(model, false);
    CHECK(swr->text() == "SWR 1.33" && writes.isEmpty());
    auto* key = widget.findChild<QPushButton*>("katAmpInterruptStatus");
    CHECK(key->text() == "KEY PASS");
    AetherSDR::Kat500ModelTest::interrupt(model, true);
    CHECK(key->text() == "KEY BLOCK");
    AetherSDR::Kat500ModelTest::interrupt(model, false);
    for (const char* name : {"katManualToggle", "katSettings", "katHistory", "katDebug"}) {
        auto* toggle = widget.findChild<QPushButton*>(name);
        toggle->setChecked(false);
        CHECK(toggle->text().startsWith(QStringLiteral("▸")));
        toggle->setChecked(true);
        CHECK(toggle->text().startsWith(QStringLiteral("▾")));
        toggle->setChecked(false);
    }
    widget.findChild<QPushButton*>("katManualToggle")->setChecked(true);
    for (const char* name : {"katCapacitorSide"}) {
        CHECK(!widget.findChild<QCheckBox*>(name)->isVisible());
    }
    auto* memory = widget.findChild<QSpinBox*>("katRecallFrequency");
    CHECK(memory->suffix() == " kHz");
    CHECK(memory->value() == 14096);
    memory->setValue(1);
    CHECK(!widget.findChild<QPushButton*>("katRecall")->isEnabled());
    CHECK(!widget.findChild<QPushButton*>("katMemorize")->isEnabled());
    CHECK(widget.findChild<QPushButton*>("katRecall")->toolTip().contains("1800"));
    memory->setValue(7160);
    AetherSDR::Kat500ModelTest::rf(model, false);
    CHECK(memory->value() == 7160);
    CHECK(widget.findChild<QPushButton*>("katRecall")->isEnabled());
    memory->setValue(14096);
    CHECK(widget.findChild<QLabel*>("katRecallFrequencyLabel")->buddy() == memory);
    widget.findChild<QPushButton*>("katSettings")->setChecked(true);
    for (const char* name : {"katCapacitorSide"}) {
        auto* control = widget.findChild<QCheckBox*>(name);
        CHECK(control->isVisible() && control->isEnabled());
    }
    const QString observed = widget.findChild<QLabel*>("katLastObserved")->text();
    CHECK(observed.contains("14096 kHz") && observed.contains("14088 kHz") &&
          observed.contains("14090 kHz") && observed.contains("14080-14099 kHz"));
    CHECK(widget.findChild<QLabel*>("katAvailability")->isVisible());
    CHECK(widget.findChild<QLabel*>("katAvailability")->text().contains("ms since reply"));
    CHECK(!widget.findChild<QPushButton*>("katAntenna1")->isEnabled());
    AetherSDR::Kat500ModelTest::antennas(model);
    CHECK(widget.findChild<QPushButton*>("katAntenna1")->isEnabled());
    CHECK(!widget.findChild<QPushButton*>("katAntenna2")->isEnabled());
    CHECK(widget.findChild<QPushButton*>("katAntenna2")->accessibleDescription().contains("disabled"));
    for (const char* name : {"katManualToggle", "katSettings", "katHistory"}) {
        widget.findChild<QPushButton*>(name)->setChecked(false);
    }
    widget.resize(248, 170);
    QTest::qWait(20);
    CHECK(scroll->verticalScrollBar()->maximum() == 0);
    const QString capture = qEnvironmentVariable("AETHER_KAT500_GRAB");
    if (!capture.isEmpty()) {
        CHECK(widget.grab().save(capture + "/applet-compact.png"));
    }
    mini->setChecked(true);
    widget.resize(248, 98);
    QTest::qWait(20);
    CHECK(widget.findChild<QLabel*>("katMiniState")->text() == "AUTO · ANT 1 · READY");
    CHECK(scroll->verticalScrollBar()->maximum() == 0);
    if (!capture.isEmpty()) {
        CHECK(widget.grab().save(capture + "/applet-mini.png"));
    }
    mini->setChecked(false);
    widget.findChild<QPushButton*>("katManualToggle")->setChecked(true);
    widget.findChild<QPushButton*>("katSettings")->setChecked(true);
    widget.resize(700, 1400);
    if (!capture.isEmpty()) {
        widget.findChild<QPushButton*>("katSettings")->setChecked(false);
        widget.resize(248, 540);
        QTest::qWait(30);
        CHECK(widget.grab().save(capture + "/applet-manual.png"));
        widget.findChild<QPushButton*>("katManualToggle")->setChecked(false);
        widget.findChild<QPushButton*>("katSettings")->setChecked(true);
        widget.resize(248, 720);
        QTest::qWait(30);
        CHECK(widget.grab().save(capture + "/applet-settings.png"));
        widget.findChild<QPushButton*>("katManualToggle")->setChecked(true);
        widget.resize(700, 1400);
    }
    auto* capacitance = widget.findChild<QSpinBox*>("katCapacitance");
    capacitance->setValue(1000);
    emit model.availabilityChanged();
    CHECK(capacitance->value() == 1000);
    widget.findChild<QPushButton*>("katHistory")->setChecked(true);
    AetherSDR::Kat500::Status sample = model.status();
    sample.swr = 1;
    sample.forwardAdc = 1;
    sample.reflectedAdc = 1;
    for (const QString& command : {QStringLiteral("VSWR"), QStringLiteral("VFWD"), QStringLiteral("VRFL")}) {
        emit model.telemetryReceived(command, sample);
    }
    QTest::qWait(20);
    sample.swr = 5;
    sample.forwardAdc = 250;
    sample.reflectedAdc = 250;
    for (const QString& command : {QStringLiteral("VSWR"), QStringLiteral("VFWD"), QStringLiteral("VRFL")}) {
        emit model.telemetryReceived(command, sample);
    }
    emit model.availabilityChanged();
    const QString summary = widget.findChild<QLabel*>("katHistoryGraph")->accessibleDescription();
    CHECK(summary.contains("SWR") && summary.contains("Forward") && summary.contains("Reflected"));
    CHECK(summary.contains("250") && summary.contains("5 minutes"));
    const QImage image = widget.findChild<QLabel*>("katHistoryGraph")->pixmap().toImage();
    const QColor cyan = AetherSDR::ThemeManager::instance().color(&widget, "color.canon.cyan");
    const QColor aqua = AetherSDR::ThemeManager::instance().color(&widget, "color.canon.aqua");
    int cyanPixels = 0, aquaPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            cyanPixels += image.pixelColor(x, y) == cyan;
            aquaPixels += image.pixelColor(x, y) == aqua;
        }
    }
    const bool visibleTraces = cyanPixels > 10 && aquaPixels > 10;
    if (!visibleTraces) {
        std::fprintf(stderr, "FAIL missing visible history traces: cyan=%d aqua=%d\n", cyanPixels,
                     aquaPixels);
    }
    if (!capture.isEmpty()) {
        widget.findChild<QPushButton*>("katSettings")->setChecked(false);
        widget.findChild<QPushButton*>("katManualToggle")->setChecked(false);
        widget.resize(248, 450);
        QTest::qWait(30);
        CHECK(widget.grab().save(capture + "/applet-history.png"));
    }
    for (int condition = 0; condition < 3; ++condition) {
        AetherSDR::Kat500ModelTest::blocked(model, condition);
        auto* selectedMode = widget.findChild<QPushButton*>("katAuto");
        auto* selectedAntenna = widget.findChild<QPushButton*>("katAntenna1");
        CHECK(selectedMode->isChecked() && selectedAntenna->isChecked());
        CHECK(!selectedMode->isEnabled() && !selectedAntenna->isEnabled());
        for (const char* name : {"katCapacitorSide", "katBypassedStatus", "katAttenuatorStatus", "katAmpInterruptStatus"}) {
            auto* control = widget.findChild<QWidget*>(name);
            CHECK(!control->isEnabled() && !control->accessibleDescription().isEmpty());
        }
        // Render each disabled button in both states: selection must change its
        // background/border as well as retaining the checked property.
        for (QPushButton* selected : {selectedMode, selectedAntenna}) {
            const QImage checked = selected->grab().toImage();
            selected->setChecked(false);
            const QImage unchecked = selected->grab().toImage();
            selected->setChecked(true);
            const QColor selectedFill = AetherSDR::ThemeManager::instance().color(
                selected, "color.accessory.key.meffa.active.background");
            int changed = 0, selectedPixels = 0;
            for (int y = 0; y < checked.height(); ++y) {
                for (int x = 0; x < checked.width(); ++x) {
                    changed += checked.pixel(x, y) != unchecked.pixel(x, y);
                    selectedPixels += checked.pixelColor(x, y) == selectedFill;
                }
            }
            if (selectedPixels <= checked.width()) {
                std::fprintf(stderr, "selection %s: changed=%d selected=%d expected=%s center=%s\n",
                             qPrintable(selected->objectName()), changed, selectedPixels,
                             qPrintable(selectedFill.name()), qPrintable(checked.pixelColor(10,10).name()));
            }
            CHECK(changed > checked.width() && selectedPixels > checked.width());
        }
    }
    AetherSDR::Kat500ModelTest::blocked(model, 3);
    AetherSDR::Kat500ModelTest::stale(model);
    CHECK(!tune->isEnabled() && widget.findChild<QPushButton*>("katPower")->text() == "ON");
    AetherSDR::Kat500ModelTest::off(model);
    CHECK(widget.findChild<QPushButton*>("katPower")->text() == "OFF" && !tune->isEnabled());
    CHECK(!model.followTxFrequency());
    CHECK(widget.findChild<QPushButton*>("katAuto")->isChecked() &&
          widget.findChild<QPushButton*>("katAntenna1")->isChecked());
    auto* transient = new AetherSDR::Kat500Applet;
    transient->show();
    transient->findChild<QPushButton*>("katSettings")->setChecked(true);
    transient->findChild<QPushButton*>("katDebug")->setChecked(true);
    transient->findChild<QPushButton*>("katDebugSave")->click();
    QTest::qWait(250);
    QPointer<QFileDialog> dialog = transient->findChild<QFileDialog*>("katDebugFileDialog");
    CHECK(dialog);
    delete transient;
    CHECK(dialog && !dialog->isVisible() && !dialog->parent());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTest::qWait(250);
    CHECK(dialog.isNull());
    return visibleTraces ? 0 : 1;
}
