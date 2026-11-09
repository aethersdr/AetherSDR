// Independent literal EX fixtures: PC reference printed24/26.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QVariantMap>
#include <QtTest>
#include <functional>
#include <memory>
namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void ready(KenwoodBackend& b) { b.m_clock.start();b.m_protocol.start("prefs-fixture","fixture-only",false,0);b.m_protocol.receive("##CN1;",1);b.m_protocol.receive("##ID1;",2);b.ready(); }
    static void observe(KenwoodBackend& b,const QByteArray& body) { b.adopt(body); }
    static void poll(KenwoodBackend& b,qint64 now) { b.pollScanPreferences(now); }
    static quint64 sequence(const KenwoodBackend& b) { return b.m_controlIngressSequence; }
    static void resumed(KenwoodBackend& b,const QByteArray& body,quint64 batch) { b.adopt(body,batch); }
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
    int bank=0,hold=0,resume=0;
    bool answer=false,accept=true;
    quint64 next=1;
    qint64 now=0;
    Fixture() {
        backend=std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& wire) { wires.append(wire);const bool accepted=accept;const auto callback=onWire;if (callback) { callback(wire); }if (backend && accepted && answer) { const QByteArray body=reply(wire);if (!body.isEmpty()) { observe(body); } }return accepted; },{}});
        QObject::connect(backend.get(),&IRadioBackend::extensionStatus,backend.get(),[this](const QString&,const QString& kind,const QVariantMap& value) { if (kind=="scanPreferences") { state=value;const auto callback=onStatus;if (callback) { callback(value); } } });
        QObject::connect(backend.get(),&IRadioBackend::extensionResult,backend.get(),[this](quint64 id,const QVariant& value) { results[id]=value;if (value.toMap().contains("rows")) { state=value.toMap(); } });
        QObject::connect(backend.get(),&IRadioBackend::extensionError,backend.get(),[this](quint64 id,const QString& value) { errors[id]=value; });
    }
    QByteArray reply(const QByteArray& wire) const { if (wire=="MF;") { return "MF"+QByteArray::number(bank); }if (wire=="EX00404;") { return "EX00404 "+QByteArray::number(hold).rightJustified(3,'0'); }if (wire=="EX00405;") { return "EX00405 "+QByteArray::number(resume).rightJustified(3,'0'); }return {}; }
    void observe(const QByteArray& body) { kenwood::KenwoodBackendTestAccess::observe(*backend,body); }
    quint64 call(const QString& verb,const QVariant& args={}) { const quint64 id=next++;backend->invokeExtension("kenwood",verb,id,args);return id; }
    void get() { call("scan.preferences.get"); }
    void ready() { kenwood::KenwoodBackendTestAccess::ready(*backend);observe("ID024");observe("RX");get();wires.clear(); }
    QVariantMap row(const QString& id) const { for (const QVariant& v:state.value("rows").toList()) { if (v.toMap().value("id")==id) { return v.toMap(); } }return {}; }
    QVariantMap progress() const { return state.value("refresh").toMap(); }
    QVariantMap last() const { return state.value("lastWrite").toMap(); }
    QVariantMap args() const { return {{"context",state.value("context")}}; }
    QVariantMap writeArgs(const QString& id) const { QVariantMap a=args();a["id"]=id;a["revision"]=row(id).value("revision");return a; }
    quint64 set(const QString& id,int value) { QVariantMap a=writeArgs(id);a["value"]=value;return call("scan.preferences.set",a); }
    quint64 reset(const QString& id) { return call("scan.preferences.reset",writeArgs(id)); }
    void tick(int count=1,int step=100) { for (int i=0;i<count && backend;++i) { now+=step;kenwood::KenwoodBackendTestAccess::poll(*backend,now); } }
    void read() { answer=true;call("scan.preferences.read",args());tick(8);answer=false; }
    void cancel() { QVariantMap a=args();a["token"]=progress().value("token");call("scan.preferences.cancel",a); }
};
}
class KenwoodScanPreferencesTest final:public QObject {
    Q_OBJECT
private slots:
    void cachedGetterHasNoWireOrDefault() {
        Fixture f;f.get();QVERIFY(f.wires.isEmpty());QCOMPARE(f.state.value("rows").toList().size(),2);QVERIFY(!f.state.value("bank").toMap().value("known").toBool());
        for (const QVariant& v:f.state.value("rows").toList()) { QVERIFY(!v.toMap().value("known").toBool());QVERIFY(!v.toMap().contains("value"));QVERIFY(!v.toMap().value("resettable").toBool()); }
    }
    void explicitReadAcquiresInitialBankAndBothValuesInThreeJobs() {
        Fixture f;f.ready();f.read();QCOMPARE(f.wires,QList<QByteArray>({"MF;","EX00404;","EX00405;"}));QCOMPARE(f.progress().value("total"),QVariant(3));QCOMPARE(f.progress().value("observed"),QVariant(3));QVERIFY(f.state.value("bank").toMap().value("known").toBool());QVERIFY(f.row("hold").value("known").toBool());QVERIFY(f.row("resume").value("known").toBool());
    }
    void bothRowsBothChoicesHaveExactNativeWireAndActualReadback() {
        Fixture f;f.ready();f.read();for (const QString& id:{QString("hold"),QString("resume")}) { for (int value=0;value<2;++value) { f.wires.clear();const quint64 request=f.set(id,value);QVERIFY2(f.results.contains(request),qPrintable(f.errors.value(request)));QCOMPARE(f.results[request].toMap().size(),3);QCOMPARE(f.results[request].toMap().value("verified"),QVariant(false));const QByteArray key=id=="hold" ? "EX00404" : "EX00405";QCOMPARE(f.wires,QList<QByteArray>({key+" "+QByteArray::number(value).rightJustified(3,'0')+";"}));if (id=="hold") { f.hold=value; }else { f.resume=value; }f.answer=true;f.tick(5);f.answer=false;QCOMPARE(f.wires,QList<QByteArray>({key+" "+QByteArray::number(value).rightJustified(3,'0')+";","MF;",key+";"}));QCOMPARE(f.row(id).value("value"),QVariant(value));QCOMPARE(f.last().value("state"),QVariant("observed_consistent")); } }
    }
    void resetIsNativeClassificationNineAndNeverPredictsInitialValue() {
        Fixture f;f.ready();f.read();for (const QString& id:{QString("hold"),QString("resume")}) { f.wires.clear();const QVariant old=f.row(id).value("value");QVERIFY(f.results.contains(f.reset(id)));const QByteArray key=id=="hold" ? "EX00404" : "EX00405";QCOMPARE(f.wires,QList<QByteArray>({key+"9;"}));QCOMPARE(f.row(id).value("value"),old);f.hold=f.resume=1;f.answer=true;f.tick(5);f.answer=false;QCOMPARE(f.row(id).value("value"),QVariant(1));QCOMPARE(f.last().value("operation"),QVariant("reset"));QCOMPARE(f.last().value("state"),QVariant("sent_unverified")); }
    }
    void globalPreferencesIgnoreSourceModeTfAndFrequencyChanges() {
        Fixture f;f.ready();f.read();const QVariant context=f.state.value("context"),revision=f.row("hold").value("revision");for (const QByteArray& body:{QByteArray("FR1"),QByteArray("SF1000070740004"),QByteArray("TS1"),QByteArray("MV1"),QByteArray("QR11"),QByteArray("FA00014075000")}) { f.observe(body); }f.get();QCOMPARE(f.state.value("context"),context);QCOMPARE(f.row("hold").value("revision"),revision);QVERIFY(f.results.contains(f.set("hold",1)));
    }
    void sameBankPollPreservesDraftAuthority() {
        Fixture f;f.ready();f.read();const QVariant context=f.state.value("context"),revision=f.row("resume").value("revision");f.observe("MF0");f.get();QCOMPARE(f.state.value("context"),context);QCOMPARE(f.row("resume").value("revision"),revision);QVERIFY(f.results.contains(f.set("resume",1)));
    }
    void bankChangeInvalidatesBothRowsAndRejectsOldDraft() {
        Fixture f;f.ready();f.read();QVariantMap old=f.writeArgs("hold");old["value"]=1;f.observe("MF1");f.get();QVERIFY(!f.row("hold").value("known").toBool());QVERIFY(!f.row("resume").value("known").toBool());f.wires.clear();QVERIFY(f.errors.contains(f.call("scan.preferences.set",old)));QVERIFY(f.wires.isEmpty());
    }
    void transientBankRoundTripBeforeHandoffNeverRevivesProducer() {
        Fixture f;f.ready();f.read();f.wires.clear();bool nested=false;f.onStatus=[&](const QVariantMap& state) { for (const QVariant& v:state.value("rows").toList()) { if (!nested && v.toMap().value("pending").toBool()) { nested=true;f.observe("MF1");f.observe("MF0"); } } };f.set("hold",1);QVERIFY(nested);QVERIFY(!f.wires.contains("EX00404 001;"));
    }
    void mfTimeoutCannotBeQualifiedBySubsequentMenuReplies() {
        Fixture f;f.ready();f.read();f.call("scan.preferences.read",f.args());f.tick();f.tick(1,2500);f.answer=true;f.tick(8);f.answer=false;QVERIFY(!f.state.value("bank").toMap().value("known").toBool());QVERIFY(!f.row("hold").value("known").toBool());QVERIFY(!f.row("resume").value("known").toBool());f.wires.clear();QVERIFY(f.errors.contains(f.set("hold",1)));QVERIFY(f.wires.isEmpty());
    }
    void failedPostwriteMfCannotBecomeConsistentFromMatchingValue() {
        Fixture f;f.ready();f.read();f.set("hold",1);f.tick();f.tick(1,2500);f.hold=1;f.answer=true;f.tick(6);f.answer=false;QVERIFY(f.last().value("state")!=QVariant("observed_consistent"));QVERIFY(!f.state.value("bank").toMap().value("known").toBool());
    }
    void pendingRxRoundTripCannotSendOldReset() {
        Fixture f;f.ready();f.read();f.wires.clear();bool nested=false;f.onStatus=[&](const QVariantMap& state) { for (const QVariant& v:state.value("rows").toList()) { if (!nested && v.toMap().value("pending").toBool()) { nested=true;f.observe("TX0");f.observe("RX"); } } };f.reset("resume");QVERIFY(nested);QVERIFY(!f.wires.contains("EX004059;"));
    }
    void strictTypedWritesAndResetArgumentsRejectWithoutWire() {
        Fixture f;f.ready();f.read();for (const QVariant& v:{QVariant(-1),QVariant(2),QVariant(true),QVariant(1.0),QVariant("1"),QVariant::fromValue(qlonglong(1))}) { QVariantMap a=f.writeArgs("hold");a["value"]=v;f.wires.clear();QVERIFY(f.errors.contains(f.call("scan.preferences.set",a)));QVERIFY(f.wires.isEmpty()); }QVariantMap reset=f.writeArgs("hold");reset["value"]=0;QVERIFY(f.errors.contains(f.call("scan.preferences.reset",reset)));QVERIFY(f.wires.isEmpty());
    }
    void malformedRepliesNeverInventOrReplaceLastValue() {
        Fixture f;f.ready();f.read();const QVariant old=f.row("hold").value("value");for (const QByteArray& bad:{QByteArray("EX004040001"),QByteArray("EX00404 01"),QByteArray("EX00404 002"),QByteArray("EX00404 999"),QByteArray("EX004049"),QByteArray("EX00404 001x")}) { f.observe(bad);f.get();QCOMPARE(f.row("hold").value("value"),old); }
    }
    void resumedOldBankAndMenuCannotOverwriteNewBankHistory() {
        Fixture f;f.ready();f.read();const quint64 old=kenwood::KenwoodBackendTestAccess::sequence(*f.backend);f.bank=1;f.observe("MF1");f.hold=1;f.read();const QVariant current=f.row("hold").value("value");kenwood::KenwoodBackendTestAccess::resumed(*f.backend,"MF0",old);kenwood::KenwoodBackendTestAccess::resumed(*f.backend,"EX00404 000",old);f.get();QCOMPARE(f.state.value("bank").toMap().value("value"),QVariant(1));QCOMPARE(f.row("hold").value("value"),current);
    }
    void reconnectReadCanAcquireDifferentBankWithoutCancelingItsOwnSurvey() {
        Fixture f;f.ready();f.read();f.backend->disconnectRadio();f.bank=1;f.ready();f.read();
        QCOMPARE(f.progress().value("total"),QVariant(3));QCOMPARE(f.progress().value("observed"),QVariant(3));QVERIFY(f.state.value("bank").toMap().value("known").toBool());QCOMPARE(f.state.value("bank").toMap().value("value"),QVariant(1));QVERIFY(f.row("hold").value("known").toBool());QVERIFY(f.row("resume").value("known").toBool());
    }
    void freshReadAfterFailedBankCanAcquireDifferentBank() {
        Fixture f;f.ready();f.read();f.call("scan.preferences.read",f.args());f.tick();f.tick(1,2500);f.answer=true;f.tick(8);f.answer=false;f.bank=1;f.read();
        QCOMPARE(f.progress().value("observed"),QVariant(3));QCOMPARE(f.state.value("bank").toMap().value("value"),QVariant(1));QVERIFY(f.row("hold").value("known").toBool());
    }
    void predispatchBankObservationCannotLeaveOldBankWriteAuthority() {
        Fixture f;f.ready();f.read();f.wires.clear();bool nested=false;quint64 write=0;
        f.onStatus=[&](const QVariantMap& state) { if (!nested && state.value("refresh").toMap().value("pendingKey")==QVariant("MF")) { nested=true;f.observe("MF1");f.cancel();write=f.set("hold",1); } };
        f.call("scan.preferences.read",f.args());f.tick();QVERIFY(nested);QVERIFY(f.errors.contains(write));QVERIFY(!f.wires.contains("EX00404 001;"));
    }
    void predispatchMenuObservationCannotPreserveOldRowBaseline() {
        Fixture f;f.ready();f.read();QVariantMap old=f.writeArgs("hold");old["value"]=0;bool nested=false;quint64 write=0;
        f.onStatus=[&](const QVariantMap& state) { if (!nested && state.value("refresh").toMap().value("pendingKey")==QVariant("EX00404")) { nested=true;f.observe("EX00404 001");f.cancel();write=f.call("scan.preferences.set",old); } };
        f.answer=true;f.call("scan.preferences.read",f.args());f.tick(2);f.answer=false;QVERIFY(nested);QVERIFY(f.errors.contains(write));QVERIFY(!f.wires.contains("EX00404 000;"));
    }
    void sameIngressPredispatchFactCannotBecomeAQueryReply() {
        Fixture f;f.ready();f.read();f.wires.clear();bool nested=false;
        f.onStatus=[&](const QVariantMap& state) { if (!nested && state.value("refresh").toMap().value("pendingKey")==QVariant("MF")) { nested=true;kenwood::KenwoodBackendTestAccess::resumed(*f.backend,"MF1",kenwood::KenwoodBackendTestAccess::sequence(*f.backend));QCOMPARE(f.progress().value("observed"),QVariant(0)); } };
        f.answer=true;f.call("scan.preferences.read",f.args());f.tick(6);QVERIFY(nested);QCOMPARE(f.state.value("bank").toMap().value("value"),QVariant(0));QCOMPARE(f.progress().value("observed"),QVariant(3));QCOMPARE(f.wires,QList<QByteArray>({"MF;","EX00404;","EX00405;"}));
    }
    void freshUnchangedPredispatchFactDoesNotAcknowledgeUnsentQuery() {
        Fixture f;f.ready();f.read();f.wires.clear();bool nested=false;
        f.onStatus=[&](const QVariantMap& state) { if (!nested && state.value("refresh").toMap().value("pendingKey")==QVariant("MF")) { nested=true;f.observe("MF0");QCOMPARE(f.progress().value("observed"),QVariant(0));QVERIFY(f.wires.isEmpty()); } };
        f.answer=true;f.call("scan.preferences.read",f.args());f.tick(6);QVERIFY(nested);QCOMPARE(f.progress().value("observed"),QVariant(3));QCOMPARE(f.wires,QList<QByteArray>({"MF;","EX00404;","EX00405;"}));
    }
    void deletionDuringPendingReadPublicationCannotSendQuery() {
        Fixture f;f.ready();f.read();f.wires.clear();const QPointer<Backend> alive(f.backend.get());
        f.onStatus=[&](const QVariantMap& state) { if (!state.value("refresh").toMap().value("pendingKey").toString().isEmpty()) { f.backend.reset(); } };
        f.call("scan.preferences.read",f.args());f.tick();QVERIFY(!alive);QVERIFY(f.wires.isEmpty());
    }
    void cancelAndDisconnectNeverResetSwitchBankOrRestorePreferences() {
        Fixture f;f.ready();f.read();f.wires.clear();f.set("hold",1);f.cancel();f.tick(10);QCOMPARE(f.wires,QList<QByteArray>({"EX00404 001;"}));f.backend->disconnectRadio();f.wires.clear();f.tick(10);QVERIFY(f.wires.isEmpty());
    }
};
int main(int argc,char** argv) { TestSettingsProfile profile("kenwood-scan-preferences");if (!profile.isValid()) { return 1; }QCoreApplication app(argc,argv);KenwoodScanPreferencesTest test;return QTest::qExec(&test,argc,argv); }
#include "kenwood_scan_preferences_test.moc"
