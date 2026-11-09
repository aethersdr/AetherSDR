// Real Qt controls -> RadioModel -> KenwoodBackend, injected transport only.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeRadioMessagesWidget.h"
#include "models/RadioModel.h"
#include <QAccessible>
#include <QApplication>
#include <QLabel>
#include <QList>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSpinBox>
#include <QStringList>
#include <QVariantMap>
#include <QtTest>
#include <functional>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend) {
        backend.m_clock.start(); backend.m_protocol.start("messages-fixture", "fixture-only", false, 0);
        backend.m_protocol.receive("##CN1;", 1); backend.m_protocol.receive("##ID1;", 2); backend.ready();
    }
    static void observe(KenwoodBackend& backend, const QByteArray& bytes) { backend.adopt(bytes); }
    static void intent(KenwoodBackend& backend, int kind, bool engaged) {
        if (kind==0) { backend.m_keyIntent=engaged; }
        if (kind==1) { backend.m_cwIntent=engaged; }
        if (kind==2) { backend.m_atuIntent=engaged; }
        if (kind==3) { if (engaged) { backend.m_tune.emplace(); } else { backend.m_tune.reset(); } }
    }
    static void poll(KenwoodBackend& backend, bool expire = false) { backend.pollRadioMessageWrite(expire ? backend.m_radioBeepDeadlineMs : backend.m_clock.elapsed()); }
};
}
namespace {
using namespace AetherSDR;
using Backend = kenwood::KenwoodBackend;
std::function<void(QAccessibleEvent*)> accessibilityCallback;
void accessibleUpdate(QAccessibleEvent* event) { if (accessibilityCallback) { accessibilityCallback(event); } }
struct Fixture {
    QList<QByteArray> wires;
    QVariantMap state;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(quint64,const QVariant&)> onResult;
    RadioModel model;
    Backend* backend = nullptr;
    std::unique_ptr<NativeRadioMessagesWidget> widget;
    Fixture() { install(); makeWidget(); }
    void install(char mode = '2') {
        auto owned = std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& bytes) { wires.append(bytes); return true; }, {}});
        backend = owned.get(); model.setBackendForTest(std::move(owned), "kenwood");
        QObject::connect(backend,&IRadioBackend::extensionStatus,&model,[this](const QString&,const QString& kind,const QVariantMap& status) { if (kind=="radioMessages") { state=status; if (onStatus) { onStatus(status); } } });
        QObject::connect(backend,&IRadioBackend::extensionResult,&model,[this](quint64 id,const QVariant& result) { if (onResult) { onResult(id,result); } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& bytes : {QByteArray("ID024"),QByteArray("FR0"),QByteArray("FT0"),QByteArray("FA00014074000"),QByteArray("OM0")+mode,QByteArray("SF000014074000")+mode,QByteArray("TS0"),QByteArray("RX")}) { observe(bytes); }
        // Test seam omits setupBackend lifecycle wiring. Supply only the real
        // public connection notification; no copied extension/status relay.
        emit model.connectionStateChanged(true);
    }
    void makeWidget() { widget=std::make_unique<NativeRadioMessagesWidget>(model); widget->resize(850,400); widget->show(); QCoreApplication::processEvents(); }
    void observe(const QByteArray& bytes) { kenwood::KenwoodBackendTestAccess::observe(*backend,bytes); }
    void load(int value=10) { observe("ME0000"); observe("ME1000"); observe("EX00100 "+QByteArray::number(value).rightJustified(3,'0')); }
    template<class T> T child(const QString& name) { return widget->findChild<T>(name); }
    QPushButton* apply() { return child<QPushButton*>("nativeRadioBeepApply"); }
    QSpinBox* draft() { return child<QSpinBox*>("nativeRadioBeepDraft"); }
    QLabel* observed() { return child<QLabel*>("nativeRadioBeepObserved"); }
    void stage(int value) { draft()->setFocus(); draft()->selectAll(); QTest::keyClicks(draft(),QString::number(value)); draft()->interpretText(); }
    void read() { QTest::mouseClick(child<QPushButton*>("nativeRadioMessagesRead"),Qt::LeftButton); }
    void publish(const QVariantMap& status) { emit backend->extensionStatus("kenwood","radioMessages",status); }
};
}
class NativeRadioMessagesWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void unknownReadQueriesOnlyAndSourcesRemainDistinct() {
        Fixture f; QVERIFY(f.observed()->text().contains("Unknown")); QVERIFY(!f.draft()->isEnabled()); QVERIFY(!f.apply()->isEnabled());
        QVERIFY(!f.apply()->accessibleDescription().isEmpty()); f.wires.clear(); f.read();
        QCOMPARE(f.wires,QList<QByteArray>({"ME0;","ME1;","EX00100;"}));
        f.observe("ME0003"); const QString first=f.child<QLabel*>("nativeRadioMessage1")->text();
        QVERIFY(first.contains("003")); QVERIFY(first.contains("equal",Qt::CaseInsensitive));
        QVERIFY(f.child<QLabel*>("nativeRadioMessagePriority")->text().contains("Unknown"));
        f.observe("ME1003"); const QString second=f.child<QLabel*>("nativeRadioMessage2")->text();
        QVERIFY(second.contains("003")); QVERIFY(second.contains("USB",Qt::CaseInsensitive)); QVERIFY(first!=second);
        QCOMPARE(f.child<QLabel*>("nativeRadioMessage1")->text(),first);
        QVERIFY(f.child<QLabel*>("nativeRadioMessagePriority")->text().contains("2"));
        f.observe("ME1000"); QVERIFY(f.child<QLabel*>("nativeRadioMessagePriority")->text().contains("1"));
        f.observe("ME0999"); QVERIFY(f.child<QLabel*>("nativeRadioMessage1")->text().contains("999"));
        f.observe("ME0000"); QVERIFY(f.child<QLabel*>("nativeRadioMessagePriority")->text().contains("No active"));
        for (const QByteArray& bytes : f.wires) { QVERIFY(!bytes.startsWith("ME0003")); QVERIFY(!bytes.startsWith("ME1003")); }
    }
    void everyBeepValueHasRealUiWriteAndFreshReadbackInEveryMode() {
        for (char mode : QByteArray("12345679ABCDEF")) {
            Fixture f; f.install(mode); f.load();
            for (int value=0; value<=20; ++value) {
                f.stage(value); QVERIFY(f.apply()->isEnabled()); const QString previous=f.observed()->text(); f.wires.clear();
                QTest::mouseClick(f.apply(),Qt::LeftButton);
                const QByteArray expected="EX00100 "+QByteArray::number(value).rightJustified(3,'0')+";";
                QCOMPARE(f.wires,QList<QByteArray>({expected,"EX00100;"})); QCOMPARE(f.observed()->text(),previous); QVERIFY(!f.apply()->isEnabled());
                f.observe(expected.chopped(1)); QVERIFY(f.apply()->isEnabled());
                QCOMPARE(f.state.value("beep").toMap().value("value").toInt(),value);
                QCOMPARE(f.observed()->text(),value==0 ? QString("Off (0)") : QString("%1 (native index)").arg(value));
            }
        }
    }
    void resetIsDistinctFromOffAndDoesNotInventInitialValue() {
        Fixture f; f.load(); f.stage(0); f.wires.clear(); QTest::mouseClick(f.apply(),Qt::LeftButton);
        QCOMPARE(f.wires,QList<QByteArray>({"EX00100 000;","EX00100;"})); f.observe("EX00100 000");
        auto* reset=f.child<QPushButton*>("nativeRadioBeepReset"); QVERIFY(reset->isEnabled());
        f.wires.clear(); QTest::mouseClick(reset,Qt::LeftButton); QCOMPARE(f.wires,QList<QByteArray>({"EX001009;","EX00100;"}));
        QCOMPARE(f.observed()->text(),QString("Off (0)")); QVERIFY(!reset->isEnabled());
        f.observe("EX00100 007"); QCOMPARE(f.observed()->text(),QString("7 (native index)")); QVERIFY(reset->isEnabled()); QCOMPARE(f.draft()->value(),0);
    }
    void pendingContextWithdrawalCannotWriteAndKeepsDraft() {
        Fixture f; f.load(); f.stage(17); bool nested=false;
        f.onStatus=[&](const QVariantMap& status) { if (!nested && status.value("beep").toMap().value("pending").toBool()) { nested=true; f.observe("TX0"); } };
        f.wires.clear(); QTest::mouseClick(f.apply(),Qt::LeftButton); QVERIFY(nested); QVERIFY(!f.wires.contains("EX00100 017;")); QVERIFY(!f.apply()->isEnabled()); QCOMPARE(f.draft()->value(),17);
        f.onStatus={}; f.observe("RX"); f.observe("EX00100 010"); QCOMPARE(f.draft()->value(),17);
    }
    void olderPushAndLateGetCannotOverrideNewerMessageOrBeep() {
        Fixture f; f.load(); const QVariantMap old=f.state; f.observe("ME1003"); const QString latest=f.child<QLabel*>("nativeRadioMessage2")->text();
        f.publish(old); QCOMPARE(f.child<QLabel*>("nativeRadioMessage2")->text(),latest);
        bool nested=false; f.onResult=[&](quint64,const QVariant&) { if (!nested) { nested=true; f.observe("EX00100 020"); } };
        f.read(); QVERIFY(nested); QCOMPARE(f.observed()->text(),QString("20 (native index)")); f.onResult={};
    }
    void malformedHighRevisionCannotPoisonReadOrDraft() {
        Fixture f; f.load(); f.stage(17);
        for (const QString& revision : {QString("1000000"),QString("18446744073709551615")}) {
            f.observe("EX00100 010"); QVariantMap bad=f.state; bad["statusRevision"]=revision; QVariantMap beep=bad.value("beep").toMap(); beep["value"]=true; bad["beep"]=beep;
            f.publish(bad); QVERIFY(!f.apply()->isEnabled()); f.read(); QVERIFY(f.apply()->isEnabled()); QCOMPARE(f.draft()->value(),17); QCOMPARE(f.observed()->text(),QString("10 (native index)"));
        }
        for (const QVariant& token : {QVariant(1),QVariant("01"),QVariant("-1"),QVariant("18446744073709551616")}) {
            f.observe("EX00100 010"); QVariantMap bad=f.state; bad["statusRevision"]=token; f.publish(bad); QVERIFY(!f.apply()->isEnabled()); f.read(); QVERIFY(f.apply()->isEnabled()); QCOMPARE(f.draft()->value(),17);
        }
    }
    void changedExternalBaselineRetiresDraftAcrossValueCycle() {
        Fixture f; f.load(); f.stage(17); QVERIFY(f.apply()->isEnabled());
        f.observe("EX00100 010"); QCOMPARE(f.draft()->value(),17); QVERIFY(f.apply()->isEnabled());
        f.observe("EX00100 012"); QCOMPARE(f.draft()->value(),17); QVERIFY(!f.apply()->isEnabled());
        f.observe("EX00100 010"); QCOMPARE(f.draft()->value(),17); QVERIFY(!f.apply()->isEnabled());
        f.read(); QVERIFY(!f.apply()->isEnabled());
        f.wires.clear(); f.draft()->clearFocus(); QCoreApplication::processEvents();
        QVERIFY(!f.apply()->isEnabled()); QVERIFY(f.wires.isEmpty());
        f.stage(18); QVERIFY(f.apply()->isEnabled()); f.wires.clear();
        QTest::mouseClick(f.apply(),Qt::LeftButton); QCOMPARE(f.wires,QList<QByteArray>({"EX00100 018;","EX00100;"}));
        QCOMPARE(f.observed()->text(),QString("10 (native index)")); f.observe("EX00100 018");
        QCOMPARE(f.observed()->text(),QString("18 (native index)")); QVERIFY(f.apply()->isEnabled());
    }
    void malformedBodiesRecoverAndCannotForgePriority() {
        Fixture f; f.load();
        for (int kind=0; kind<5; ++kind) {
            f.observe("EX00100 010"); QVariantMap bad=f.state;
            if (kind==0) { bad["activeMessage"]=2; }
            if (kind==1) { QVariantMap message=bad.value("message1").toMap(); message["id"]="003"; bad["message1"]=message; }
            if (kind==2) { QVariantMap beep=bad.value("beep").toMap(); beep["value"]=21; bad["beep"]=beep; }
            if (kind==3) { bad["connected"]=1; }
            if (kind==4) { bad["context"]=QString(); }
            f.publish(bad); QVERIFY(!f.apply()->isEnabled()); QVERIFY(f.observed()->text().contains("Unknown")); f.read(); QVERIFY(f.apply()->isEnabled());
        }
    }
    void malformedHandoffCannotClaimConfirmation() {
        Fixture f; f.load(); f.stage(17); bool injected=false;
        f.onResult=[&](quint64 id,const QVariant&) { if (!injected) { injected=true; emit f.backend->extensionResult(id,true); } };
        QTest::mouseClick(f.apply(),Qt::LeftButton); QVERIFY(injected);
        QVERIFY(f.child<QLabel*>("nativeRadioMessagesStatus")->text().contains("Invalid result"));
        QCOMPARE(f.observed()->text(),QString("10 (native index)"));
        f.onResult={}; f.observe("EX00100 017"); QCOMPARE(f.observed()->text(),QString("17 (native index)"));
    }
    void existingPollPublishesIntentAvailabilityAndRetirement() {
        for (int kind=0; kind<4; ++kind) {
            Fixture f; f.load(); QVERIFY(f.apply()->isEnabled()); f.wires.clear();
            kenwood::KenwoodBackendTestAccess::intent(*f.backend,kind,true);
            kenwood::KenwoodBackendTestAccess::poll(*f.backend);
            QVERIFY(!f.apply()->isEnabled()); QVERIFY(!f.draft()->isEnabled());
            QVERIFY(!f.child<QPushButton*>("nativeRadioBeepReset")->isEnabled());
            QCOMPARE(f.observed()->text(),QString("10 (native index)")); QVERIFY(f.wires.isEmpty());
            kenwood::KenwoodBackendTestAccess::intent(*f.backend,kind,false);
            kenwood::KenwoodBackendTestAccess::poll(*f.backend);
            QVERIFY(f.apply()->isEnabled()); QVERIFY(f.draft()->isEnabled());
            QVERIFY(f.child<QPushButton*>("nativeRadioBeepReset")->isEnabled()); QVERIFY(f.wires.isEmpty());
        }
    }
    void expiryPublicationDeletionCannotContinueAvailabilityPoll() {
        Fixture f; f.load(); f.stage(17); QTest::mouseClick(f.apply(),Qt::LeftButton);
        QVERIFY(f.state.value("beep").toMap().value("pending").toBool());
        bool deleted=false; const QPointer<Backend> alive(f.backend);
        f.onStatus=[&](const QVariantMap& status) {
            if (!deleted && !status.value("beep").toMap().value("pending").toBool()) {
                deleted=true; f.model.setBackendForTest({},"kenwood");
            }
        };
        kenwood::KenwoodBackendTestAccess::poll(*f.backend,true); QVERIFY(deleted); QVERIFY(!alive);
        f.onStatus={};
    }
    void replacementReseedsAndFocusedEditorDestructionIsSafe() {
        Fixture f; f.load(); f.stage(17); f.install(); f.load(4); QCOMPARE(f.draft()->value(),4); QCOMPARE(f.observed()->text(),QString("4 (native index)"));
        f.stage(13); const QPointer<NativeRadioMessagesWidget> alive(f.widget.get()); f.widget.reset(); QVERIFY(alive.isNull()); QCoreApplication::processEvents();
    }
    void accessibilityNestedStatusAndDeletionAreGuarded() {
        QAccessible::setActive(true);
        if (!QAccessible::isActive()) { QSKIP("Accessibility activation unavailable; no reentry or deletion proof on this platform."); }
        Fixture f; f.load(); bool nested=false;
        const auto previous=QAccessible::installUpdateHandler(accessibleUpdate); const auto cleanup=qScopeGuard([previous] { accessibilityCallback={}; QAccessible::installUpdateHandler(previous); });
        accessibilityCallback=[&](QAccessibleEvent* event) { if (!nested && event->object()==f.observed()) { nested=true; f.observe("EX00100 020"); } };
        f.observe("EX00100 011"); QVERIFY(nested); QCOMPARE(f.observed()->text(),QString("20 (native index)"));
        nested=false; const QPointer<NativeRadioMessagesWidget> alive(f.widget.get());
        accessibilityCallback=[&](QAccessibleEvent* event) { if (!nested && event->object()==f.observed()) { nested=true; f.widget.reset(); } };
        f.observe("EX00100 012"); QVERIFY(nested); QVERIFY(alive.isNull());
    }
};
int main(int argc,char** argv) {
    TestSettingsProfile profile("native-radio-messages"); if (!profile.isValid()) { return 1; }
    QApplication app(argc,argv); NativeRadioMessagesWidgetTest test; return QTest::qExec(&test,argc,argv);
}
#include "native_radio_messages_widget_test.moc"
