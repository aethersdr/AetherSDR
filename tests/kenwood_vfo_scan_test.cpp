// Independent PC-reference fixtures; transport is injected and never binds sockets.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QPair>
#include <QVariantMap>
#include <QtTest>
#include <functional>
#include <memory>
namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void ready(KenwoodBackend& b) { b.m_clock.start(); b.m_protocol.start("scan-fixture","fixture-only",false,0); b.m_protocol.receive("##CN1;",1); b.m_protocol.receive("##ID1;",2); b.ready(); }
    static void observe(KenwoodBackend& b,const QByteArray& body) { b.adopt(body); }
    static void poll(KenwoodBackend& b,qint64 now) { b.pollVfoScan(now); }
    static quint64 sequence(const KenwoodBackend& b) { return b.m_controlIngressSequence; }
    static void resumed(KenwoodBackend& b,const QByteArray& body,quint64 batch) { b.adopt(body,batch); }
    static void batch(KenwoodBackend& b,const QList<QByteArray>& bodies) { const quint64 batch=++b.m_controlIngressSequence; for (const QByteArray& body:bodies) { b.adopt(body,batch); } }
};
}
namespace {
using namespace AetherSDR;
using Backend=kenwood::KenwoodBackend;
struct Fixture {
    QList<QByteArray> wires;
    QVariantMap state;
    QMap<quint64,QVariant> results;
    QMap<quint64,QString> errors;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(const QByteArray&)> onWire;
    std::unique_ptr<Backend> backend;
    bool answer=false,accept=true;
    char mode='3';
    int source=0,tf=0,memory=0,quick=0,enabled=0,slow=0,speed=5,tone=0,type=1;
    qint64 now=0;
    quint64 next=1;
    Fixture() {
        backend=std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& bytes) {
            wires.append(bytes); const bool accepted=accept; const auto callback=onWire; if (callback) { callback(bytes); }
            if (backend && accepted && answer) { const QByteArray body=reply(bytes); if (!body.isEmpty()) { observe(body); } } return accepted;
        },{}});
        QObject::connect(backend.get(),&IRadioBackend::extensionStatus,backend.get(),[this](const QString&,const QString& kind,const QVariantMap& value) { if (kind=="vfoScan") { state=value; const auto callback=onStatus; if (callback) { callback(value); } } });
        QObject::connect(backend.get(),&IRadioBackend::extensionResult,backend.get(),[this](quint64 id,const QVariant& value) { results[id]=value; if (value.toMap().contains("rows")) { state=value.toMap(); } });
        QObject::connect(backend.get(),&IRadioBackend::extensionError,backend.get(),[this](quint64 id,const QString& value) { errors[id]=value; });
    }
    QByteArray reply(const QByteArray& wire) const {
        if (wire=="MF;") { return "MF0"; }
        if (wire=="FR;") { return "FR"+QByteArray::number(source); }
        if (wire=="MV;") { return "MV"+QByteArray::number(memory); }
        if (wire=="QR;") { return "QR"+QByteArray::number(quick)+"0"; }
        if (wire=="OM0;") { return QByteArray("OM0")+mode; }
        if (wire=="TS;") { return "TS"+QByteArray::number(tf); }
        if (wire=="SF0;") { return QByteArray("SF000014074000")+mode; }
        if (wire=="SF1;") { return QByteArray("SF100007074000")+mode; }
        if (wire=="SC0;") { return "SC0"+QByteArray::number(enabled)+QByteArray::number(slow); }
        if (wire=="SC1;") { return "SC1"+QByteArray::number(speed); }
        if (wire=="SC2;") { return "SC2"+QByteArray::number(tone); }
        if (wire=="SC3;") { return "SC3"+QByteArray::number(type); }
        if (wire=="TO;") { return "TO2"; }
        if (wire=="TN;") { return "TN08"; }
        if (wire=="CN;") { return "CN08"; }
        return {};
    }
    void observe(const QByteArray& body) { kenwood::KenwoodBackendTestAccess::observe(*backend,body); }
    quint64 call(const QString& verb,const QVariant& args={}) { const quint64 id=next++; backend->invokeExtension("kenwood",verb,id,args); return id; }
    void get() { call("scan.vfo.get"); }
    void ready() { kenwood::KenwoodBackendTestAccess::ready(*backend); observe("ID024"); observe("RX"); observe("FR0"); observe(QByteArray("SF000014074000")+mode); observe("TS0"); observe("MV0"); observe("QR00"); get(); wires.clear(); }
    QVariantMap row(const QString& id) const { for (const QVariant& v:state.value("rows").toList()) { if (v.toMap().value("id")==id) { return v.toMap(); } } return {}; }
    QVariantMap progress() const { return state.value("refresh").toMap(); }
    QVariantMap last() const { return state.value("lastWrite").toMap(); }
    QVariantMap arguments() const { return {{"context",state.value("context")}}; }
    QVariantMap setArgs(const QString& id,int value) const { QVariantMap args=arguments(); args["id"]=id; args["revision"]=row(id).value("revision"); args["value"]=value; return args; }
    quint64 set(const QString& id,int value) { return call("scan.vfo.set",setArgs(id,value)); }
    void tick(int count=1,int step=100) { for (int i=0;i<count && backend;++i) { now+=step; kenwood::KenwoodBackendTestAccess::poll(*backend,now); } }
    void read() { answer=true; call("scan.vfo.read",arguments()); tick(30); answer=false; }
    void cancel() { QVariantMap args=arguments();args["token"]=progress().value("token");call("scan.vfo.cancel",args); }
};
}
class KenwoodVfoScanTest final:public QObject {
    Q_OBJECT
private slots:
    void cachedReadHasNoWireOrInventedValue() {
        Fixture f;f.get();QVERIFY(f.wires.isEmpty());QCOMPARE(f.state.value("rows").toList().size(),4);
        for (const QVariant& v:f.state.value("rows").toList()) { QVERIFY(!v.toMap().value("known").toBool());QVERIFY(!v.toMap().contains("value")); }
        const QVariantMap caps=f.backend->capabilities().extensions.value("kenwood").toMap();QCOMPARE(caps.value("nativeVfoScanVersion"),QVariant(1));QVERIFY(!caps.contains("nativeMemoryReadVersion"));
    }
    void explicitReadIsThirteenOwnedQueriesAndNoSetter() {
        Fixture f;f.ready();f.read();QCOMPARE(f.progress().value("total"),QVariant(13));QCOMPARE(f.progress().value("observed"),QVariant(13));QVERIFY(!f.progress().value("busy").toBool());
        for (const QByteArray& q:{QByteArray("MF;"),QByteArray("FR;"),QByteArray("MV;"),QByteArray("QR;"),QByteArray("OM0;"),QByteArray("TS;"),QByteArray("SC0;"),QByteArray("SC1;"),QByteArray("SC2;"),QByteArray("SC3;"),QByteArray("TO;"),QByteArray("TN;"),QByteArray("CN;")}) { QVERIFY2(f.wires.contains(q),q.constData()); }
        QVERIFY(!f.wires.contains("SC01;"));QVERIFY(!f.wires.contains("SC00;"));QCOMPARE(f.row("speed").value("value"),QVariant(5));
    }
    void sc0PreservesBothIndependentReplyBits() {
        Fixture f;f.ready();f.read();for (int active=0;active<2;++active) { for (int slow=0;slow<2;++slow) { f.observe("SC0"+QByteArray::number(active)+QByteArray::number(slow));QCOMPARE(f.row("enabled").value("value"),QVariant(active));QCOMPARE(f.state.value("source").toMap().value("slowRange"),QVariant(slow!=0)); } }
        const QVariant old=f.row("enabled").value("value");for (const QByteArray& bad:{QByteArray("SC01"),QByteArray("SC0100"),QByteArray("SC021"),QByteArray("SC01x")}) { f.observe(bad);QCOMPARE(f.row("enabled").value("value"),old); }
    }
    void allNineSpeedValuesRequireNativeIntegerAndReadback() {
        Fixture f;f.ready();f.read();for (int speed=1;speed<=9;++speed) { f.wires.clear();const quint64 id=f.set("speed",speed);QVERIFY2(f.results.contains(id),qPrintable(f.errors.value(id)));QVERIFY(f.wires.contains("SC1"+QByteArray::number(speed)+";"));QCOMPARE(f.results[id].toMap().value("verified"),QVariant(false));f.speed=speed;f.answer=true;f.tick(4);f.answer=false;QCOMPARE(f.row("speed").value("value"),QVariant(speed)); }
        for (const QVariant& value:{QVariant(0),QVariant(10),QVariant(true),QVariant(2.0),QVariant(QString("2")),QVariant::fromValue(qlonglong(2))}) { QVariantMap args=f.setArgs("speed",2);args["value"]=value;f.wires.clear();QVERIFY(f.errors.contains(f.call("scan.vfo.set",args)));QVERIFY(f.wires.isEmpty()); }
    }
    void programChoiceCannotBeSetButVfoCanBeSelectedFromProgram() {
        Fixture f;f.ready();f.type=0;f.read();f.wires.clear();QVERIFY(f.errors.contains(f.set("type",0)));QVERIFY(f.wires.isEmpty());QVERIFY(f.results.contains(f.set("type",1)));QVERIFY(f.wires.contains("SC31;"));QVERIFY(!f.wires.contains("SC30;"));
    }
    void startRequiresObservedVfoTypeAndCancelNeverStopsRadio() {
        Fixture f;f.ready();f.type=0;f.read();f.wires.clear();QVERIFY(f.errors.contains(f.set("enabled",1)));QVERIFY(f.wires.isEmpty());f.observe("SC31");const QVariant old=f.row("enabled").value("value");QVERIFY(f.results.contains(f.set("enabled",1)));QCOMPARE(f.row("enabled").value("value"),old);QVERIFY(f.wires.contains("SC01;"));f.cancel();f.tick(30);QVERIFY(!f.wires.contains("SC00;"));QVERIFY(!f.wires.contains("TO0;"));
    }
    void scanFrequencyProgressionDoesNotInvalidateSetterAuthority() {
        Fixture f;f.ready();f.read();const QVariant context=f.state.value("context");const QVariant revision=f.row("speed").value("revision");for (int i=0;i<5;++i) { f.observe("FA"+QByteArray::number(14074000+i*100).rightJustified(11,'0')); }
        QCOMPARE(f.state.value("context"),context);QCOMPARE(f.row("speed").value("revision"),revision);QVERIFY(f.results.contains(f.set("speed",6)));
    }
    void toneCompanionQueryDomains()
    {
        Fixture f;
        f.mode = '4';
        f.ready();
        f.read();
        const QVariant scanBefore = f.row("tone").value("value");
        const QVariant typeBefore = f.state.value("toneObservations").toMap().value("type").toMap().value("value");
        f.wires.clear();
        for (const QPair<QByteArray, QString>& item : {
                 QPair<QByteArray, QString>{"TN", "index"}, {"CN", "ctcssIndex"}}) {
            const int maximum = item.first == "TN" ? 50 : 49;
            for (int index = 0; index <= maximum; ++index) {
                f.observe(item.first + QByteArray::number(index).rightJustified(2, '0'));
                f.get();
                const QVariantMap observed = f.state.value("toneObservations").toMap().value(item.second).toMap();
                QVERIFY(observed.value("known").toBool());
                QCOMPARE(observed.value("value").toInt(), index);
            }
            for (const QByteArray& invalid : {QByteArray::number(maximum + 1), QByteArray("99"),
                     QByteArray("-1"), QByteArray("0"), QByteArray("000"), QByteArray("A0")}) {
                f.observe(item.first + invalid);
                f.get();
                const QVariantMap observed = f.state.value("toneObservations").toMap().value(item.second).toMap();
                QVERIFY(observed.value("known").toBool());
                QCOMPARE(observed.value("value").toInt(), maximum);
            }
        }
        QVERIFY(f.wires.isEmpty());
        QCOMPARE(f.row("tone").value("value"), scanBefore);
        QCOMPARE(f.state.value("toneObservations").toMap().value("type").toMap().value("value"), typeBefore);
    }

    void fmToneScanHasFourReadonlyPostqueriesAndNoCompanionSetter() {
        Fixture f;f.mode='4';f.ready();f.read();f.wires.clear();QVERIFY(f.errors.contains(f.set("speed",6)));QVERIFY(f.wires.isEmpty());
        for (int tone=0;tone<=2;++tone) { f.wires.clear();QVERIFY(f.results.contains(f.set("tone",tone)));QVERIFY(f.wires.contains("SC2"+QByteArray::number(tone)+";"));QCOMPARE(f.progress().value("total"),QVariant(4));f.tone=tone;f.answer=true;f.tick(8);f.answer=false;for (const QByteArray& q:{QByteArray("SC2;"),QByteArray("TO;"),QByteArray("TN;"),QByteArray("CN;")}) { QVERIFY(f.wires.contains(q)); }for (const QByteArray& wire:f.wires) { if (wire.startsWith("TO") || wire.startsWith("TN") || wire.startsWith("CN")) { QVERIFY(wire=="TO;" || wire=="TN;" || wire=="CN;"); } } }
    }
    void typeSelectionCanUseKnownTfOnWithoutModeClaim() {
        Fixture f;f.ready();f.read();f.tf=1;f.observe("TS1");f.read();QVERIFY(f.results.contains(f.set("type",1)));f.cancel();QVERIFY(f.errors.contains(f.set("enabled",1)));QVERIFY(f.errors.contains(f.set("speed",4)));QVERIFY(!f.wires.contains("SC01;"));
    }
    void transientSourceRoundTripDuringPendingCannotReviveIntent() {
        Fixture f;f.ready();f.read();f.wires.clear();bool nested=false;f.onStatus=[&](const QVariantMap& state) { for (const QVariant& row:state.value("rows").toList()) { if (!nested && row.toMap().value("pending").toBool()) { nested=true;f.observe("FR1");f.observe("FR0"); } } };f.set("speed",7);QVERIFY(nested);QVERIFY(!f.wires.contains("SC17;"));
    }
    void memoryAndQuickViewsDoNotGrantScanMutationOrMemorySupport() {
        Fixture f;f.ready();f.read();f.observe("QR11");f.get();f.wires.clear();QVERIFY(f.errors.contains(f.set("enabled",1)));QVERIFY(f.wires.isEmpty());f.observe("QR00");f.observe("MV1");f.get();QVERIFY(f.errors.contains(f.set("type",1)));QVERIFY(!f.backend->capabilities().extensions.value("kenwood").toMap().contains("nativeMemoryReadVersion"));
    }
    void receiveVfoChangeCannotReuseOtherVfoModeForSpeedAdmission() {
        Fixture f;f.ready();f.read();f.observe("FR1");f.observe("SF1000070740004");f.observe("SC15");f.get();f.wires.clear();
        const quint64 id=f.set("speed",6);QVERIFY(f.errors.contains(id));QVERIFY(!f.wires.contains("SC16;"));
    }
    void programSelectionAfterStartHandoffCannotQualifyVfoOutcome() {
        Fixture f;f.ready();f.read();f.wires.clear();QVERIFY(f.results.contains(f.set("enabled",1)));f.observe("SC30");f.enabled=1;f.answer=true;f.tick(4);f.answer=false;
        QCOMPARE(f.last().value("state"),QVariant("indeterminate"));QVERIFY(!f.wires.contains("SC00;"));
    }
    void programScanCannotAcquireExcludedStopControl() {
        Fixture f;f.ready();f.type=0;f.enabled=1;f.read();f.wires.clear();QVERIFY(f.errors.contains(f.set("enabled",0)));QVERIFY(!f.wires.contains("SC00;"));
    }
    void resumedOldRoleCannotBorrowNewerDifferentSourceMode() {
        Fixture f;f.ready();f.read();const quint64 oldBatch=kenwood::KenwoodBackendTestAccess::sequence(*f.backend);f.observe("FR1");f.observe("FR0");f.observe("SF0000140740004");f.observe("OM04");f.observe("SC20");
        kenwood::KenwoodBackendTestAccess::resumed(*f.backend,"FR1",oldBatch);f.get();f.wires.clear();QVERIFY(f.errors.contains(f.set("tone",1)));QVERIFY(!f.wires.contains("SC21;"));
    }
    void selectedVfoModeCannotBorrowOlderFmDisplayForToneScan() {
        Fixture f;f.mode='4';f.ready();f.read();f.observe("SF0000140740003");f.observe("SC20");f.get();f.wires.clear();QVERIFY(f.errors.contains(f.set("tone",1)));QVERIFY(!f.wires.contains("SC21;"));
    }
    void disconnectAndTimeoutNeverReplayOrStop() {
        Fixture f;f.ready();f.read();f.wires.clear();f.set("enabled",1);f.tick(1);f.tick(1,2500);QVERIFY(!f.progress().value("busy").toBool());QVERIFY(!f.wires.contains("SC00;"));f.backend->disconnectRadio();f.wires.clear();f.tick(30);QVERIFY(f.wires.isEmpty());
    }
};
int main(int argc,char** argv) { TestSettingsProfile profile("kenwood-vfo-scan");if (!profile.isValid()) { return 1; }QCoreApplication app(argc,argv);KenwoodVfoScanTest test;return QTest::qExec(&test,argc,argv); }
#include "kenwood_vfo_scan_test.moc"
