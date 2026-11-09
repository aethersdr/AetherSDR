// Real native page -> model -> production backend; injected transport only.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeAutoModeWidget.h"
#include "models/RadioModel.h"
#include <QAccessible>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QList>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QTableWidget>
#include <QtTest>
#include <functional>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) { b.m_clock.start(); b.m_protocol.start("auto-mode-fixture","fixture-only",false,0); b.m_protocol.receive("##CN1;",1); b.m_protocol.receive("##ID1;",2); b.ready(); }
    static void observe(KenwoodBackend& b,const QByteArray& message) { b.adopt(message); }
    static void poll(KenwoodBackend& b,qint64 now) { b.pollAutoMode(now); }
};
}
namespace {
using namespace AetherSDR;
using Backend=kenwood::KenwoodBackend;
struct Division { qint64 upper; char mode; };
std::function<void(QAccessibleEvent*)> accessibilityCallback;
void accessibleUpdate(QAccessibleEvent* event) { if (accessibilityCallback) { accessibilityCallback(event); } }
struct Fixture {
    QList<QByteArray> wires;
    QList<Division> table{{7100000,'1'},{14200000,'2'}};
    bool enabled=false;
    bool replyReads=false;
    QVariantMap state;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(quint64,const QVariant&)> onResult;
    RadioModel model;
    Backend* backend=nullptr;
    std::unique_ptr<NativeAutoModeWidget> widget;
    qint64 now=0;
    Fixture() { install(); widget=std::make_unique<NativeAutoModeWidget>(model); widget->resize(1000,900); widget->show(); QCoreApplication::processEvents(); }
    void install() {
        auto owned=std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& bytes) { wires.append(bytes); if (replyReads) { const QByteArray response=reply(bytes); if (!response.isEmpty()) { observe(response); } } return true; },{}});
        backend=owned.get(); model.setBackendForTest(std::move(owned),"kenwood");
        QObject::connect(backend,&IRadioBackend::extensionStatus,&model,[this](const QString&,const QString& kind,const QVariantMap& status) { if (kind=="autoMode") { state=status; if (onStatus) { onStatus(status); } } });
        QObject::connect(backend,&IRadioBackend::extensionResult,&model,[this](quint64 id,const QVariant& result) { if (onResult) { onResult(id,result); } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"),QByteArray("FR0"),QByteArray("FT0"),QByteArray("FA00014074000"),QByteArray("OM02"),QByteArray("SF0000140740002"),QByteArray("TS0"),QByteArray("RX")}) { observe(message); }
        // Injection omits setupBackend lifecycle wiring; supply the real
        // public connection notification, never a copied status relay.
        emit model.connectionStateChanged(true);
    }
    QByteArray reply(const QByteArray& wire) const {
        if (wire=="MF;") { return "MF0"; }
        if (wire=="AM;") { return enabled ? "AM1" : "AM0"; }
        if (wire=="AS1;") { return "AS1"+QByteArray::number(table.size()).rightJustified(2,'0'); }
        if (wire.startsWith("AS2") && wire.size()==6) { const int index=wire.mid(3,2).toInt(); return "AS2"+QByteArray::number(index).rightJustified(2,'0')+(index<table.size() ? QByteArray::number(table[index].upper).rightJustified(11,'0')+table[index].mode : QByteArray("000000000000")); }
        return {};
    }
    void observe(const QByteArray& message) { kenwood::KenwoodBackendTestAccess::observe(*backend,message); }
    void poll(int count) { for (int index=0;index<count;++index) { kenwood::KenwoodBackendTestAccess::poll(*backend,now+=100); } }
    template<class T> T child(const QString& name) { return widget->findChild<T>(name); }
    QPushButton* button(const QString& name) { return child<QPushButton*>(name); }
    void click(const QString& name) { QTest::mouseClick(button(name),Qt::LeftButton); }
    QTableWidget* rows() { return child<QTableWidget*>("nativeAutoModeRows"); }
    void read() { replyReads=true; click("nativeAutoModeRead"); poll(35); }
    void stage(qint64 frequency,char mode) {
        auto* spin=child<QDoubleSpinBox*>("nativeAutoModeFrequencyDraft"); spin->setFocus(); spin->selectAll(); QTest::keyClicks(spin,spin->locale().toString(frequency)); spin->interpretText(); QCOMPARE(static_cast<qint64>(spin->value()),frequency);
        auto* combo=child<QComboBox*>("nativeAutoModeModeDraft"); const int target=combo->findData(QString(QChar::fromLatin1(mode))); QVERIFY(target>=0); combo->setFocus(); QTest::keyClick(combo,Qt::Key_Home); for (int index=0;index<target;++index) { QTest::keyClick(combo,Qt::Key_Down); } QTest::keyClick(combo,Qt::Key_Return);
    }
    void selectRow(int index) { rows()->scrollToItem(rows()->item(index,0)); QTest::mouseClick(rows()->viewport(),Qt::LeftButton,Qt::NoModifier,rows()->visualItemRect(rows()->item(index,0)).center()); }
    void publish(const QVariantMap& status) { emit backend->extensionStatus("kenwood","autoMode",status); }
    void exportQa(const QString& tag) {
        const QString directory=qEnvironmentVariable("AETHER_AUTOMODE_QA_DIR"); if (directory.isEmpty()) { return; }
        QVERIFY(QDir().mkpath(directory)); QVERIFY(widget->grab().save(QDir(directory).filePath(tag+".png")));
        QJsonArray columns; for (int index=0;index<rows()->columnCount();++index) { columns.append(QJsonObject{{"index",index},{"label",rows()->horizontalHeaderItem(index)->text()},{"width",rows()->columnWidth(index)}}); }
        QJsonArray labels; for (const QString& name : {QString("nativeAutoModeObserved"),QString("nativeAutoModeStatus"),QString("nativeAutoModeProgress"),QString("nativeAutoModeWriteStatus")}) { QLabel* label=child<QLabel*>(name); labels.append(QJsonObject{{"objectName",name},{"text",label->text()},{"width",label->width()},{"height",label->height()},{"sizeHintHeight",label->sizeHint().height()}}); }
        const QJsonObject geometry{{"widgetWidth",widget->width()},{"widgetHeight",widget->height()},{"rowCount",rows()->rowCount()},{"tableWidth",rows()->width()},{"tableHeight",rows()->height()},{"viewportWidth",rows()->viewport()->width()},{"viewportHeight",rows()->viewport()->height()},{"columns",columns},{"labels",labels},{"backendStatus",QJsonObject::fromVariantMap(state)}};
        QFile output(QDir(directory).filePath(tag+"-geometry.json")); QVERIFY(output.open(QIODevice::WriteOnly)); const QByteArray document=QJsonDocument(geometry).toJson(); QCOMPARE(output.write(document),qint64(document.size()));
    }
};
}
class NativeAutoModeWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void unsupportedControlsExplainConnectionOrSupportInsteadOfImpossibleEdit() {
        RadioModel model; NativeAutoModeWidget widget(model); widget.resize(1000,900); widget.show(); QCoreApplication::processEvents();
        const QStringList names{"nativeAutoModeRead","nativeAutoModeCancel","nativeAutoModeEnabledDraft","nativeAutoModeEnabledApply","nativeAutoModeFrequencyDraft","nativeAutoModeModeDraft","nativeAutoModeRegister","nativeAutoModeDelete"};
        for (const QString& name : names) {
            QWidget* control=widget.findChild<QWidget*>(name); QVERIFY(control); QVERIFY(control->isVisible()); QVERIFY(!control->isEnabled());
            for (const QString& reason : {control->toolTip(),control->accessibleDescription()}) {
                QVERIFY2(!reason.isEmpty(),qPrintable(name));
                QVERIFY2(reason.contains("connect",Qt::CaseInsensitive) || reason.contains("support",Qt::CaseInsensitive),qPrintable(name+": "+reason));
                QVERIFY2(!reason.contains("Edit or select again",Qt::CaseInsensitive),qPrintable(name+": "+reason));
            }
        }
        const QString label=widget.findChild<QLabel*>("nativeAutoModeObserved")->text();
        QVERIFY(!label.section("Snapshot:",1).trimmed().isEmpty());
    }
    void snapshotPresentationNamesCompleteObservedAndIncompleteStates() {
        Fixture f; f.read(); const QString complete=f.child<QLabel*>("nativeAutoModeObserved")->text().section("Snapshot:",1).trimmed();
        QVERIFY(!complete.isEmpty()); QVERIFY(complete.contains("complete",Qt::CaseInsensitive)); QVERIFY(complete.contains("observed",Qt::CaseInsensitive));
        f.click("nativeAutoModeRead"); const QString incomplete=f.child<QLabel*>("nativeAutoModeObserved")->text().section("Snapshot:",1).trimmed();
        QVERIFY(incomplete.contains("incomplete",Qt::CaseInsensitive)); f.click("nativeAutoModeCancel");
    }
    void completeIncoherentSurveyRetainsObservedRowsAndRefusesTableWrites() {
        Fixture f; f.replyReads=false; f.wires.clear(); f.click("nativeAutoModeRead");
        for (int index=0;index<35;++index) {
            const int before=f.wires.size(); f.poll(1); QCOMPARE(f.wires.size(),before+1);
            const QByteArray query=f.wires.last(); const QByteArray response=query=="AS1;" ? QByteArray("AS103") : f.reply(query);
            QVERIFY(!response.isEmpty()); f.observe(response);
        }
        QVERIFY(f.state.value("snapshot").toMap().value("complete").toBool());
        QVERIFY(!f.state.value("snapshot").toMap().value("coherent").toBool());
        QCOMPARE(f.state.value("count").toInt(),3);
        QCOMPARE(f.rows()->item(0,1)->text(),QString("populated"));
        QCOMPARE(f.rows()->item(0,2)->text(),QString("7100000"));
        QVERIFY(f.child<QLabel*>("nativeAutoModeObserved")->text().contains("Divisions: 3"));
        QVERIFY(!f.button("nativeAutoModeRegister")->isEnabled()); QVERIFY(!f.button("nativeAutoModeDelete")->isEnabled());
    }
    void uneditedEnabledDraftFollowsObservedRadioState() {
        Fixture f; f.read(); auto* draft=f.child<QCheckBox*>("nativeAutoModeEnabledDraft"); QCOMPARE(draft->checkState(),Qt::Unchecked);
        f.observe("AM1"); QCOMPARE(draft->checkState(),Qt::Checked); QVERIFY(f.button("nativeAutoModeEnabledApply")->isEnabled());
        QVERIFY(f.child<QLabel*>("nativeAutoModeObserved")->text().contains("On"));
    }
    void keyboardSelectionCannotDeletePreviouslyClickedRow() {
        Fixture f; f.table={{1000000,'2'},{2000000,'2'},{3000000,'2'}}; f.read(); f.selectRow(0); QTest::keyClick(f.rows(),Qt::Key_Down); QCOMPARE(f.rows()->currentRow(),1);
        f.wires.clear(); f.click("nativeAutoModeDelete"); QVERIFY(!f.wires.contains("AS300;"));
        QVERIFY(f.wires.isEmpty() || f.wires==QList<QByteArray>({"AS301;"}));
    }
    void initialCachedGetterAndSelectionCannotWrite() {
        Fixture f; QCOMPARE(f.rows()->rowCount(),32); QCOMPARE(f.child<QCheckBox*>("nativeAutoModeEnabledDraft")->checkState(),Qt::PartiallyChecked);
        QVERIFY(!f.button("nativeAutoModeRegister")->isEnabled()); QVERIFY(!f.button("nativeAutoModeDelete")->isEnabled());
        f.wires.clear(); f.rows()->selectRow(3); QTest::keyClick(f.rows(),Qt::Key_Return); QVERIFY(f.wires.isEmpty());
        f.read(); QCOMPARE(f.wires.size(),35); QCOMPARE(f.wires[0],QByteArray("MF;")); QCOMPARE(f.wires[1],QByteArray("AM;")); QCOMPARE(f.wires[2],QByteArray("AS1;")); QCOMPARE(f.wires.last(),QByteArray("AS231;"));
        f.exportQa("native-auto-mode-known-read");
    }
    void bothEnableStatesUseRealCheckboxApplyAndReadback() {
        Fixture f; f.read();
        for (bool enabled : {true,false}) {
            auto* draft=f.child<QCheckBox*>("nativeAutoModeEnabledDraft"); draft->setFocus(); QTest::keyClick(draft,Qt::Key_Space); QCOMPARE(draft->isChecked(),enabled); f.wires.clear();
            const QString previous=f.child<QLabel*>("nativeAutoModeObserved")->text(); f.click("nativeAutoModeEnabledApply"); QCOMPARE(f.wires,QList<QByteArray>({enabled ? "AM1;" : "AM0;"}));
            QCOMPARE(f.child<QLabel*>("nativeAutoModeObserved")->text(),previous); QVERIFY(!f.button("nativeAutoModeEnabledApply")->isEnabled()); f.enabled=enabled; f.poll(1);
            QVERIFY(f.button("nativeAutoModeEnabledApply")->isEnabled()); QVERIFY(f.child<QLabel*>("nativeAutoModeWriteStatus")->text().contains("observed_consistent"));
        }
    }
    void allFourteenStoredPoliciesRegisterThroughUiWithoutOptimism() {
        for (char mode : QByteArray("12345679ABCDEF")) {
            Fixture f; f.read(); f.stage(20000000,mode); QVERIFY(f.button("nativeAutoModeRegister")->isEnabled()); f.wires.clear(); f.click("nativeAutoModeRegister");
            QCOMPARE(f.wires,QList<QByteArray>({QByteArray("AS000020000000")+mode+";"}));
            QCOMPARE(f.state.value("count").toInt(),2); QVERIFY(!f.button("nativeAutoModeRegister")->isEnabled());
            f.table.append({20000000,mode}); f.poll(35); QCOMPARE(f.state.value("count").toInt(),3); QVERIFY(f.state.value("snapshot").toMap().value("coherent").toBool());
            QVERIFY(f.child<QLabel*>("nativeAutoModeWriteStatus")->text().contains("observed_consistent"));
            if (mode=='1') { f.rows()->selectRow(2); f.exportQa("native-auto-mode-known-write"); }
        }
    }
    void exactElevenDigitDraftAndDuplicateBoundaryRefusal() {
        Fixture f; f.read(); f.stage(99999999999LL,'2'); f.wires.clear(); f.click("nativeAutoModeRegister"); QCOMPARE(f.wires,QList<QByteArray>({"AS0999999999992;"}));
        f.click("nativeAutoModeCancel"); QVERIFY(f.child<QLabel*>("nativeAutoModeWriteStatus")->text().contains("indeterminate"));
        f.read(); f.stage(7100000,'2'); QVERIFY(!f.button("nativeAutoModeRegister")->isEnabled());
    }
    void eachDenseIndexDeletesThroughActualSelectionAndPostRead() {
        for (int selected=0;selected<32;++selected) {
            Fixture f; f.table.clear(); for (int index=0;index<32;++index) { f.table.append({1000000+index*10000,'2'}); } f.read(); f.selectRow(selected); QVERIFY(f.button("nativeAutoModeDelete")->isEnabled());
            f.wires.clear(); f.click("nativeAutoModeDelete"); QCOMPARE(f.wires,QList<QByteArray>({"AS3"+QByteArray::number(selected).rightJustified(2,'0')+";"})); QCOMPARE(f.state.value("count").toInt(),32);
            f.table.removeAt(selected); f.poll(35); QCOMPARE(f.state.value("count").toInt(),31); QVERIFY(f.child<QLabel*>("nativeAutoModeWriteStatus")->text().contains("observed_consistent"));
        }
    }
    void malformedHighDescriptorCannotPoisonCachedRecoveryOrDrafts() {
        Fixture f; f.read(); f.stage(99999999999LL,'2');
        for (const QString& revision : {QString("1000000"),QString("18446744073709551615")}) {
            f.observe("AM0"); QVariantMap bad=f.state; bad["statusRevision"]=revision; bad["bankKnown"]=1; f.publish(bad); QVERIFY(!f.button("nativeAutoModeRegister")->isEnabled());
            f.click("nativeAutoModeRead"); QCOMPARE(static_cast<qint64>(f.child<QDoubleSpinBox*>("nativeAutoModeFrequencyDraft")->value()),99999999999LL); QCOMPARE(f.child<QComboBox*>("nativeAutoModeModeDraft")->currentData().toString(),QString("2"));
        }
    }
    void transientSafetyWithdrawalRejectsPendingUiWrite() {
        Fixture f; f.read(); f.stage(20000000,'2'); bool nested=false;
        f.onStatus=[&](const QVariantMap& status) { if (!nested && status.value("operations").toMap().value("register").toMap().value("pending").toBool()) { nested=true; f.observe("TX0"); f.observe("RX"); } };
        f.wires.clear(); f.click("nativeAutoModeRegister"); QVERIFY(nested); QVERIFY(!f.wires.contains("AS0000200000002;"));
    }
    void replacementCannotPresentOldRequestHandoffAsCurrent() {
        Fixture f; f.read(); f.stage(20000000,'2'); f.click("nativeAutoModeRegister");
        const QString old=f.child<QLabel*>("nativeAutoModeStatus")->text(); QVERIFY(!old.isEmpty());
        f.replyReads=false; f.wires.clear(); f.install();
        const QString current=f.child<QLabel*>("nativeAutoModeStatus")->text();
        QVERIFY(current.isEmpty() || current.contains("previous",Qt::CaseInsensitive) || current.contains("old context",Qt::CaseInsensitive));
        QVERIFY(current!=old); QVERIFY(f.child<QLabel*>("nativeAutoModeWriteStatus")->text().contains("none"));
        for (const QByteArray& wire : f.wires) { QVERIFY(!wire.startsWith("AS0")); QVERIFY(!wire.startsWith("AS3")); QVERIFY(wire!="AM0;" && wire!="AM1;"); }
    }
    void accessibilityReentryAndFocusedTeardownAreExplicit() {
        Fixture f; f.read(); bool nested=false;
        QAccessible::setActive(true); if (!QAccessible::isActive()) { QSKIP("Accessibility unavailable; no callback reentry proof."); }
        const auto previous=QAccessible::installUpdateHandler(accessibleUpdate); const auto cleanup=qScopeGuard([previous] { accessibilityCallback={}; QAccessible::installUpdateHandler(previous); });
        accessibilityCallback=[&](QAccessibleEvent* event) { if (!nested && event->object()==f.child<QLabel*>("nativeAutoModeObserved")) { nested=true; f.widget.reset(); } };
        const QPointer<NativeAutoModeWidget> alive(f.widget.get()); f.observe("AM1"); QVERIFY(nested); QVERIFY(!alive);
        accessibilityCallback={}; f.widget=std::make_unique<NativeAutoModeWidget>(f.model); f.widget->show(); f.child<QDoubleSpinBox*>("nativeAutoModeFrequencyDraft")->setFocus(); f.widget.reset(); QCoreApplication::processEvents();
    }
};
int main(int argc,char** argv) { TestSettingsProfile profile("native-auto-mode"); if (!profile.isValid()) { return 1; } QApplication app(argc,argv); NativeAutoModeWidgetTest test; return QTest::qExec(&test,argc,argv); }
#include "native_auto_mode_widget_test.moc"
