// Independent literal EX fixtures: PC reference printed24/26; Instruction Manual EN5-24.
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
    static void poll(KenwoodBackend& b,qint64 now) { b.pollPskReceive(now); }
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
    int bank=0,range=0;
    bool answer=false,accept=true;
    quint64 next=1;
    qint64 now=0;
    Fixture() {
        backend=std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& wire) { wires.append(wire);const bool accepted=accept;const auto callback=onWire;if (callback) { callback(wire); }if (backend && accepted && answer) { const QByteArray body=reply(wire);if (!body.isEmpty()) { observe(body); } }return accepted; },{}});
        QObject::connect(backend.get(),&IRadioBackend::extensionStatus,backend.get(),[this](const QString&,const QString& kind,const QVariantMap& value) { if (kind=="pskReceive") { state=value;const auto callback=onStatus;if (callback) { callback(value); } } });
        QObject::connect(backend.get(),&IRadioBackend::extensionResult,backend.get(),[this](quint64 id,const QVariant& value) { results[id]=value;if (value.toMap().contains("rows")) { state=value.toMap(); } });
        QObject::connect(backend.get(),&IRadioBackend::extensionError,backend.get(),[this](quint64 id,const QString& value) { errors[id]=value; });
    }
    QByteArray reply(const QByteArray& wire) const {
        if (wire=="MF;") { return "MF"+QByteArray::number(bank); }
        if (wire=="EX00211;") { return "EX00211 "+QByteArray::number(range).rightJustified(3,'0'); }
        return {};
    }
    void observe(const QByteArray& body) { kenwood::KenwoodBackendTestAccess::observe(*backend,body); }
    quint64 call(const QString& verb,const QVariant& args={}) { const quint64 id=next++;backend->invokeExtension("kenwood",verb,id,args);return id; }
    void get() { call("psk.receive.get"); }
    void ready() { kenwood::KenwoodBackendTestAccess::ready(*backend);observe("ID024");observe("RX");get();wires.clear(); }
    QVariantMap row(const QString& id) const { for (const QVariant& v:state.value("rows").toList()) { if (v.toMap().value("id")==id) { return v.toMap(); } }return {}; }
    QVariantMap progress() const { return state.value("refresh").toMap(); }
    QVariantMap last() const { return state.value("lastWrite").toMap(); }
    QVariantMap args() const { return {{"context",state.value("context")}}; }
    QVariantMap writeArgs(const QString& id) const { QVariantMap a=args();a["id"]=id;a["revision"]=row(id).value("revision");return a; }
    quint64 set(const QString& id,int value) { QVariantMap a=writeArgs(id);a["value"]=value;return call("psk.receive.set",a); }
    quint64 reset(const QString& id) { return call("psk.receive.reset",writeArgs(id)); }
    void tick(int count=1,int step=100) { for (int i=0;i<count && backend;++i) { now+=step;kenwood::KenwoodBackendTestAccess::poll(*backend,now); } }
    void read() { answer=true;call("psk.receive.read",args());tick(8);answer=false; }
    void cancel() { QVariantMap a=args();a["token"]=progress().value("token");call("psk.receive.cancel",a); }
};
}
class KenwoodPskReceiveTest final:public QObject {
    Q_OBJECT
private slots:
    void cachedGetAndTwoJobReadDoNotSelectModeOrBank() {
        Fixture f;f.get();QVERIFY(f.wires.isEmpty());QCOMPARE(f.state.value("rows").toList().size(),1);
        kenwood::KenwoodBackendTestAccess::ready(*f.backend);f.observe("ID024");f.get();f.wires.clear();
        f.read();QCOMPARE(f.wires,(QList<QByteArray>{"MF;","EX00211;"}));
        QVERIFY(f.row("afcRange").value("known").toBool());QVERIFY(!f.row("afcRange").value("editable").toBool());
        QCOMPARE(f.progress().value("observed").toInt(),2);
    }
    void bothLiteralChoicesAndStrictArguments() {
        Fixture f;f.ready();f.read();
        for (int value:{1,0}) {
            f.wires.clear();const int old=f.row("afcRange").value("value").toInt();
            const quint64 request=f.set("afcRange",value);QVERIFY(!f.errors.contains(request));
            QCOMPARE(f.row("afcRange").value("value").toInt(),old);
            QVERIFY(!f.results.value(request).toMap().value("verified").toBool());
            f.range=value;f.answer=true;f.tick(5);f.answer=false;
            QCOMPARE(f.wires,(QList<QByteArray>{QByteArray("EX00211 ")+QByteArray::number(value).rightJustified(3,'0')+";","MF;","EX00211;"}));
            QCOMPARE(f.row("afcRange").value("value").toInt(),value);QCOMPARE(f.last().value("state").toString(),QString("observed_consistent"));
        }
        f.wires.clear();
        for (const QVariant& bad:{QVariant(true),QVariant("1"),QVariant(1.0),QVariant(-1),QVariant(2)}) {
            QVariantMap args=f.writeArgs("afcRange");args["value"]=bad;
            QVERIFY(f.errors.contains(f.call("psk.receive.set",args)));
        }
        QVERIFY(f.wires.isEmpty());
    }
    void resetNeverPredictsTheInitialValue() {
        Fixture f;f.ready();f.read();
        for (int observed:{1,0}) {
            f.wires.clear();const int before=f.row("afcRange").value("value").toInt();
            const quint64 request=f.reset("afcRange");QVERIFY(!f.errors.contains(request));
            QCOMPARE(f.row("afcRange").value("value").toInt(),before);
            f.range=observed;f.answer=true;f.tick(5);f.answer=false;
            QCOMPARE(f.wires,(QList<QByteArray>{"EX002119;","MF;","EX00211;"}));
            QCOMPARE(f.row("afcRange").value("value").toInt(),observed);
            QCOMPARE(f.last().value("state").toString(),QString("sent_unverified"));
        }
    }
    void malformedLiteralsDoNotBecomeAnEnumOrResetObservation() {
        Fixture f;f.ready();f.read();
        for (const QByteArray& body:{QByteArray("EX00211000"),QByteArray("EX00211 00"),QByteArray("EX00211 0000"),QByteArray("EX002119"),QByteArray("EX00211 009"),QByteArray("EX00211 001x")}) { f.observe(body); }
        QCOMPARE(f.row("afcRange").value("value").toInt(),0);
    }
    void globalPreferenceDraftDoesNotDependOnModeVfoOrTf() {
        Fixture f;f.ready();f.read();const QString context=f.state.value("context").toString();
        for (const QByteArray& body:{QByteArray("FR1"),QByteArray("OM04"),QByteArray("FA00014074000"),QByteArray("TS1")}) { f.observe(body); }
        QCOMPARE(f.state.value("context").toString(),context);
        QVERIFY(!f.errors.contains(f.set("afcRange",1)));
    }
    void knownBankChangeRetiresOldAuthorityAndWriteDraft() {
        Fixture f;f.ready();f.read();QVariantMap old=f.writeArgs("afcRange");old["value"]=1;
        f.observe("MF1");QVERIFY(!f.row("afcRange").value("known").toBool());
        f.wires.clear();QVERIFY(f.errors.contains(f.call("psk.receive.set",old)));QVERIFY(f.wires.isEmpty());
        f.bank=1;f.range=1;f.read();QVERIFY(f.row("afcRange").value("known").toBool());
        QCOMPARE(f.progress().value("observed").toInt(),2);
    }
    void reconnectCanAcquireDifferentBankInItsOwnSurvey() {
        Fixture f;f.ready();f.read();f.backend->disconnectRadio();
        kenwood::KenwoodBackendTestAccess::ready(*f.backend);f.observe("ID024");f.observe("RX");f.get();
        f.bank=1;f.range=1;f.wires.clear();f.read();
        QCOMPARE(f.progress().value("observed").toInt(),2);QCOMPARE(f.wires,(QList<QByteArray>{"MF;","EX00211;"}));
        QCOMPARE(f.state.value("bank").toMap().value("value").toInt(),1);
        QVERIFY(f.row("afcRange").value("known").toBool());
    }
    void copiedSameIngressFactCannotAcknowledgeOrReplaceUnsentBankRead() {
        Fixture f;f.ready();f.read();bool nested=false;
        f.onStatus=[&](const QVariantMap& state) {
            if (!nested && state.value("refresh").toMap().value("pendingKey")=="MF") {
                nested=true;const quint64 old=kenwood::KenwoodBackendTestAccess::sequence(*f.backend);
                kenwood::KenwoodBackendTestAccess::resumed(*f.backend,"MF1",old);
            }
        };
        f.wires.clear();f.call("psk.receive.read",f.args());f.tick();
        QVERIFY(nested);QCOMPARE(f.state.value("bank").toMap().value("value").toInt(),0);
        QCOMPARE(f.progress().value("observed").toInt(),0);QVERIFY(f.wires.contains("MF;"));
    }
    void failedBankSurveyCannotBorrowUnownedFactBeforeNextDispatch() {
        Fixture f;f.ready();f.read();f.call("psk.receive.read",f.args());f.tick();
        f.tick(1,2100);QVERIFY(f.progress().value("failed").toInt()>0);
        f.observe("MF0");f.observe("EX00211 001");f.cancel();
        QVERIFY(!f.row("afcRange").value("known").toBool());
        f.wires.clear();QVERIFY(f.errors.contains(f.set("afcRange",0)));QVERIFY(f.wires.isEmpty());
    }
    void newerUnchangedFactFencesAnOlderCopiedChangeBeforeWrite() {
        Fixture f;f.ready();f.read();bool nested=false;
        f.onStatus=[&](const QVariantMap& state) {
            if (!nested && state.value("lastWrite").toMap().value("state")=="pending") {
                nested=true;f.observe("MF0");const quint64 old=kenwood::KenwoodBackendTestAccess::sequence(*f.backend);
                f.observe("EX00211 000");kenwood::KenwoodBackendTestAccess::resumed(*f.backend,"EX00211 001",old);
            }
        };
        f.wires.clear();f.set("afcRange",1);QVERIFY(nested);
        QCOMPARE(f.row("afcRange").value("value").toInt(),0);QVERIFY(f.wires.contains("EX00211 001;"));
    }
    void deletionDuringPendingReadPublicationCannotSendQuery() {
        Fixture f;f.ready();QPointer<Backend> guard(f.backend.get());
        f.onStatus=[&](const QVariantMap& state) { if (state.value("refresh").toMap().value("busy").toBool()) { f.backend.reset(); } };
        f.wires.clear();f.call("psk.receive.read",f.args());QVERIFY(guard.isNull());QVERIFY(f.wires.isEmpty());
    }
};
int main(int argc,char** argv) {
    TestSettingsProfile profile("kenwood-psk-receive");if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc,argv);KenwoodPskReceiveTest test;return QTest::qExec(&test,argc,argv);
}
#include "kenwood_psk_receive_test.moc"
