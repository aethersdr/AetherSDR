// Production widget -> RadioModel -> Kenwood backend, injected transport only.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeOperatingDisplayWidget.h"
#include "models/RadioModel.h"
#include <QAccessible>
#include <QAbstractItemModel>
#include <QByteArray>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QScopeGuard>
#include <QStandardItemModel>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtTest>
#include <functional>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) { b.m_clock.start(); b.m_protocol.start("display-fixture","fixture-only",false,0); b.m_protocol.receive("##CN1;",1); b.m_protocol.receive("##ID1;",2); b.ready(); }
    static void observe(KenwoodBackend& b,const QByteArray& message) { b.adopt(message); }
    static void poll(KenwoodBackend& b,qint64 now) { b.pollOperatingDisplay(now); }
};
}
namespace {
using namespace AetherSDR;
using Backend=kenwood::KenwoodBackend;
std::function<void(QAccessibleEvent*)> accessibilityCallback;
void accessibleUpdate(QAccessibleEvent* event) { if (accessibilityCallback) { accessibilityCallback(event); } }
struct Fixture {
    QList<QByteArray> wires;
    QVariantMap state;
    int basic=0,screen=0,edit=0;
    bool xv=false,replyReads=false;
    bool initialXvKnown=true;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(quint64,const QVariant&)> onResult;
    RadioModel model;
    Backend* backend=nullptr;
    std::unique_ptr<NativeOperatingDisplayWidget> widget;
    qint64 now=0;
    explicit Fixture(bool knownXv = true) : initialXvKnown(knownXv) { install(); makeWidget(); }
    void install() {
        auto owned=std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& bytes) { wires.append(bytes); if (replyReads) { const QByteArray response=reply(bytes); if (!response.isEmpty()) { observe(response); } } return true; },{}}); backend=owned.get(); model.setBackendForTest(std::move(owned),"kenwood");
        QObject::connect(backend,&IRadioBackend::extensionStatus,&model,[this](const QString&,const QString& kind,const QVariantMap& status) { if (kind=="operatingDisplay") { state=status; if (onStatus) { onStatus(status); } } });
        QObject::connect(backend,&IRadioBackend::extensionResult,&model,[this](quint64 id,const QVariant& result) { if (onResult) { onResult(id,result); } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"),QByteArray("FR0"),QByteArray("FT0"),QByteArray("FA00014074000"),QByteArray("OM02"),QByteArray("SF0000140740002"),QByteArray("TS0"),QByteArray("RX")}) { observe(message); }
        if (initialXvKnown) { observe("XV0"); }
        // Test seam omits setupBackend lifecycle wiring. Supply only its real
        // public notification, never a copied status or control relay.
        emit model.connectionStateChanged(true);
    }
    void makeWidget() { widget=std::make_unique<NativeOperatingDisplayWidget>(model); widget->resize(1000,750); widget->show(); QCoreApplication::processEvents(); }
    QByteArray reply(const QByteArray& wire) const { if (wire=="MF;") { return "MF0"; } if (wire=="XV;") { return xv ? "XV1" : "XV0"; } if (wire=="DS0;") { return "DS0"+QByteArray::number(basic); } if (wire=="DS1;") { return "DS1"+QByteArray::number(screen).rightJustified(3,'0'); } if (wire=="DS2;") { return "DS2"+QByteArray::number(edit); } return {}; }
    void observe(const QByteArray& message) { kenwood::KenwoodBackendTestAccess::observe(*backend,message); }
    void poll(int count=1) { for (int index=0;index<count;++index) { kenwood::KenwoodBackendTestAccess::poll(*backend,now+=100); } }
    template<class T> T child(const QString& name) { return widget->findChild<T>(name); }
    QPushButton* button(const QString& name) { return child<QPushButton*>(name); }
    void click(const QString& name) { QTest::mouseClick(button(name),Qt::LeftButton); }
    QComboBox* draft() { return child<QComboBox*>("nativeOperatingDisplayDraft"); }
    void stage(int value) { const int index=draft()->findData(value); QVERIFY(index>=0); draft()->setFocus(); QTest::keyClick(draft(),Qt::Key_Home); for (int step=0;step<index;++step) { QTest::keyClick(draft(),Qt::Key_Down); } QTest::keyClick(draft(),Qt::Key_Return); QCOMPARE(draft()->currentData().toInt(),value); }
    void read() { replyReads=true; click("nativeOperatingDisplayRead"); poll(5); }
    void publish(const QVariantMap& status) { emit backend->extensionStatus("kenwood","operatingDisplay",status); }
    void exportQa(const QString& tag) {
        const QString directory=qEnvironmentVariable("AETHER_OPERATING_DISPLAY_QA_DIR"); if (directory.isEmpty()) { return; } QVERIFY(QDir().mkpath(directory)); QVERIFY(widget->grab().save(QDir(directory).filePath(tag+".png")));
        QJsonArray labels; for (const QString& name : {QString("nativeOperatingDisplayBasic"),QString("nativeOperatingDisplayTransverter"),QString("nativeOperatingDisplayScreen"),QString("nativeOperatingDisplayEdit"),QString("nativeOperatingDisplayRestrictions"),QString("nativeOperatingDisplayProgress"),QString("nativeOperatingDisplayWriteStatus"),QString("nativeOperatingDisplayStatus")}) { QLabel* label=child<QLabel*>(name); labels.append(QJsonObject{{"objectName",name},{"text",label->text()},{"height",label->height()},{"width",label->width()},{"sizeHintHeight",label->sizeHint().height()}}); }
        QFile output(QDir(directory).filePath(tag+"-geometry.json")); QVERIFY(output.open(QIODevice::WriteOnly)); const QByteArray document=QJsonDocument(QJsonObject{{"widgetWidth",widget->width()},{"widgetHeight",widget->height()},{"labels",labels},{"backendStatus",QJsonObject::fromVariantMap(state)}}).toJson(); QCOMPARE(output.write(document),qint64(document.size()));
    }
};
}
class NativeOperatingDisplayWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void unknownAndUnsupportedControlsStayVisibleWithTruthfulReasons() {
        RadioModel model; NativeOperatingDisplayWidget widget(model); widget.resize(1000,750); widget.show(); QCoreApplication::processEvents();
        for (const QString& name : {QString("nativeOperatingDisplayRead"),QString("nativeOperatingDisplayCancel"),QString("nativeOperatingDisplayDraft"),QString("nativeOperatingDisplayApply"),QString("nativeOperatingDisplayEscape")}) { QWidget* control=widget.findChild<QWidget*>(name); QVERIFY(control); QVERIFY(control->isVisible()); QVERIFY(!control->isEnabled()); QVERIFY(control->accessibleDescription().contains("connect",Qt::CaseInsensitive) || control->accessibleDescription().contains("support",Qt::CaseInsensitive)); QVERIFY(!control->toolTip().isEmpty()); }
        QVERIFY(widget.findChild<QLabel*>("nativeOperatingDisplayBasic")->text().contains("Unknown"));
    }
    void cachedGetAndExplicitFiveReadsNeverNavigateOrSet() {
        Fixture f; QVERIFY(!f.button("nativeOperatingDisplayApply")->isEnabled()); QVERIFY(!f.button("nativeOperatingDisplayEscape")->isEnabled());
        f.wires.clear(); f.read(); QCOMPARE(f.wires,QList<QByteArray>({"MF;","XV;","DS0;","DS1;","DS2;"}));
        QVERIFY(f.child<QLabel*>("nativeOperatingDisplayBasic")->text().contains("Standard")); f.exportQa("native-operating-display-known-read");
    }
    void bothBasicValuesUseActualApplyAndThreeObservedPostReads() {
        Fixture f; f.read();
        for (int value : {1,0}) {
            f.stage(value); f.wires.clear(); const QString old=f.child<QLabel*>("nativeOperatingDisplayBasic")->text(); f.click("nativeOperatingDisplayApply");
            QCOMPARE(f.wires,QList<QByteArray>({"DS0"+QByteArray::number(value)+";"})); QCOMPARE(f.child<QLabel*>("nativeOperatingDisplayBasic")->text(),old); QVERIFY(!f.button("nativeOperatingDisplayApply")->isEnabled());
            f.basic=value; f.poll(3); QCOMPARE(f.wires.mid(1),QList<QByteArray>({"DS0;","DS1;","DS2;"})); QVERIFY(f.button("nativeOperatingDisplayApply")->isEnabled()); QVERIFY(f.child<QLabel*>("nativeOperatingDisplayWriteStatus")->text().contains("observed_consistent"));
        }
    }
    void uneditedDraftFollowsObservedAndSameValueApplyIsExplicit() {
        Fixture f; f.read(); f.observe("DS01"); QCOMPARE(f.draft()->currentData().toInt(),1); QVERIFY(f.button("nativeOperatingDisplayApply")->isEnabled());
        f.stage(0); f.observe("DS00"); QCOMPARE(f.draft()->currentData().toInt(),0); f.wires.clear(); f.click("nativeOperatingDisplayApply"); QCOMPARE(f.wires,QList<QByteArray>({"DS00;"}));
    }
    void standardExitWorksWithUnknownOrOffTransverterAndMemoryUnknownModeTf() {
        for (bool knownXv : {false,true}) {
            Fixture f(false); f.observe("FR3"); f.observe("TS1"); f.observe("DS01"); if (knownXv) { f.observe("XV0"); }
            f.stage(0); QVERIFY(f.button("nativeOperatingDisplayApply")->isEnabled()); f.wires.clear(); f.click("nativeOperatingDisplayApply"); QCOMPARE(f.wires,QList<QByteArray>({"DS00;"}));
        }
    }
    void swlEntryRequiresKnownOffButDoesNotInventScreenBans() {
        Fixture f(false); f.observe("DS00");
        auto* choices=qobject_cast<QStandardItemModel*>(f.draft()->model()); QVERIFY(choices); QVERIFY(!choices->item(f.draft()->findData(1))->isEnabled());
        f.observe("XV0"); f.observe("DS1175"); f.observe("DS25"); f.stage(1); QVERIFY(f.button("nativeOperatingDisplayApply")->isEnabled());
        f.wires.clear(); f.click("nativeOperatingDisplayApply"); QCOMPARE(f.wires,QList<QByteArray>({"DS01;"}));
    }
    void allEditStatesAndKnownReservedUnlistedScreensAreVisible() {
        Fixture f; f.read(); const QStringList words{"off","frequency","log","channel","character","memory"};
        for (int value=0;value<6;++value) { f.observe("DS2"+QByteArray::number(value)); const QString label=f.child<QLabel*>("nativeOperatingDisplayEdit")->text(); QVERIFY(label.contains(QString::number(value))); QVERIFY(label.contains(words[value],Qt::CaseInsensitive)); }
        for (int id : {0,1,3,10,175,186,999}) { const QString raw=QString::number(id).rightJustified(3,'0'); f.observe("DS1"+raw.toLatin1()); const QString label=f.child<QLabel*>("nativeOperatingDisplayScreen")->text(); QVERIFY(label.contains(raw)); if (id==1) { QVERIFY(label.contains("reserved")); } if (id==175 || id==999) { QVERIFY(label.contains("unlisted")); } }
        f.observe("ME0010"); QVERIFY(f.child<QLabel*>("nativeOperatingDisplayScreen")->text().contains("999"));
    }
    void explicitEscapeHasNoAutomaticZeroOrVerifiedOutcome() {
        Fixture f; f.observe("DS1175"); f.observe("DS25"); QVERIFY(f.button("nativeOperatingDisplayEscape")->isEnabled());
        f.wires.clear(); f.click("nativeOperatingDisplayEscape"); QCOMPARE(f.wires,QList<QByteArray>({"DS3;"})); QVERIFY(f.child<QLabel*>("nativeOperatingDisplayScreen")->text().contains("175"));
        f.replyReads=true; f.screen=3; f.edit=4; f.poll(3); QCOMPARE(f.wires,QList<QByteArray>({"DS3;","DS0;","DS1;","DS2;"}));
        QVERIFY(f.child<QLabel*>("nativeOperatingDisplayScreen")->text().contains("003")); QVERIFY(f.child<QLabel*>("nativeOperatingDisplayEdit")->text().contains("4"));
        QVERIFY(f.child<QLabel*>("nativeOperatingDisplayWriteStatus")->text().contains("sent_unverified")); QVERIFY(!f.child<QLabel*>("nativeOperatingDisplayWriteStatus")->text().contains("observed_consistent")); f.exportQa("native-operating-display-post-escape");
    }
    void cancelReadCannotUndoSentEscapeAndNeverRepeatsIt() {
        Fixture f; f.read(); f.click("nativeOperatingDisplayEscape"); f.wires.clear(); f.click("nativeOperatingDisplayCancel"); f.poll(8);
        QVERIFY(f.wires.isEmpty()); QVERIFY(f.child<QLabel*>("nativeOperatingDisplayWriteStatus")->text().contains("indeterminate")); QVERIFY(!f.button("nativeOperatingDisplayCancel")->isEnabled());
    }
    void failedDiagnosticDoesNotDisableBasicStandardExit() {
        Fixture f; f.read(); f.observe("DS1003"); f.wires.clear(); f.replyReads=false; f.click("nativeOperatingDisplayRead");
        for (int index=0;index<5;++index) { f.poll(); const QByteArray query=f.wires.last(); if (query=="DS1;") { kenwood::KenwoodBackendTestAccess::poll(*f.backend,f.now+=2000); } else { f.observe(f.reply(query)); } }
        QVERIFY(!f.state.value("screenKnown").toBool()); QVERIFY(f.state.value("basicKnown").toBool()); f.stage(0); QVERIFY(f.button("nativeOperatingDisplayApply")->isEnabled()); QVERIFY(!f.button("nativeOperatingDisplayEscape")->isEnabled());
    }
    void transverterOnRetiresDisplaySessionAndLeavesControlsUnavailable() {
        Fixture f; f.read(); f.observe("XV1"); QVERIFY(!f.backend->isConnected());
        emit f.model.connectionStateChanged(false);
        for (const QString& name : {QString("nativeOperatingDisplayRead"), QString("nativeOperatingDisplayDraft"), QString("nativeOperatingDisplayApply"), QString("nativeOperatingDisplayEscape")}) {
            QWidget* control = f.child<QWidget*>(name); QVERIFY(control); QVERIFY(control->isVisible());
            QVERIFY(!control->isEnabled()); QVERIFY(!control->accessibleDescription().isEmpty());
        }
        f.wires.clear(); f.button("nativeOperatingDisplayApply")->click(); f.button("nativeOperatingDisplayEscape")->click();
        f.observe("XV0"); QVERIFY(!f.backend->isConnected()); QVERIFY(f.wires.isEmpty());
    }
    void transientXvWithdrawalPreventsOldPendingEntry() {
        Fixture f; f.read(); f.stage(1); bool nested=false;
        f.onStatus=[&](const QVariantMap& status) { if (!nested && status.value("operation").toMap().value("basic").toMap().value("pending").toBool()) { nested=true; f.observe("XV1"); f.observe("XV0"); } };
        f.wires.clear(); f.click("nativeOperatingDisplayApply"); QVERIFY(nested); QVERIFY(!f.backend->isConnected()); QVERIFY(!f.wires.contains("DS01;"));
    }
    void malformedHighBodyAndTokenCannotPoisonReadRecoveryOrDraft() {
        Fixture f; f.read(); f.stage(1);
        for (const QString& revision : {QString("1000000"),QString("18446744073709551615")}) { f.observe("DS00"); QVariantMap bad=f.state; bad["statusRevision"]=revision; bad["basic"]=true; f.publish(bad); QVERIFY(!f.button("nativeOperatingDisplayApply")->isEnabled()); f.read(); QCOMPARE(f.draft()->currentData().toInt(),1); QVERIFY(f.button("nativeOperatingDisplayApply")->isEnabled()); }
        for (const QVariant& token : {QVariant("01"),QVariant(true),QVariant("18446744073709551616")}) { f.observe("DS00"); QVariantMap bad=f.state; bad["statusRevision"]=QString("1000000"); QVariantMap refresh=bad.value("refresh").toMap(); refresh["token"]=token; bad["refresh"]=refresh; f.publish(bad); QVERIFY(!f.button("nativeOperatingDisplayApply")->isEnabled()); f.read(); QCOMPARE(f.draft()->currentData().toInt(),1); }
    }
    void unknownEditMapUnexpectedKeyCannotPoisonProducerFloor() {
        Fixture f; f.read(); f.stage(1);
        for (const QString& revision : {QString("1000000"),QString("18446744073709551615")}) {
            f.observe("DS00"); QVariantMap bad=f.state; bad["statusRevision"]=revision; bad["editKnown"]=false;
            QVariantMap edit=bad.value("edit").toMap(); edit["known"]=false; edit.remove("value"); edit["unexpected"]=true; bad["edit"]=edit;
            QVariantMap operations=bad.value("operation").toMap(); QVariantMap escape=operations.value("escape").toMap(); escape["editable"]=false; escape["reason"]=QString("Edit state unknown"); operations["escape"]=escape; bad["operation"]=operations;
            f.publish(bad); QVERIFY(!f.button("nativeOperatingDisplayApply")->isEnabled());
            QVERIFY(f.child<QLabel*>("nativeOperatingDisplayBasic")->text().contains("Unknown"));
            f.read(); QCOMPARE(f.draft()->currentData().toInt(),1); QVERIFY(f.button("nativeOperatingDisplayApply")->isEnabled());
            QVERIFY(f.child<QLabel*>("nativeOperatingDisplayBasic")->text().contains("Standard"));
        }
    }
    void escapeCannotClaimObservedConsistencyAndPoisonReadRecovery() {
        Fixture f; f.read(); f.click("nativeOperatingDisplayEscape"); f.screen=3; f.edit=4; f.poll(3);
        QCOMPARE(f.state.value("lastWrite").toMap().value("operation").toString(),QString("escape"));
        QCOMPARE(f.state.value("lastWrite").toMap().value("state").toString(),QString("sent_unverified"));
        QVariantMap bad=f.state; bad["statusRevision"]=QString("1000000");
        QVariantMap write=bad.value("lastWrite").toMap(); write["state"]=QString("observed_consistent"); bad["lastWrite"]=write;
        f.publish(bad); QVERIFY(!f.button("nativeOperatingDisplayApply")->isEnabled());
        QVERIFY(!f.child<QLabel*>("nativeOperatingDisplayWriteStatus")->text().contains("observed_consistent"));
        f.read(); QVERIFY(f.button("nativeOperatingDisplayApply")->isEnabled());
        QVERIFY(f.child<QLabel*>("nativeOperatingDisplayWriteStatus")->text().contains("sent_unverified"));
        QVERIFY(f.child<QLabel*>("nativeOperatingDisplayScreen")->text().contains("003"));
        QVERIFY(f.child<QLabel*>("nativeOperatingDisplayEdit")->text().contains("4"));
    }
    void olderPushLateGetAndReplacementCannotShowOldHandoff() {
        Fixture f; f.read(); const QVariantMap old=f.state; f.observe("DS01"); const QString current=f.child<QLabel*>("nativeOperatingDisplayBasic")->text(); f.publish(old); QCOMPARE(f.child<QLabel*>("nativeOperatingDisplayBasic")->text(),current);
        f.widget.reset(); bool nested=false; f.onResult=[&](quint64,const QVariant&) { if (!nested) { nested=true; f.observe("DS00"); } }; f.makeWidget(); QVERIFY(nested); QVERIFY(f.child<QLabel*>("nativeOperatingDisplayBasic")->text().contains("Standard")); f.onResult={};
        f.stage(1); f.click("nativeOperatingDisplayApply"); QVERIFY(!f.child<QLabel*>("nativeOperatingDisplayStatus")->text().isEmpty()); f.replyReads=false; f.wires.clear(); f.install(); QVERIFY(f.child<QLabel*>("nativeOperatingDisplayStatus")->text().isEmpty()); for (const QByteArray& wire : f.wires) { QVERIFY(wire!="DS00;" && wire!="DS01;" && wire!="DS3;"); }
    }
    void itemModelAndAccessibilityReentryCannotResumeOldRender() {
        Fixture f; f.read(); bool nested=false;
        const auto connection=QObject::connect(f.draft()->model(),&QAbstractItemModel::rowsInserted,&f.model,[&] { if (!nested) { nested=true; f.observe("DS01"); } });
        f.observe("DS00"); QVERIFY(nested); QCOMPARE(f.draft()->currentData().toInt(),1); QObject::disconnect(connection);
        QAccessible::setActive(true); if (!QAccessible::isActive()) { QSKIP("Accessibility unavailable; model reentry checked, no accessibility proof."); }
        const auto previous=QAccessible::installUpdateHandler(accessibleUpdate); const auto cleanup=qScopeGuard([previous] { accessibilityCallback={}; QAccessible::installUpdateHandler(previous); }); nested=false;
        accessibilityCallback=[&](QAccessibleEvent* event) { if (!nested && event->object()==f.child<QLabel*>("nativeOperatingDisplayBasic")) { nested=true; f.widget.reset(); } };
        const QPointer<NativeOperatingDisplayWidget> alive(f.widget.get()); f.observe("DS00"); QVERIFY(nested); QVERIFY(!alive);
        accessibilityCallback={}; f.makeWidget(); f.draft()->setFocus(); f.widget.reset(); QCoreApplication::processEvents();
    }
};
int main(int argc,char** argv) { TestSettingsProfile profile("native-operating-display"); if (!profile.isValid()) { return 1; } QApplication app(argc,argv); NativeOperatingDisplayWidgetTest test; return QTest::qExec(&test,argc,argv); }
#include "native_operating_display_widget_test.moc"
