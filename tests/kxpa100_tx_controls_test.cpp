#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/SettingsDatabase.h"
#include "gui/TxApplet.h"
#include "gui/SharedTxMeterSelection.h"
#include "models/Kxpa100Model.h"
#include "models/Kxpa100RadioAssociation.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QApplication>
#include <QPushButton>
#include <QScopeGuard>
#include <QStringList>
#include <QLabel>
#include <QTimer>
#include <QTest>
#include <cstdio>
using namespace AetherSDR;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while (false)
namespace {
QStringList preferenceWarnings;
void capturePreferenceWarning(QtMsgType type, const QMessageLogContext& context,
                              const QString& message)
{
    if (type == QtWarningMsg && QString::fromLatin1(context.category) == "aether.kxpa100") {
        preferenceWarnings.append(message);
    }
}
}

namespace AetherSDR {
class TxOperationIntegrationTestAccess {
public:
    static void identity(RadioModel& r, const QString& serial = "HL2-TEST") { r.m_lastInfo.serial=serial; emit r.infoChanged(); }
    static void family(RadioModel& r, const QString& family) { r.m_family=family; }
};
class Kxpa100ModelTest {
public:
    static void ready(Kxpa100Model& k, QList<QByteArray>& wire) {
        k.m_timer.stop(); k.m_now=[] { return qint64(100); }; k.m_lastReply=100; k.m_nextPoll=100000;
        k.m_write=[&wire](const QByteArray& bytes) { wire.append(bytes); return true; };
        k.m_connected=k.m_detected=true;
        auto& s=k.m_status; s.identity="KXPA100"; s.firmware="01.39"; s.serial="00031";
        s.tuning=false; s.inputWatts=0; s.forwardWatts=0; s.tunerPresent=true;
        s.mode=Kxpa100::Mode::Auto; s.antenna=2; s.operate=true; s.faultCode='N';
        for (const QString& key : {"^I","^RV","^SN","^TP","^PI","^PF","^TU","^MD","^AN","^OP","^FL"}) { k.m_received[key]=100; }
    }
    static void frame(Kxpa100Model& k,const QByteArray& reply) { k.receiveBytes(reply); }
    static void observe(Kxpa100Model& k,const QByteArray& query,const QByteArray& reply) { k.query(query); step(k,reply); }
    static void step(Kxpa100Model& k,const QByteArray& reply) { k.tick(); k.receiveBytes(reply); }
    static void sendNext(Kxpa100Model& k) { k.tick(); }
    static void preflight(Kxpa100Model& k) {
        for (const QByteArray& s : {QByteArray("^IKXPA100;"),QByteArray("^RV01.39;"),QByteArray("^SN00031;"),QByteArray("^TP0;"),QByteArray("^PI0000;"),QByteArray("^PF0000;")}) { step(k,s); }
    }
    static void stale(Kxpa100Model& k) { k.m_now=[] { return qint64(6100); }; emit k.availabilityChanged(); }
    static void drop(Kxpa100Model& k) { k.transportClosed(); }
    static void match(Kxpa100Model& k) {
        for (const QByteArray& s : {QByteArray("^TP0;"),QByteArray("^MDA;"),QByteArray("^AN2;"),QByteArray("^BYN;"),QByteArray("^SIT;"),QByteArray("^CR00;"),QByteArray("^SW010;"),QByteArray("^SB010;"),QByteArray("^LR00;")}) { step(k,s); }
    }
};
}
class Backend final : public IRadioBackend {
public:
    QStringList wire;
    bool connected = false;
    RadioCapabilities capabilities() const override { RadioCapabilities c; c.canTransmit=true; c.hasTuner=false; return c; }
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
int main(int argc,char** argv) {
    TestSettingsProfile profile("kxpa100_tx_controls");
    QApplication app(argc,argv);
    AppSettings::instance().load();
    RadioModel radio; auto backend=std::make_unique<Backend>(); auto* b=backend.get();
    radio.setBackendForTest(std::move(backend),"test");
    CHECK(radio.automationApplySliceFixture(0,"A"));
    b->connected = true;
    SliceDelta slice; slice.txSlice=true; slice.mode="USB"; slice.frequency=14.1; slice.panId="0x40000000"; radio.slice(0)->applyChanges(slice);
    radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
    radio.transmitModel().setHasTuner(false);
    // Host-modulated radios expose the TX slice without Flex's transmit-freq status.
    TransmitDelta delta; delta.transmitFreq=0; delta.tunePower=1; radio.transmitModel().applyChanges(delta);
    Kxpa100Model k; QList<QByteArray> wire; Kxpa100ModelTest::ready(k,wire);
    Kxpa100RadioAssociation association(&radio, &k);
    CHECK(!association.associated() && b->wire.isEmpty() && wire.isEmpty());
    TxOperationIntegrationTestAccess::identity(radio);
    CHECK(Kxpa100RadioAssociation::associate(&radio, &k));
    CHECK(association.associated());
    CHECK(!radio.settingsScope().featureExact("kxpa100").contains("useTxControls"));
    TxOperationIntegrationTestAccess::family(radio, "previous-family");
    TxOperationIntegrationTestAccess::identity(radio);
    CHECK(!association.associated());
    TxOperationIntegrationTestAccess::family(radio, "test");
    emit radio.connectionStateChanged(true);
    CHECK(association.associated());
    {
        const RadioSettingsScope scope = radio.settingsScope();
        const QJsonObject original = scope.featureExact("kxpa100");
        SettingsDatabase locker;
        CHECK(locker.open(AppSettings::instance().filePath()) && locker.begin());
        const QtMessageHandler previous = qInstallMessageHandler(capturePreferenceWarning);
        const auto restoreHandler = qScopeGuard([previous] { qInstallMessageHandler(previous); });
        CHECK(!Kxpa100RadioAssociation::dissociate(&radio, &k));
        CHECK(association.associated() && scope.featureExact("kxpa100") == original);
        CHECK(preferenceWarnings.size() == 1);
        CHECK(preferenceWarnings.first().contains("association could not be saved"));
        locker.rollback();
        locker.close();
        CHECK(Kxpa100RadioAssociation::dissociate(&radio, &k));
        CHECK(!association.associated());
        CHECK(Kxpa100RadioAssociation::associate(&radio, &k));
        CHECK(association.associated());
    }
    // Existing stored overlay preferences cannot change the native radio controls.
    {
        QJsonObject legacy = radio.settingsScope().featureExact("kxpa100");
        legacy["useTxControls"] = true;
        CHECK(radio.settingsScope().setFeature("kxpa100", 1, legacy));
        emit k.associationChanged();
        CHECK(association.associated());
        TxApplet applet;
        applet.setRadioModel(&radio);
        applet.setTransmitModel(&radio.transmitModel());
        QPushButton* atu = nullptr;
        QPushButton* mem = nullptr;
        for (auto* button : applet.findChildren<QPushButton*>()) {
            if (button->accessibleName() == "ATU tune") { atu = button; }
            if (button->accessibleName() == "ATU memories") { mem = button; }
        }
        CHECK(atu && mem && atu->text() == "ATU" && mem->text() == "MEM");
        CHECK(!atu->isEnabled() && !mem->isEnabled());
        for (auto* label : applet.findChildren<QLabel*>()) { CHECK(label->text() != "KXPA100"); }
        Kxpa100ModelTest::observe(k, "^TU;", "^TU0;");
        CHECK(association.associated() && k.amplifierMeterEligible() && !k.tunerEligible());
        CHECK(!atu->isEnabled() && atu->text() == "ATU");
        radio.transmitModel().setHasTuner(true);
        CHECK(atu->isEnabled() && atu->text() == "ATU" && mem->text() == "MEM");
    }
    const auto kxOnly = selectSharedTxMeters(5, false, false, false, true);
    CHECK(kxOnly.source == SharedTxMeterSource::Kxpa100 && kxOnly.maxWatts == 100);
    const auto pgxl = selectSharedTxMeters(100, true, true, true, true);
    CHECK(pgxl.source == SharedTxMeterSource::Amplifier && pgxl.highPowerAmplifier);
    const auto kpa = selectSharedTxMeters(100, false, true, false, true);
    CHECK(kpa.source == SharedTxMeterSource::Amplifier && kpa.maxWatts == 500);
    const auto blocked = selectSharedTxMeters(100, false, true, true, true);
    CHECK(blocked.source == SharedTxMeterSource::RadioKeyBlocked && blocked.maxWatts == 100);
    const auto stale = selectSharedTxMeters(5, false, false, false, false);
    CHECK(stale.source == SharedTxMeterSource::Radio && stale.maxWatts == 5);
    TxOperationIntegrationTestAccess::identity(radio, "ANOTHER-RADIO");
    CHECK(!association.associated());
    TxOperationIntegrationTestAccess::identity(radio);
    CHECK(association.associated());
    CHECK(b->wire.isEmpty());
    for (const QByteArray& command : wire) { CHECK(command == "^TU;"); }
    return 0;
}
