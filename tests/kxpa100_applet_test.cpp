#include "gui/Kxpa100Applet.h"
#include "gui/HGauge.h"
#include "TestSettingsProfile.h"
#include "core/ThemeManager.h"
#include "models/Kxpa100Model.h"
#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while (false)
namespace AetherSDR {
class Kxpa100ModelTest {
public:
    static void ready(Kxpa100Model& model) {
        model.m_now=[] { return qint64(100); }; model.m_connected=model.m_detected=true; model.m_lastReply=100;
        auto& s=model.m_status; s.identity="KXPA100"; s.serial="00031"; s.firmware="01.39";
        s.tunerPresent=true; s.tuning=false; s.operate=true; s.inputWatts=0; s.forwardWatts=0;
        s.reflectedWatts=0; s.dissipatedWatts=0; s.temperatureC=31.5; s.supplyVolts=13.3; s.drainAmps=0;
        s.mode=Kxpa100::Mode::Auto; s.antenna=2; s.band=5; s.bypassed=false; s.attenuator=0; s.faultCode='N';
        s.swr=1.3; s.bypassSwr=2.2; s.capacitorMask=128; s.inductorMask=2; s.side=Kxpa100::CapacitorSide::Transmitter;
        for (const QString& key : {"^TP","^PI","^PF","^SW","^TU","^MD","^AN","^OP"}) { model.m_received[key]=100; }
    }
    static int run() {
        Kxpa100Model model; ready(model); QList<QByteArray> wire;
        model.m_write=[&](const QByteArray& bytes) { wire.append(bytes); return true; };
        Kxpa100Applet widget; widget.setModel(&model); widget.resize(260,widget.sizeHint().height()); widget.show(); QTest::qWait(10);
        CHECK(wire.isEmpty());
        auto* drive=dynamic_cast<HGauge*>(widget.findChild<QWidget*>("kxInputGauge"));
        auto* driveLabel=widget.findChild<QLabel*>("kxMeterLabel1");
        CHECK(drive && driveLabel && driveLabel->text() == "DRV 0.0W");
        model.m_status.inputWatts=4.5; model.m_status.forwardWatts=85;
        emit model.statusUpdated(model.m_status);
        CHECK(driveLabel->text() == "DRV 4.5W" && drive->value() == 4.5f);
        CHECK(drive->toolTip().contains("input power") && drive->accessibleDescription().contains("measured by the KXPA100"));
        drive->setValueImmediate(10);
        CHECK(drive->filledFraction() == 1.0f);
        ready(model); emit model.statusUpdated(model.m_status);
        CHECK(wire.isEmpty());
        auto* mini=widget.findChild<QPushButton*>("kxMini"); auto* manual=widget.findChild<QPushButton*>("kxManualToggle");
        auto* settings=widget.findChild<QPushButton*>("kxSettings"); auto* swr=widget.findChild<QPushButton*>("kxSwr");
        CHECK(mini && manual && settings && swr);
        for (auto* b : widget.findChildren<QPushButton*>()) { CHECK(b->height() <= 22); }
        CHECK(widget.findChild<QCheckBox*>("kxCBit7")->text() == "1360");
        CHECK(widget.findChild<QCheckBox*>("kxCBit6")->text() == "660");
        CHECK(!widget.findChild<QPushButton*>("kxAntenna3"));
        CHECK(!widget.findChild<QLabel*>("kxFrequency")->text().contains("0 kHz"));
        auto* scroll=widget.findChild<QScrollArea*>("kxScroll"); CHECK(scroll);
        CHECK(scroll->verticalScrollBar()->maximum() == 0);
        widget.resize(248,widget.minimumSizeHint().height()); QTest::qWait(10);
        CHECK(scroll->verticalScrollBar()->maximum() == 0);
        const int full=widget.sizeHint().height(); mini->click(); QTest::qWait(10);
        CHECK(!drive->isVisible() && !driveLabel->isVisible());
        CHECK(widget.sizeHint().height() <= full * 0.60 && !manual->isVisible() && !settings->isVisible());
        CHECK(widget.findChild<QPushButton*>("kxTune")->isVisible() && swr->isVisible());
        mini->click(); manual->click(); CHECK(manual->text().startsWith("▾")); manual->click(); CHECK(manual->text().startsWith("▸"));
        QTest::mousePress(swr,Qt::LeftButton); CHECK(swr->text().startsWith("BYP"));
        QTest::mouseRelease(swr,Qt::LeftButton); CHECK(swr->text().startsWith("SWR"));
        CHECK(!swr->accessibleDescription().contains("Space") && !swr->toolTip().contains("Space"));
        widget.setRadioContext("Test radio", 14074, true);
        widget.setAssociationResult(false);
        auto* result = widget.findChild<QLabel*>("kxOperationResult");
        CHECK(result && result->text().contains("not saved") && result->accessibleName() == result->text());
        widget.setRadioContext("Test radio", 14074, true);
        CHECK(result->text().contains("not saved"));
        CHECK(widget.findChild<QPushButton*>("kxAssociateRadio")->text() == "Remove radio association");
        widget.setAssociationResult(true);
        CHECK(result->text() == "Radio association updated.");
        model.m_status.tunerPresent=false; emit model.statusUpdated(model.m_status);
        auto* antenna=widget.findChild<QPushButton*>("kxAntenna2");
        CHECK(antenna->isVisible() && !antenna->isEnabled() && !antenna->accessibleDescription().isEmpty());
        CHECK(widget.findChild<QPushButton*>("kxOperate")->isEnabled());
        model.m_status.tunerPresent=true; model.m_status.attenuator=2; emit model.statusUpdated(model.m_status);
        CHECK(!widget.findChild<QPushButton*>("kxAttenuatorStatus")->isEnabled());
        settings->click(); widget.resize(260,700); widget.findChild<QPushButton*>("kxDebug")->click();
        auto* output=widget.findChild<QPlainTextEdit*>("kxDebugOutput"); CHECK(output);
        for (int i=0;i<150;++i) { model.report(QStringLiteral("Diagnostic %1").arg(i)); }
        QTest::qWait(10); CHECK(output->verticalScrollBar()->value()==output->verticalScrollBar()->maximum());
        output->verticalScrollBar()->setValue(0); model.report("Stay at earlier line"); QTest::qWait(10);
        CHECK(output->verticalScrollBar()->value()==0);
        settings->click(); widget.findChild<QPushButton*>("kxHistory")->click(); widget.resize(700,700); QTest::qWait(10);
        auto* graph=widget.findChild<QLabel*>("kxHistoryGraph"); const int wide=graph->width();
        widget.resize(260,500); QTest::qWait(10); CHECK(graph->width() < wide && graph->accessibleDescription().contains("W"));
        return 0;
    }
};
}
int main(int argc,char** argv) {
    TestSettingsProfile profile("kxpa100-applet"); QApplication app(argc,argv);
    return AetherSDR::Kxpa100ModelTest::run();
}
