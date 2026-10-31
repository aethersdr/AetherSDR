#include "TestSettingsProfile.h"
#include "core/TxKeyingMarker.h"
#include "core/ThemeManager.h"
#include <QPalette>
#include "gui/TxApplet.h"
#include "gui/AtuPreTuneDialog.h"
#include "models/Kat500Model.h"
#include "models/Kat500TxController.h"
#include "models/RadioModel.h"
#include "models/BandPlanManager.h"
#include "models/SliceModel.h"
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QAction>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTimer>
#include <QTest>
#include <cstdio>

using namespace AetherSDR;
namespace AetherSDR {
class TxAppletPowerReconciliationTestAccess {
public:
    static void prepare(AtuPreTuneDialog& dialog, const std::shared_ptr<TxController>& input) {
        dialog.m_programController = input;
        dialog.m_programInput = input->captureProgram(dialog.usesKat500()
            ? TxController::Activity::Tune : TxController::Activity::Atu);
        dialog.m_sweepActive = true;
        dialog.m_currentIndex = 0;
        dialog.m_txSliceId = 0;
        dialog.m_points = {{"20m", 14.1, 1, 1, 14.0, 14.35}};
        dialog.requestTuneNow();
    }
    static QVector<double> centers(const AtuPreTuneDialog& dialog, const QString& name) {
        for (const auto& row : dialog.m_bands) { if (row.name == name) { return row.centers; } }
        return {};
    }
    static void twoPoints(AtuPreTuneDialog& dialog) {
        dialog.m_mode = AtuPreTuneDialog::Mode::Auto;
        dialog.m_points.append({"20m", 14.12, 2, 2, 14.0, 14.35});
    }
    static void pause(AtuPreTuneDialog& dialog) {
        dialog.m_settleTimer->stop();
        dialog.m_waitingForAtu = false;
    }
    static void settle(AtuPreTuneDialog& dialog) {
        dialog.m_settleTimer->stop();
        QMetaObject::invokeMethod(dialog.m_settleTimer, "timeout", Qt::DirectConnection);
    }
    static void cancel(AtuPreTuneDialog& dialog) { dialog.cancelProgram(); }
    static bool active(const AtuPreTuneDialog& dialog) { return dialog.m_sweepActive; }
    static int successes(const AtuPreTuneDialog& dialog) { return dialog.m_successCount; }
};
// Inject only the serial transport/clock. Production parsing and operations run unchanged.
class Kat500ModelTest {
public:
    static void ready(Kat500Model& k, QList<QByteArray>& wire) {
        k.m_timer.stop(); k.m_now=[] { return qint64(0); };
        k.m_write=[&wire](const QByteArray& bytes) { wire.append(bytes); return true; };
        k.m_nextPoll=60000;
        k.m_connected=true; k.m_detected=true; k.m_lastIo=k.m_lastReply=0;
        k.m_status.identity="KAT500"; k.m_status.serial="00222"; k.m_status.firmware="02.16";
        k.m_status.powerOn=true; k.m_status.tuning=false; k.m_status.forwardAdc=0;
        k.m_status.mode=Kat500::Mode::Auto; k.m_status.faultCode=0; k.m_status.antenna=1;
        for(const QString& field : {"TP","VFWD","MD","PS","FLT"}) { k.m_received[field]=0; }
    }
    static void mode(Kat500Model& k, Kat500::Mode value) {
        k.m_status.mode=value; emit k.statusUpdated(k.m_status);
    }
    static void step(Kat500Model& k, const QByteArray& reply={}) {
        k.tick(); if(!reply.isEmpty()) { k.receiveBytes(reply); }
    }
    static void observe(Kat500Model& k, const QByteArray& query, const QByteArray& reply) { k.query(query); step(k,reply); }
    static void frame(Kat500Model& k, const QByteArray& reply) { k.receiveBytes(reply); }
    static void advance(Kat500Model& k, qint64 now) { k.m_now=[now] { return now; }; k.m_lastIo=now; k.m_lastReply=now; }
    static void drop(Kat500Model& k) { k.transportClosed(); }
    static void stale(Kat500Model& k) {
        k.m_now=[] { return qint64(6000); };
        emit k.availabilityChanged();
    }
    static void reopen(Kat500Model& k) {
        k.transportOpened(); k.m_timer.stop();
    }
};
}
namespace {
int failures=0;
void check(bool ok, const char* what) { std::printf("[%s] %s\n",ok?"PASS":"FAIL",what); failures+=!ok; }
class Backend final : public IRadioBackend {
public:
    QStringList wire;
    bool connected=false;
    RadioCapabilities capabilities() const override { RadioCapabilities c; c.canTransmit=true; c.hasTuner=true; return c; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int,double) override {}
    void setSliceMode(int,const QString&) override {}
    void setSliceFilter(int,int,int) override {}
    void setSliceAgc(int,const QString&,int) override {}
    void setPanCenter(const QString&,double,PanCenterIntent) override {}
    void setKeying(bool,const TxCoordinator::Operation&,const TxCoordinator::Completion&) override {}
    void setAtu(bool on,const TxCoordinator::Operation&,const TxCoordinator::Completion&) override { wire << (on?"atu:on":"atu:off"); }
    void setTune(bool on,int,const TxCoordinator::Operation&,const TxCoordinator::Completion&) override { wire << (on?"tune:on":"tune:off"); }
    void invokeExtension(const QString&,const QString&,quint64,const QVariant&) override {}
};
struct Fixture {
    RadioModel radio;
    Kat500Model kat;
    QList<QByteArray> serial;
    Backend* backend;
    std::shared_ptr<TxController> input;
    std::unique_ptr<Kat500TxController> routing;
    Fixture() {
        auto b=std::make_unique<Backend>(); backend=b.get(); radio.setBackendForTest(std::move(b),"test");
        check(radio.automationApplySliceFixture(0,"A"),"socket-free TX slice fixture");
        SliceDelta slice; slice.txSlice=true; slice.mode="USB"; slice.panId="0x40000000"; slice.frequency=14.1;
        radio.slice(0)->applyChanges(slice);
        backend->connected=true;
        radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
        radio.transmitModel().setHasTuner(true);
        TransmitDelta d; d.transmitFreq=14.1; d.tunePower=3; d.atuEnabled=true; d.atuStatusRaw="TUNE_SUCCESSFUL";
        radio.transmitModel().applyChanges(d);
        Kat500ModelTest::ready(kat,serial);
        routing=std::make_unique<Kat500TxController>(&radio,&kat);
        input=std::make_shared<TxController>(&radio);
    }
    void bypass() { TransmitDelta d; d.atuStatusRaw="TUNE_BYPASS"; radio.transmitModel().applyChanges(d); }
    bool start() { return routing->start(input->capture(TxController::Activity::Tune)); }
    void readResult() {
        for (const QByteArray& reply : {QByteArray("MDA;"), QByteArray("AN1;"), QByteArray("BYPN;"),
             QByteArray("SIDET;"), QByteArray("C02;"), QByteArray("L03;"), QByteArray("VSWR1.33;"), QByteArray("FLT0;"),
             QByteArray("TP0;"), QByteArray("VFWD0;")}) {
            Kat500ModelTest::step(kat, reply);
        }
    }
    void acknowledgeSearch() {
        Kat500ModelTest::step(kat,"F 14100;");
        Kat500ModelTest::step(kat,"TP1;");
    }
};
void routingAndCycle() {
    Fixture f;
    check(f.routing->selected(),"confirmed Auto is eligible for dedicated pre-tune");
    check(f.backend->wire.isEmpty(),"opening external pre-tune leaves the internal ATU untouched");
    check(f.start(),"explicit click captures a tune request");
    Kat500ModelTest::step(f.kat);
    check(f.serial.isEmpty() && !f.backend->wire.contains("tune:on"),"wait for confirmed internal bypass before tuner or RF start");
    f.bypass(); f.acknowledgeSearch();
    check(f.serial.contains("F 14100;F;") && f.serial.contains("T;TP;"),"tune targets current TX frequency then full search");
    check(f.backend->wire.count("tune:on")==1 && !f.backend->wire.contains("atu:on"),"one captured radio carrier; internal ATU never starts");
    check(f.kat.status().mode==Kat500::Mode::Auto,"fresh tune preserves Auto mode");
    QTest::qWait(220);
    check(f.routing->busy() && f.radio.transmitModel().isTuning()
          && !f.backend->wire.contains("tune:off"),
          "watchdog retains the carrier when the backend provides no Tune readback");
    Kat500ModelTest::frame(f.kat,"FT;");
    check(f.backend->wire.count("tune:off")==1,"completion stops its carrier immediately");
    f.readResult();
    check(!f.routing->busy() && f.routing->matched(), "post-tune relay readback confirms the new match");
    check(f.radio.transmitModel().tunePower()==3,"tune does not raise operator drive power");
    Kat500ModelTest::mode(f.kat,Kat500::Mode::Bypass);
    check(!f.routing->selected(),"explicit Bypass restores internal route");
    Kat500ModelTest::mode(f.kat,Kat500::Mode::Manual);
    check(f.routing->selected(),"Manual selects external route");
}
void cancellationAndFences() {
    {
        Fixture f;
        Kat500ModelTest::stale(f.kat);
        check(f.routing->selected() && !f.routing->unavailableReason().isEmpty(),
            "stale readback on an open connection blocks actions without changing tuner route");
        Kat500ModelTest::drop(f.kat);
        check(!f.routing->selected(),"closing a stale connection restores the radio route");
        Kat500ModelTest::reopen(f.kat);
        check(!f.routing->selected(),"reconnect waits for fresh tuner mode before selecting KAT500");
    }
    {
        Fixture f; f.bypass(); f.start(); f.acknowledgeSearch(); f.routing->cancel(); f.routing->cancel();
        Kat500ModelTest::step(f.kat);
        Kat500ModelTest::advance(f.kat,2100); Kat500ModelTest::step(f.kat);
        Kat500ModelTest::advance(f.kat,4200); Kat500ModelTest::step(f.kat);
        check(f.serial.count("CT;TP;")==1 && !f.routing->busy(), "missing cancellation acknowledgement is terminal without CT retries");
    }
    {
        Fixture f; f.bypass(); f.start();
        const auto competing = f.input->capture(TxController::Activity::Mox);
        check(competing.start(), "offline competing MOX starts");
        f.acknowledgeSearch();
        check(!f.serial.contains("T;TP;") && !f.backend->wire.contains("tune:on"), "competing transmission cancels tune preflight");
        competing.stop();
    }
    {
        Fixture f; f.bypass(); f.start();
        Kat500ModelTest::step(f.kat,"F 14100;");
        Kat500ModelTest::step(f.kat);
        check(f.serial.contains("T;TP;"), "pending tuner request was dispatched");
        f.routing.reset();
        Kat500ModelTest::advance(f.kat,2100);
        Kat500ModelTest::step(f.kat);
        check(f.serial.contains("CT;TP;") && !f.backend->wire.contains("tune:on"), "controller destruction retains tuner cancellation after late replies drain");
    }
    {
        Fixture f; f.bypass(); f.start(); f.acknowledgeSearch();
        Kat500ModelTest::frame(f.kat,"FT;");
        f.readResult();
        check(f.routing->matched(), "confirmed completion exposes successful match");
        Kat500ModelTest::observe(f.kat,"AN;","AN2;");
        check(!f.routing->matched(), "antenna change clears previous match success");
    }
    {
        Fixture f; f.start(); f.routing->cancel(); f.bypass();
        Kat500ModelTest::step(f.kat,"TP1;");
        check(!f.backend->wire.contains("tune:on") && f.serial.isEmpty(),"cancel before bypass readback prevents all delayed tune work");
    }
    {
        Fixture f; f.bypass(); f.start(); f.input->invalidate();
        f.acknowledgeSearch();
        check(!f.backend->wire.contains("tune:on") && !f.serial.contains("T;TP;"),"revoked input prevents queued tuner search and carrier");
    }
    {
        Fixture f; f.bypass(); f.start(); f.acknowledgeSearch();
        Kat500ModelTest::drop(f.kat);
        check(f.backend->wire.contains("tune:off") && !f.routing->busy(),"disconnect stops carrier");
        check(!f.routing->selected(),"closed tuner connection releases the external route");
        check(!f.backend->wire.contains("atu:on"),"disconnect never starts the internal ATU");
    }
    {
        Fixture f; f.bypass(); f.start(); f.acknowledgeSearch();
        Kat500ModelTest::mode(f.kat,Kat500::Mode::Bypass);
        check(f.backend->wire.contains("tune:off") && !f.routing->selected(),"mode change cancels owned carrier before returning internal route");
    }
    {
        Fixture f; f.bypass(); f.start();
        auto* deadline=f.routing->findChild<QTimer*>("katTxDeadline");
        check(deadline,"bounded tune deadline exists");
        if(deadline) { QMetaObject::invokeMethod(deadline,"timeout",Qt::DirectConnection); }
        f.acknowledgeSearch();
        check(!f.backend->wire.contains("tune:on"),"timeout cannot be revived by a late tuner reply");
    }
}
QPushButton* button(TxApplet& a,const QString& name) {
    for(auto* b:a.findChildren<QPushButton*>()) { if(b->accessibleName()==name) { return b; } }
    return nullptr;
}
void nativeSweepRouteFence() {
    Fixture f;
    AtuPreTuneDialog dialog(&f.radio, nullptr);
    check(!dialog.usesKat500(), "radio sweep never adopts a connected external tuner");
    TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
    Kat500ModelTest::mode(f.kat, Kat500::Mode::Bypass);
    Kat500ModelTest::mode(f.kat, Kat500::Mode::Auto);
    TxAppletPowerReconciliationTestAccess::settle(dialog);
    check(f.backend->wire.contains("atu:on") && f.serial.isEmpty(),
          "external mode changes leave native radio sweep routing unchanged");
}
void externalSweep() {
    {
        Fixture f;
        BandPlanManager plan;
        plan.setSegmentsForTest({{1.8,1.83,"","",{}}, {14.0,14.06,"","",{}},
            {28.0,28.3,"","",{}}, {50.0,50.4,"","",{}}, {5.3306,5.3334,"","",{}}});
        AtuPreTuneDialog dialog(&f.radio, &plan, nullptr, f.routing.get());
        const auto centers = [&dialog](const QString& band) { return TxAppletPowerReconciliationTestAccess::centers(dialog, band); };
        check(centers("160m") == QVector<double>{1.805,1.815,1.825}
            && centers("20m") == QVector<double>{14.010,14.030,14.050}
            && centers("10m") == QVector<double>{28.050,28.150,28.250}
            && centers("6m") == QVector<double>{50.100,50.300}, "sweep points cover KAT500 bins with documented band-specific widths");
        check(centers("60m") == QVector<double>{5.332}, "narrow legal channel uses its midpoint without crossing a gap");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        TxAppletPowerReconciliationTestAccess::twoPoints(dialog);
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        f.acknowledgeSearch();
        Kat500ModelTest::frame(f.kat,"FT;"); f.readResult(); QCoreApplication::processEvents();
        TransmitDelta delta; delta.transmitFreq=14.12; f.radio.transmitModel().applyChanges(delta);
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        Kat500ModelTest::step(f.kat,"F 14120;"); Kat500ModelTest::step(f.kat,"TP1;");
        check(f.backend->wire.count("tune:on") == 2,
            "two-point sweep refreshes idle/RF state before starting the next frequency");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        TxAppletPowerReconciliationTestAccess::pause(dialog);
        Kat500ModelTest::drop(f.kat); QCoreApplication::processEvents();
        check(!TxAppletPowerReconciliationTestAccess::active(dialog),
            "disconnect invalidates a sweep paused between points");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        auto other = std::make_shared<TxController>(&f.radio);
        check(f.routing->start(other->capture(TxController::Activity::Tune)), "independent operator tune starts");
        f.acknowledgeSearch();
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        Kat500ModelTest::step(f.kat);
        check(f.routing->busy() && !f.serial.contains("CT;TP;") && !f.backend->wire.contains("tune:off"),
            "refused sweep point does not cancel a separate operator tune");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        f.acknowledgeSearch();
        check(f.serial.contains("T;TP;") && f.backend->wire.contains("tune:on")
            && !f.backend->wire.contains("atu:on"), "external sweep drives KAT500 search with radio carrier only");
        Kat500ModelTest::frame(f.kat,"FT;");
        QCoreApplication::processEvents();
        check(TxAppletPowerReconciliationTestAccess::active(dialog), "sweep waits for match readback after search ends");
        f.readResult(); QCoreApplication::processEvents();
        check(!TxAppletPowerReconciliationTestAccess::active(dialog)
            && TxAppletPowerReconciliationTestAccess::successes(dialog) == 1,
            "KAT500 result completes the sweep without internal ATU status");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        f.acknowledgeSearch();
        TxAppletPowerReconciliationTestAccess::cancel(dialog);
        Kat500ModelTest::step(f.kat,"TP0;");
        check(f.backend->wire.contains("tune:off") && f.serial.contains("CT;TP;"),
            "aborting sweep stops its carrier and cancels the external search");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        f.routing.reset();
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        check(!f.backend->wire.contains("atu:on") && !f.backend->wire.contains("tune:on"),
            "destroyed external route cannot fall back to internal ATU");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        TransmitDelta delta; delta.transmitFreq=7.1; f.radio.transmitModel().applyChanges(delta);
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        Kat500ModelTest::step(f.kat);
        check(f.serial.isEmpty() && !f.backend->wire.contains("tune:on"),
            "sweep rejects a TX frequency that differs from the planned point");
    }
}
void contextMenu() {
    Fixture f;
    TxApplet applet;
    applet.setRadioModel(&f.radio);
    applet.setTransmitModel(&f.radio.transmitModel());
    QMenu menu;
    applet.buildAtuContextMenu(menu);
    check(menu.actions().size() == 2, "radio retains pre-tune and erase context actions");
    for (QAction* action : menu.actions()) {
        check(!action->text().contains("KAT500"), "TX Controls context menu stays radio-side");
    }
}
void presentation() {
    Fixture f;
    TxApplet applet;
    applet.setRadioModel(&f.radio);
    applet.setTransmitModel(&f.radio.transmitModel());
    auto* atu = button(applet, "ATU tune");
    auto* mem = button(applet, "ATU memories");
    check(atu && mem, "radio tuner buttons retain accessible identities");
    if (!atu || !mem) { return; }
    for (const auto mode : {Kat500::Mode::Auto, Kat500::Mode::Manual, Kat500::Mode::Bypass}) {
        Kat500ModelTest::mode(f.kat, mode);
        check(atu->text() == "ATU" && mem->text() == "MEM" && mem->isCheckable(),
              "external tuner mode cannot replace radio ATU/MEM");
    }
    Kat500ModelTest::mode(f.kat, Kat500::Mode::Auto);
    check(f.backend->wire.isEmpty(), "external connection and mode changes never bypass radio ATU");
    f.bypass();
    f.backend->wire.clear();
    const auto click = prepareTxKeyingAction(atu, f.input, "click", {});
    if (click) { click(); }
    check(f.backend->wire.contains("atu:on") && !f.backend->wire.contains("tune:on")
          && f.serial.isEmpty(), "ATU click targets only the radio with an active KAT500 present");
    bool externalPill = false;
    for (auto* label : applet.findChildren<QLabel*>()) { externalPill |= label->text() == "KAT500"; }
    check(!externalPill, "TX Controls has no external tuner status overlay");
}

}
int main(int argc,char** argv) {
    TestSettingsProfile profile("kat500-tx-controls"); if(!profile.isValid()) { return 1; }
    QApplication app(argc,argv);
    routingAndCycle(); cancellationAndFences(); presentation(); contextMenu(); nativeSweepRouteFence(); externalSweep();
    return failures ? 1:0;
}
