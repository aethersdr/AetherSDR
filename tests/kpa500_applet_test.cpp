#include "gui/Kpa500Applet.h"
#include "gui/HGauge.h"
#include "models/MeterModel.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "TestSettingsProfile.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <QVBoxLayout>
#include <cmath>
#include <cstdio>

namespace AetherSDR {
class Kpa500AppletTest {
public:
    static int run()
    {
        int failed = 0;
        const auto check = [&failed](bool ok, const char* name) {
            std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok) { ++failed; }
        };
        Kpa500Applet widget;
        widget.resize(520, 390);
        widget.show();
        QTest::qWait(20);
        auto* band = widget.findChild<QComboBox*>("kpaBand");
        auto* power = widget.findChild<QPushButton*>("kpaPower");
        auto* operate = widget.findChild<QPushButton*>("kpaOperate");
        auto* follow = widget.findChild<QCheckBox*>("kpaFollowTx");
        auto* speaker = widget.findChild<QCheckBox*>("kpaSpeaker");
        QSignalSpy powers(&widget, &Kpa500Applet::powerRequested);
        QSignalSpy modes(&widget, &Kpa500Applet::operateToggled);
        QSignalSpy bands(&widget, &Kpa500Applet::bandRequested);
        QSignalSpy fans(&widget, &Kpa500Applet::fanSpeedChangeRequested);
        QSignalSpy speakers(&widget, &Kpa500Applet::speakerRequested);
        MeterModel radioMeters;
        widget.updateRadioDrive(radioMeters, true);
        check(widget.m_driveReadout->text() == "DRV —" && !widget.m_driveGauge->isEnabled(),
              "Missing radio power is unavailable, not a fabricated zero");
        MeterDef forward;
        forward.index = 8; forward.source = "TX"; forward.name = "FWDPWR"; forward.unit = "Watts";
        radioMeters.defineMeter(forward);
        widget.updateRadioDrive(radioMeters, true);
        check(!widget.m_driveGauge->isEnabled(), "A declared but unfed radio meter is unavailable");
        radioMeters.updateValueByName("TX", "FWDPWR", 12.5f);
        widget.updateRadioDrive(radioMeters, true);
        check(widget.m_driveReadout->text() == "DRV 12.5 W" && widget.m_driveGauge->value() == 12.5f,
              "DRV uses measured radio watts with fractional precision");
        check(widget.m_driveReadout->toolTip().contains("before cable losses")
              && widget.m_driveGauge->accessibleDescription().contains("not measured at the KPA500 input"),
              "DRV identifies the source and cable-loss limitation in tooltip and accessibility text");
        Kpa500::Status amplified;
        amplified.operate = true; amplified.forwardPowerW = 500.0f;
        widget.applyStatus(amplified);
        MeterDef amp;
        amp.index = 12; amp.source = "AMP"; amp.name = "FWD"; amp.unit = "dBm";
        radioMeters.defineMeter(amp);
        radioMeters.updateValueByName("AMP", "FWD", 57.0f);
        widget.updateRadioDrive(radioMeters, true);
        check(widget.m_driveGauge->value() == 12.5f && widget.m_readouts[0]->text() == "PWR 500 W",
              "Amplifier status and AMP relay meters cannot replace DRV radio watts");
        widget.updateRadioDrive(radioMeters, false);
        check(widget.m_driveReadout->text() == "DRV —" && widget.m_driveGauge->value() == 0.0f,
              "Radio disconnect immediately clears a still-fresh drive reading");
        QTest::qWait(MeterModel::kTxMeterStaleMs + 50);
        MeterDef swr;
        swr.index = 10; swr.source = "TX"; swr.name = "SWR"; swr.unit = "SWR";
        radioMeters.defineMeter(swr);
        radioMeters.updateValueByName("TX", "SWR", 1.2f);
        radioMeters.updateValueByName("AMP", "FWD", 57.0f);
        widget.updateRadioDrive(radioMeters, true);
        check(widget.m_driveReadout->text() == "DRV —" && !widget.m_driveGauge->isEnabled()
              && widget.m_driveGauge->filledFraction() == 0.0f,
              "Fresh SWR and amplifier telemetry cannot keep stale radio forward watts alive");
        radioMeters.updateValueByName("TX", "FWDPWR", 0.0f);
        widget.updateRadioDrive(radioMeters, true);
        check(widget.m_driveReadout->text() == "DRV 0.0 W" && widget.m_driveGauge->isEnabled(),
              "A measured zero is available and distinct from a missing reading");
        radioMeters.removeMeter(8);
        widget.updateRadioDrive(radioMeters, true);
        check(!widget.m_driveGauge->isEnabled(), "Removing the radio meter invalidates DRV");
        radioMeters.defineMeter(forward);
        radioMeters.updateValueByName("TX", "FWDPWR", 10.0f);
        widget.updateRadioDrive(radioMeters, true);
        check(widget.m_driveGauge->isEnabled(), "Fresh data restores drive availability before session reset");
        radioMeters.clear();
        widget.updateRadioDrive(radioMeters, true);
        check(!widget.m_driveGauge->isEnabled(), "A new radio session cannot inherit the previous drive reading");
        widget.applyStatus({});
        check(!widget.m_verbose->isChecked(), "Verbose debug defaults off");
        check(widget.followTx() && band->currentIndex() == -1 && !band->isEnabled(), "Follow TX defaults on without inventing a band");
        check(!power->isEnabled() && power->text() == QStringLiteral("—"), "Disconnected power control is unavailable and shows no invented power state");
        widget.setConnected(true);
        check(power->isEnabled() && !operate->isEnabled(), "An open silent port permits explicit wake but no Operate");
        power->click();
        check(powers.size() == 1 && powers[0][0].toBool() && !widget.m_status.powerOn && power->text() == QStringLiteral("—"), "Wake intent does not assert hardware power state");
        widget.setResponding(true);
        Kpa500::Status state;
        state.band = 3;
        widget.applyStatus(state);
        check(!operate->isEnabled() && widget.m_state->text().contains("UNKNOWN"), "Partial recovery does not fabricate operating state");
        state.powerOn = true;
        state.operate = false;
        state.forwardPowerW = 0.0f;
        state.paTemperatureC = 34.0f;
        state.paVoltageV = 68.1f;
        state.paCurrentA = 0.0f;
        state.fanMinSpeed = 0;
        state.faultCode = 0;
        state.speakerOn = true;
        state.firmwareVersion = "01.54";
        state.serialNumber = "01199";
        widget.applyStatus(state);
        widget.setConnectionDescription("amp.example.test:2000 · raw TCP; baud configured on server");
        check(widget.m_link->text().contains("amp.example.test:2000")
              && widget.m_identity->text().contains("01.54") && widget.m_identity->text().contains("01199"),
              "Settings reports network endpoint and amplifier identity from readback");
        check(widget.m_state->text() == "AMP ON · STANDBY" && widget.m_readouts[1]->text() == "SWR NO RF", "Standby shows zero power and unmeasured SWR");
        check(modes.isEmpty() && bands.isEmpty() && fans.isEmpty() && speakers.isEmpty(), "Status readback emits no hardware setters");
        QSignalSpy advancedRequests(&widget, &Kpa500Applet::advancedSettingsRequested);
        widget.findChild<QPushButton*>("kpaAdvancedSettings")->click();
        check(advancedRequests.size() == 1, "Advanced shortcut requests the shared Peripherals panel");
        const int priorPowerRequests = powers.size();
        widget.setMaintenanceBusy(true);
        check(!power->isEnabled() && !operate->isEnabled() && !widget.m_fan->isEnabled()
              && !speaker->isEnabled() && widget.m_state->text().contains("MAINTENANCE"),
              "Maintenance disables hardware controls with an explicit state");
        power->click();
        check(powers.size() == priorPowerRequests, "Maintenance-disabled power cannot emit a hardware request");
        widget.setMaintenanceBusy(false);
        check(power->isEnabled() && operate->isEnabled(), "Maintenance completion restores readback-qualified controls");
        widget.m_units->setChecked(true);
        check(widget.m_readouts[2]->text().contains("93") && std::abs(widget.m_gauges[2]->value() - 93.2f) < 0.01f,
              "Fahrenheit updates the readout and gauge together");
        widget.m_units->setChecked(false);
        power->click();
        check(powers.size() == 2 && !powers[1][0].toBool() && widget.m_state->text().contains("AMP ON")
              && power->text() == "ON" && power->isChecked() && power->toolTip().contains("turn the amplifier off"),
              "Power button stays ON until an OFF request is confirmed");
        operate->click();
        check(modes.size() == 1 && !operate->isChecked(), "Operate button retains confirmed Standby until readback");
        follow->setChecked(false);
        check(band->isEnabled(), "Manual mode unlocks band selection");
        band->setCurrentIndex(5);
        QMetaObject::invokeMethod(band, "activated", Q_ARG(int, 5));
        check(bands.size() == 1 && bands[0][0].toInt() == 5 && band->currentIndex() == 3, "Band request keeps confirmed dropdown until readback");
        speaker->click();
        check(speakers.size() == 1 && !speakers[0][0].toBool() && speaker->isChecked(), "Speaker checkbox also waits for hardware");
        state.operate = true;
        widget.applyStatus(state);
        const QString idleBannerStyle = widget.m_state->styleSheet();
        widget.setRadioTransmitting(true);
        check(!power->isEnabled() && !operate->isEnabled(), "Radio TX still disables power and operating controls");
        check(widget.m_state->text() == "AMP ON · OPERATE"
              && widget.m_state->styleSheet() == idleBannerStyle,
              "Radio TX with zero amplifier power cannot claim amplifier activity or paint its banner red");
        const int beforePowerRequests = powers.size();
        const int beforeModeRequests = modes.size();
        state.forwardPowerW = 100.0f;
        widget.applyStatus(state);
        check(widget.m_state->text() == "AMP ON · OPERATE · RF ACTIVE",
              "Amplifier forward-power readback establishes RF activity");
        widget.setRadioTransmitting(false);
        check(widget.m_state->text().endsWith("RF ACTIVE") && !power->isEnabled(),
              "Amplifier RF activity persists independently of the radio TX flag");
        state.forwardPowerW = 0.0f;
        widget.applyStatus(state);
        check(widget.m_state->text() == "AMP ON · OPERATE" && power->isEnabled()
              && widget.m_state->styleSheet() == idleBannerStyle,
              "A zero-power reply clears amplifier RF activity and restores its idle color");
        state.forwardPowerW.reset();
        widget.setRadioTransmitting(true);
        widget.applyStatus(state);
        check(widget.m_state->text() == "AMP ON · OPERATE",
              "Radio TX with unknown amplifier power does not fabricate amplifier activity");
        check(powers.size() == beforePowerRequests && modes.size() == beforeModeRequests,
              "Radio TX and RF-status changes do not emit amplifier power or mode requests");
        widget.setRadioTransmitting(false);
        state.forwardPowerW = 0.0f;
        state.operate = false;
        widget.applyStatus(state);
        follow->setChecked(true);
        widget.setBandTrackingText("Following TX B · 14.119100 MHz → 20m (queued until RX)");
        check(widget.m_tracking->text() == "40m · Follow TX" && !band->isVisible()
              && widget.m_tracking->toolTip().contains("queued until RX")
              && widget.m_trackingDetails->text().contains("14.119100"),
              "Header keeps confirmed band separate from the follow target and preserves full tracking details");
        check(widget.m_driveGauge->isVisible() && widget.m_driveReadout->isVisible(),
              "Full exposes the DRV gauge and pill");
        const int fullHeight = widget.minimumSizeHint().height();
        widget.m_miniButton->setChecked(true);
        QTest::qWait(10);
        check(widget.minimumSizeHint().height() * 2 <= fullHeight,
              "Mini occupies at most half the collapsed full layout height");
        check(!widget.m_driveGauge->isVisible() && !widget.m_driveReadout->isVisible(),
              "Mini hides the entire DRV row");
        check(!widget.m_toolsPane->isVisible() && !operate->isVisible()
              && widget.m_readouts[0]->isVisible() && widget.m_readouts[1]->isVisible()
              && widget.m_state->isVisible() && power->isVisible(),
              "Mini retains power, status and two meters without mode or drawer buttons");
        widget.m_settingsButton->setChecked(true);
        widget.m_debugButton->setChecked(true);
        check(!widget.m_settingsPane->isVisible(), "Opening Mini keeps expanded Settings and Debug out of the compact view");
        widget.m_miniButton->setChecked(false);
        check(widget.m_settingsPane->isVisible() && widget.m_debugPane->isVisible(),
              "Full restores the user's expanded Settings and Debug");
        widget.m_settingsButton->setChecked(false);
        widget.m_debugButton->setChecked(false);
        widget.m_miniButton->setChecked(true);
        state.swr = 5.0f;
        widget.applyStatus(state);
        widget.m_gauges[1]->setValueImmediate(5.0f);
        check(std::abs(widget.m_gauges[1]->filledFraction() - 1.0f) < 0.001f,
              "KPA500 SWR reaches full scale at the amplifier's 5:1 limit");
        state.swr = 3.0f;
        widget.applyStatus(state);
        widget.m_gauges[1]->setValueImmediate(3.0f);
        check(std::abs(widget.m_gauges[1]->filledFraction() - 0.5f) < 0.001f,
              "KPA500 SWR remains 1-5 instead of inheriting the tuner's shorter scale");
        state.swr.reset();
        widget.applyStatus(state);
        check(widget.m_gauges[1]->filledFraction() == 0.0f,
              "Unmeasured SWR immediately clears the fill instead of retaining a stale reading");
        state.faultCode = 4;
        widget.applyStatus(state);
        check(!widget.m_detail->isVisible() && widget.m_fault->isVisible() && widget.m_clear->isEnabled(), "Mini retains the active fault; Full exposes its qualified Clear action");
        widget.recordTelemetry("WS", state);
        widget.recordTelemetry("TM", state);
        widget.recordTelemetry("VI", state);
        check(widget.m_history[0].size() == 1 && widget.m_history[2].size() == 1, "History collects while mini and collapsed");
        for (int i = 0; i < 5000; ++i) { widget.recordTelemetry("WS", state); widget.appendDiagnostic(QString::number(i)); }
        check(widget.m_history[0].size() == 4096 && widget.m_debugLines.size() == 2000, "History and debug buffers stay bounded");
        widget.setResponding(false);
        check(!widget.m_history[0].back().value && !widget.m_status.hasAnyField(), "Loss of replies inserts history gaps and clears readouts");
        widget.m_settingsButton->setChecked(true);
        widget.m_debugButton->setChecked(true);
        widget.appendDiagnostic("POLL quiet mode; checking power every 5 seconds");
        widget.appendDiagnostic("TX ^ON;");
        widget.appendDiagnostic("RX ^ON0;");
        widget.appendDiagnostic("DECODE ON = 0");
        widget.appendDiagnostic("TX ^ON0;");
        widget.appendDiagnostic("REJECT malformed reply");
        check(!widget.debugText().contains("TX ^ON;") && !widget.debugText().contains("RX ^ON0;")
              && widget.debugText().contains("TX ^ON0;") && widget.debugText().contains("REJECT malformed")
              && widget.debugText().contains("POLL quiet"), "Default debug keeps commands and events without polling chatter");
        widget.m_verbose->setChecked(true);
        check(widget.debugText().contains("TX ^ON;") && widget.debugText().contains("RX ^ON0;")
              && widget.debugText().contains("DECODE ON"), "Verbose includes raw serial and decoded polling history");
        for (int i = 0; i < 2500; ++i) { widget.appendDiagnostic("RX ^ON0;"); }
        check(widget.m_verboseDebugLines.size() == 2000 && widget.m_debugLines.back().contains("REJECT malformed"),
              "Verbose polling cannot displace the event history");
        widget.m_verbose->setChecked(false);
        check(!widget.m_debug->toPlainText().contains("TX ^ON;"), "Disabling Verbose immediately restores the quiet view");
        widget.m_pause->setChecked(true);
        const QString frozen = widget.m_debug->toPlainText();
        widget.appendDiagnostic("arrived while paused");
        check(widget.m_debug->toPlainText() == frozen && widget.debugText().contains("arrived while paused"), "Pause freezes display while capture continues");
        widget.m_pause->setChecked(false);
        check(widget.m_debug->toPlainText().contains("arrived while paused"), "Resume catches up from bounded buffer");
        widget.m_follow->setChecked(true);
        const QJsonObject saved = QJsonDocument::fromJson(AppSettings::instance().value("Kpa500Applet").toString().toUtf8()).object();
        check(saved.value("FollowTx").toBool() && !saved.contains("Band") && !saved.contains("Power"), "Only presentation and follow preferences persist");

        widget.setResponding(true);
        state.powerOn = true; state.operate = true;
        widget.applyStatus(state);
        QTest::qWait(10);
        auto& theme = ThemeManager::instance();
        const auto buttonFill = [](QPushButton* button) {
            const QPixmap pixmap = button->grab();
            const int inset = qRound(6 * pixmap.devicePixelRatio());
            return pixmap.toImage().pixelColor(inset, inset);
        };
        check(buttonFill(operate) == theme.color("color.accessory.key.meffa.active.background"),
              "Operate uses PGXL active green");
        widget.setRadioTransmitting(true);
        QTest::qWait(10);
        check(buttonFill(operate) == theme.color(operate, "color.background.2"),
              "Disabled active controls lose their active fill");
        widget.setRadioTransmitting(false);
        state.operate = false; widget.applyStatus(state);
        QTest::qWait(10);
        check(buttonFill(widget.m_standby) == theme.color("color.accessory.key.standby.background"),
              "Standby uses PGXL amber");
        state.powerOn = false; widget.applyStatus(state);
        check(!operate->isEnabled() && !widget.m_fan->isEnabled() && !power->isChecked() && widget.m_state->text() == "AMP OFF" && power->text() == "OFF",
              "Confirmed OFF disables operating controls and clears their active indication");
        power->click();
        check(powers.size() == 3 && powers[2][0].toBool() && power->text() == "OFF"
              && power->toolTip().contains("wake the amplifier"), "OFF shows confirmed state while clicking requests ON");
        state.powerOn = true;

        state.faultCode = 0;
        widget.applyStatus(state);
        widget.m_miniButton->setChecked(false);
        widget.m_settingsButton->setChecked(false);
        widget.setFloating(true);
        widget.m_historyButton->setChecked(false);
        widget.resize(380, 650);
        QTest::qWait(30);
        const auto topPositions = [&widget] {
            QList<int> positions;
            for (QWidget* item : std::array<QWidget*, 6>{widget.m_power, widget.m_readouts[0],
                    widget.m_readouts[2], widget.m_readouts[4], widget.m_tracking, widget.m_historyButton}) {
                positions.append(item->mapTo(widget.m_content, QPoint()).y());
            }
            return positions;
        };
        const QList<int> compactPositions = topPositions();
        const int narrowGaugeWidth = widget.m_gauges[0]->width();
        widget.resize(380, 950);
        QTest::qWait(30);
        check(topPositions() == compactPositions, "Resizing keeps controls and meter rows together at the top");
        widget.resize(600, 950);
        QTest::qWait(30);
        check(widget.m_gauges[0]->width() > narrowGaugeWidth, "Realtime bars grow with the available width");
        widget.m_historyButton->setChecked(true);
        QTest::qWait(30);
        const QSize largeGraph = widget.m_graph->size();
        widget.resize(380, 700);
        QTest::qWait(30);
        check(largeGraph.width() > widget.m_graph->width() && largeGraph.height() > widget.m_graph->height(),
              "History graphs grow in both dimensions with the window");
        check(widget.m_graph->pixmap().deviceIndependentSize().toSize() == widget.m_graph->size(),
              "Graph rendering follows its resized drawing area");
        widget.m_settingsButton->setChecked(true);
        widget.m_debugButton->setChecked(true);
        QWidget popout;
        QVBoxLayout popoutLayout(&popout);
        popoutLayout.setContentsMargins(0, 0, 0, 0);
        popoutLayout.addWidget(&widget);
        popout.show();
        popout.resize(300, 400);
        QTest::qWait(30);
        QScrollBar* scroll = widget.m_scroll->verticalScrollBar();
        check(widget.height() == 400 && scroll->isVisible() && scroll->maximum() > 0,
              "Expanded History and Debug scroll in a small popout without forcing its height");
        check(widget.m_content->width() <= widget.m_scroll->viewport()->width()
              && widget.m_scroll->horizontalScrollBar()->maximum() == 0,
              "Small popout content fits horizontally beside the scrollbar");
        scroll->setValue(scroll->maximum());
        const QPoint debugBottom = widget.m_debug->mapTo(widget.m_scroll->viewport(), widget.m_debug->rect().bottomLeft());
        check(debugBottom.y() >= 0 && debugBottom.y() < widget.m_scroll->viewport()->height(),
              "Scrolling reaches the bottom of Debug");
        check(widget.m_settingsPane->isAncestorOf(widget.m_historyHelp)
              && !widget.m_historyPane->isAncestorOf(widget.m_historyHelp),
              "History explanation lives in Settings instead of below the graphs");
        popout.resize(600, 1600);
        QTest::qWait(30);
        check(scroll->maximum() == 0 && !scroll->isVisible(), "Scrollbar disappears when expanded content fits");
        widget.setParent(nullptr);
        widget.show();
        popout.close();
        widget.m_historyButton->setChecked(false);
        widget.m_settingsButton->setChecked(false);
        QTest::qWait(30);
        widget.resize(380, 950);
        QTest::qWait(30);
        check(topPositions() == compactPositions, "Collapsing expanded panes preserves compact top spacing");

        if (qEnvironmentVariableIsSet("KPA_REPLAY_SCREENSHOTS")) {
            widget.setFloating(false);
            widget.setWindowTitle("OFFLINE REPLAY — no hardware connection");
            widget.setResponding(true);
            state.faultCode = 0;
            widget.applyStatus(state);
            widget.setConnectionDescription("OFFLINE REPLAY · no serial port");
            widget.setBandTrackingText("Following TX A · 7.100000 MHz → 40m");
            widget.m_settingsButton->setChecked(false);
            widget.m_debugButton->setChecked(false);
            widget.m_miniButton->setChecked(false);
            widget.m_historyButton->setChecked(false);
            widget.resize(254, widget.minimumSizeHint().height());
            QTest::qWait(50);
            widget.grab().save("kpa500-full-replay.png");
            radioMeters.defineMeter(forward);
            radioMeters.updateValueByName("TX", "FWDPWR", 25.0f);
            widget.updateRadioDrive(radioMeters, true);
            QTest::qWait(250);
            widget.grab().save("kpa500-drv-synthetic-replay.png");
            widget.m_miniButton->setChecked(true);
            widget.resize(254, widget.minimumSizeHint().height());
            QTest::qWait(50);
            widget.grab().save("kpa500-mini-replay.png");
            widget.m_miniButton->setChecked(false);
            widget.m_historyButton->setChecked(true);
            const qint64 now = widget.m_clock.elapsed();
            for (auto& series : widget.m_history) { series.clear(); }
            for (int i = 0; i < 300; ++i) {
                const float envelope = (i % 55) < 34 ? std::abs(std::sin(i * 0.23f)) : 0;
                const qint64 time = now - (300 - i) * 1000;
                widget.m_history[0].push_back({time, 450 * envelope});
                widget.m_history[1].push_back({time, envelope > 0 ? std::optional<float>(1.2f + 0.3f * envelope) : std::nullopt});
                widget.m_history[2].push_back({time, 34.0f + i * 0.04f});
                widget.m_history[3].push_back({time, 68.1f - 3 * envelope});
                widget.m_history[4].push_back({time, 12 * envelope});
            }
            widget.resize(550, widget.minimumSizeHint().height());
            widget.renderHistory();
            QTest::qWait(50);
            widget.grab().save("kpa500-history-synthetic-replay.png");
            widget.m_historyButton->setChecked(false);
            widget.m_settingsButton->setChecked(true);
            widget.m_debugButton->setChecked(true);
            widget.appendDiagnostic("OFFLINE REPLAY: ^WS000 000; = 0 W; SWR unmeasured");
            widget.resize(550, widget.minimumSizeHint().height());
            QTest::qWait(50);
            widget.grab().save("kpa500-debug-replay.png");
            widget.setFloating(true);
            widget.m_historyButton->setChecked(false);
            widget.m_settingsButton->setChecked(false);
            widget.resize(380, 650);
            QTest::qWait(50);
            widget.grab().save("kpa500-tall-replay.png");
            widget.m_historyButton->setChecked(true);
            widget.resize(550, 1000);
            QTest::qWait(50);
            widget.grab().save("kpa500-resized-history-replay.png");
            widget.m_settingsButton->setChecked(true);
            widget.resize(300, 500);
            QTest::qWait(50);
            widget.m_scroll->verticalScrollBar()->setValue(0);
            widget.grab().save("kpa500-scroll-top-replay.png");
            widget.m_scroll->verticalScrollBar()->setValue(widget.m_scroll->verticalScrollBar()->maximum());
            QTest::qWait(50);
            widget.grab().save("kpa500-scroll-bottom-replay.png");
        }
        return failed ? 1 : 0;
    }
};
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("kpa500-applet-test"));
    QApplication app(argc, argv);
    if (!profile.isValid()) { return 1; }
    AetherSDR::AppSettings::instance().load();
    return AetherSDR::Kpa500AppletTest::run();
}
