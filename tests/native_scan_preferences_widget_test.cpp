// Real native widget/model/backend routes, injected transport only; no radio.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeScanPreferencesWidget.h"
#include "models/RadioModel.h"
#include <QAbstractItemModel>
#include <QAccessible>
#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QMap>
#include <QPair>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QtTest>
#include <functional>
#include <memory>
namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) { b.m_clock.start(); b.m_protocol.start("scan-preferences-fixture","fixture-only",false,0); b.m_protocol.receive("##CN1;",1); b.m_protocol.receive("##ID1;",2); b.ready(); }
    static void observe(KenwoodBackend& b,const QByteArray& bytes) { b.adopt(bytes); }
    static void poll(KenwoodBackend& b,qint64 now) { b.pollScanPreferences(now); }
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
    QMap<QString,int> values{{"hold",0},{"resume",0}};
    int bank=0; char mode='2'; bool replyReads=false; qint64 now=0;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(quint64,const QVariant&)> onResult;
    RadioModel model;
    Backend* backend=nullptr;
    std::unique_ptr<NativeScanPreferencesWidget> widget;
    Fixture() { install(); makeWidget(); }
    void install() {
        auto owned=std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& bytes) { wires.append(bytes); if (replyReads) { const QByteArray response=reply(bytes); if (!response.isEmpty()) { observe(response); } } return true; },{}}); backend=owned.get(); model.setBackendForTest(std::move(owned),"kenwood");
        QObject::connect(backend,&IRadioBackend::extensionStatus,&model,[this](const QString&,const QString& kind,const QVariantMap& status) { if (kind=="scanPreferences") { state=status; if (onStatus) { onStatus(status); } } }); QObject::connect(backend,&IRadioBackend::extensionResult,&model,[this](quint64 request,const QVariant& result) { if (onResult) { onResult(request,result); } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend); for (const QByteArray& message : {QByteArray("ID024"),QByteArray("FR0"),QByteArray("FT0"),QByteArray("FA00014074000"),QByteArray("OM02"),QByteArray("SF0000140740002"),QByteArray("TS0"),QByteArray("RX")}) { observe(message); }
        // setBackendForTest omits setupBackend lifecycle wiring. Supply only
        // the public notification; do not copy control/status relays.
        emit model.connectionStateChanged(true);
    }
    void makeWidget() { widget=std::make_unique<NativeScanPreferencesWidget>(model); widget->resize(1100,700); widget->show(); QCoreApplication::processEvents(); }
    QByteArray reply(const QByteArray& wire) const { if (wire=="MF;") { return "MF"+QByteArray::number(bank); } if (wire=="EX00404;") { return "EX00404 "+QByteArray::number(values["hold"]).rightJustified(3,'0'); } if (wire=="EX00405;") { return "EX00405 "+QByteArray::number(values["resume"]).rightJustified(3,'0'); } if (wire=="SF0;") { return QByteArray("SF000014074000")+mode; } return {}; }
    void observe(const QByteArray& message) { kenwood::KenwoodBackendTestAccess::observe(*backend,message); }
    void poll(int count=1) { for (int index=0;index<count;++index) { kenwood::KenwoodBackendTestAccess::poll(*backend,now+=100); } }
    template<class T> T child(const QString& name) { return widget->findChild<T>(name); }
    QPushButton* button(const QString& name) { return child<QPushButton*>(name); }
    void click(const QString& name) { QTest::mouseClick(button(name),Qt::LeftButton); }
    QComboBox* draft(const QString& id) { return child<QComboBox*>("nativeScanPreferences_"+id+"_Draft"); }
    QString observed(const QString& id) { return child<QLabel*>("nativeScanPreferences_"+id+"_Observed")->text(); }
    void stage(const QString& id,int value) { QComboBox* editor=draft(id); QVERIFY(editor->isEnabled()); const int position=editor->findData(value); QVERIFY(position>=0); editor->setFocus(); QTest::keyClick(editor,Qt::Key_Home); for (int step=0;step<position;++step) { QTest::keyClick(editor,Qt::Key_Down); } QTest::keyClick(editor,Qt::Key_Return); QCOMPARE(editor->currentData().toInt(),value); }
    void read() { replyReads=true; click("nativeScanPreferencesRead"); poll(3); }
    void publish(const QVariantMap& snapshot) { emit backend->extensionStatus("kenwood","scanPreferences",snapshot); }
    QVariantMap get() { QVariantMap result; QString error; constexpr quint64 request=0x50525141; const auto done=QObject::connect(backend,&IRadioBackend::extensionResult,&model,[&](quint64 id,const QVariant& value) { if (id==request) { result=value.toMap(); } }); const auto failed=QObject::connect(backend,&IRadioBackend::extensionError,&model,[&](quint64 id,const QString& value) { if (id==request) { error=value; } }); model.invokeBackendExtension("kenwood","scan.preferences.get",request); QObject::disconnect(done); QObject::disconnect(failed); if (!error.isEmpty()) { qWarning()<<error; } return result; }
    void exportQa(const QString& tag) { const QString directory=qEnvironmentVariable("AETHER_SCAN_PREFERENCES_QA_DIR"); if (directory.isEmpty()) { return; } QVERIFY(QDir().mkpath(directory)); QVERIFY(widget->grab().save(QDir(directory).filePath(tag+".png"))); QJsonArray labels; for (QLabel* label : widget->findChildren<QLabel*>()) { labels.append(QJsonObject{{"name",label->objectName()},{"text",label->text()},{"height",label->height()},{"hintHeight",label->sizeHint().height()}}); } QFile output(QDir(directory).filePath(tag+"-geometry.json")); QVERIFY(output.open(QIODevice::WriteOnly)); const QByteArray bytes=QJsonDocument(QJsonObject{{"width",widget->width()},{"height",widget->height()},{"labels",labels},{"backendStatus",QJsonObject::fromVariantMap(get())}}).toJson(); QCOMPARE(output.write(bytes),qint64(bytes.size())); }
};
}
class NativeScanPreferencesWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void unsupportedRowsStayReadableVisibleDimAndReasoned() {
        RadioModel model; NativeScanPreferencesWidget widget(model); widget.resize(1100,700); widget.show(); QCoreApplication::processEvents(); QStringList names{"nativeScanPreferencesRead","nativeScanPreferencesCancel"}; for (const QString& id : {QString("hold"),QString("resume")}) { names<<"nativeScanPreferences_"+id+"_Draft"<<"nativeScanPreferences_"+id+"_Apply"<<"nativeScanPreferences_"+id+"_Reset"; } for (const QString& name : names) { QWidget* control=widget.findChild<QWidget*>(name); QVERIFY(control); QVERIFY(control->isVisible()); QVERIFY(!control->isEnabled()); QVERIFY(control->accessibleDescription().contains("connect",Qt::CaseInsensitive) || control->accessibleDescription().contains("support",Qt::CaseInsensitive)); }
        QCOMPARE(widget.findChild<QLabel*>("nativeScanPreferences_hold_Observed")->text(),QString("Scan Hold: Unknown")); QCOMPARE(widget.findChild<QLabel*>("nativeScanPreferences_resume_Observed")->text(),QString("Scan Resume: Unknown"));
    }
    void cachedGetAndThreeQueriesNeverSelectBankOrStartScan() {
        Fixture f; f.wires.clear(); QVERIFY(!f.get().isEmpty()); QVERIFY(f.wires.isEmpty()); f.read(); QCOMPARE(f.wires,QList<QByteArray>({"MF;","EX00404;","EX00405;"})); QVERIFY(f.button("nativeScanPreferences_hold_Apply")->isEnabled()); f.exportQa("native-scan-preferences-known-read");
    }
    void bothRowsAndBothValuesUseActualApplyInFmAndNonFm() {
        Fixture f; f.read(); const QList<QPair<QString,QByteArray>> rows{{"hold","EX00404"},{"resume","EX00405"}};
        for (char mode : {'2','4'}) { f.mode=mode; f.observe(QByteArray("OM0")+mode); f.observe(QByteArray("SF000014074000")+mode);
            for (const auto& row : rows) { for (int value : {0,1}) { f.stage(row.first,value); QVERIFY(f.button("nativeScanPreferences_"+row.first+"_Apply")->isEnabled()); const QString old=f.observed(row.first); f.wires.clear(); f.click("nativeScanPreferences_"+row.first+"_Apply"); QCOMPARE(f.wires,QList<QByteArray>({row.second+" "+QByteArray::number(value).rightJustified(3,'0')+";"})); QCOMPARE(f.observed(row.first),old); f.values[row.first]=value; f.poll(2); QCOMPARE(f.wires.mid(1),QList<QByteArray>({"MF;",row.second+";"})); QVERIFY(f.child<QLabel*>("nativeScanPreferencesWriteStatus")->text().contains("observed_consistent")); } }
        }
        f.exportQa("native-scan-preferences-applied");
    }
    void bothResetButtonsObserveEitherValueWithoutAssumingDefault() {
        Fixture f; f.read(); const QList<QPair<QString,QByteArray>> rows{{"hold","EX00404"},{"resume","EX00405"}};
        for (const auto& row : rows) { for (int observed : {1,0}) { const QString old=f.observed(row.first); f.wires.clear(); f.click("nativeScanPreferences_"+row.first+"_Reset"); QCOMPARE(f.wires,QList<QByteArray>({row.second+"9;"})); QCOMPARE(f.observed(row.first),old); f.values[row.first]=observed; f.poll(2); QCOMPARE(f.wires.mid(1),QList<QByteArray>({"MF;",row.second+";"})); const QVariantMap last=f.get().value("lastWrite").toMap(); QCOMPARE(last.value("operation").toString(),QString("reset")); QCOMPARE(last.value("state").toString(),QString("sent_unverified")); QVERIFY(!f.child<QLabel*>("nativeScanPreferencesStatus")->text().contains("awaiting",Qt::CaseInsensitive)); } }
        f.exportQa("native-scan-preferences-radio-initial-observed");
    }
    void unrelatedSourceModeTfAndMemoryViewDoNotRetireDirtyPreference() {
        Fixture f; f.read(); f.stage("hold",1); f.observe("FR3"); f.observe("TS1"); f.observe("MV1"); f.observe("QR10"); f.observe("FA00014075000"); f.mode='4'; f.observe("OM04"); f.observe("SF0000140750004"); QCOMPARE(f.draft("hold")->currentData().toInt(),1); QVERIFY(f.button("nativeScanPreferences_hold_Apply")->isEnabled()); f.wires.clear(); f.click("nativeScanPreferences_hold_Apply"); QCOMPARE(f.wires,QList<QByteArray>({"EX00404 001;"}));
    }
    void unrelatedContextChangesDuringPendingPublicationDoNotCancelIntent() {
        Fixture f; f.read(); f.stage("resume",1); bool nested=false; f.onStatus=[&](const QVariantMap& status) { for (const QVariant& row : status.value("rows").toList()) { if (!nested && row.toMap().value("id")==QLatin1String("resume") && row.toMap().value("pending").toBool()) { nested=true; f.observe("FR3"); f.observe("TS1"); f.observe("MV1"); f.observe("QR10"); } } }; f.wires.clear(); f.click("nativeScanPreferences_resume_Apply"); QVERIFY(nested); QVERIFY(f.wires.contains("EX00405 001;")); f.onStatus={};
    }
    void bankChangeRetiresDraftAndResetUntilCurrentValuesAreRead() {
        Fixture f; f.read(); f.stage("hold",1); f.bank=1; f.observe("MF1"); QVERIFY(!f.button("nativeScanPreferences_hold_Apply")->isEnabled()); QVERIFY(!f.button("nativeScanPreferences_hold_Reset")->isEnabled()); f.read(); QCOMPARE(f.draft("hold")->currentData().toInt(),1); QVERIFY(!f.button("nativeScanPreferences_hold_Apply")->isEnabled()); QVERIFY(f.button("nativeScanPreferences_hold_Reset")->isEnabled()); f.stage("hold",1); QVERIFY(f.button("nativeScanPreferences_hold_Apply")->isEnabled());
    }
    void receiverWithdrawalDuringPendingPublicationCannotDispatch() {
        Fixture f; f.read(); f.stage("hold",1); bool nested=false; f.onStatus=[&](const QVariantMap& status) { for (const QVariant& row : status.value("rows").toList()) { if (!nested && row.toMap().value("id")==QLatin1String("hold") && row.toMap().value("pending").toBool()) { nested=true; f.observe("TX0"); } } }; f.wires.clear(); f.click("nativeScanPreferences_hold_Apply"); QVERIFY(nested); QVERIFY(!f.wires.contains("EX00404 001;")); QVERIFY(!f.button("nativeScanPreferences_hold_Reset")->isEnabled()); f.onStatus={};
    }
    void failedBankReadCannotQualifyNewRowAndReadStillWorksWithoutRx() {
        Fixture f; f.read(); f.replyReads=false; f.click("nativeScanPreferencesRead"); f.poll(); f.observe("MF9"); QVERIFY(!f.button("nativeScanPreferences_hold_Apply")->isEnabled()); QVERIFY(!f.button("nativeScanPreferences_hold_Reset")->isEnabled()); f.replyReads=true; f.poll(2); QVERIFY(!f.button("nativeScanPreferences_hold_Apply")->isEnabled());
        f.observe("TX0"); f.wires.clear(); f.click("nativeScanPreferencesRead"); f.poll(3); QCOMPARE(f.wires,QList<QByteArray>({"MF;","EX00404;","EX00405;"})); QVERIFY(!f.button("nativeScanPreferences_hold_Apply")->isEnabled());
    }
    void rowAuthorityCannotSurviveUnknownBank_data() {
        QTest::addColumn<bool>("disconnected");QTest::newRow("unknown-bank")<<false;QTest::newRow("disconnected-bank")<<true;
    }
    void rowAuthorityCannotSurviveUnknownBank() {
        QFETCH(bool,disconnected);Fixture f;f.read();const QVariantMap good=f.get();QVariantMap bad=good;bad["statusRevision"]="18446744073709551615";
        QVariantMap bank=bad.value("bank").toMap();if (disconnected) { bad["connected"]=false; }else { bank["known"]=false; }bad["bank"]=bank;
        QVariantList rows=bad.value("rows").toList();for (QVariant& value:rows) { QVariantMap row=value.toMap();row["editable"]=false;row["resettable"]=false;value=row; }bad["rows"]=rows;f.publish(bad);
        QVERIFY(f.observed("hold").contains("Unknown"));f.publish(good);QVERIFY(f.button("nativeScanPreferences_hold_Reset")->isEnabled());
    }
    void malformedHighBodyCannotPoisonRecoveryOrLoseDirtyChoice() {
        Fixture f; f.read(); f.stage("resume",1); for (const QString& revision : {QString("1000000"),QString("18446744073709551615")}) { QVariantMap bad=f.get(); bad["statusRevision"]=revision; QVariantMap bank=bad.value("bank").toMap(); bank["unexpected"]=true; bad["bank"]=bank; f.publish(bad); QVERIFY(!f.button("nativeScanPreferences_resume_Apply")->isEnabled()); QVERIFY(!f.button("nativeScanPreferences_resume_Reset")->isEnabled()); f.click("nativeScanPreferencesRead"); f.poll(3); QCOMPARE(f.draft("resume")->currentData().toInt(),1); QVERIFY(f.button("nativeScanPreferences_resume_Apply")->isEnabled()); }
        for (const QVariant& invalid : {QVariant(true),QVariant("01"),QVariant("18446744073709551616")}) { QVariantMap bad=f.get(); bad["statusRevision"]=invalid; f.publish(bad); QVERIFY(!f.button("nativeScanPreferences_resume_Reset")->isEnabled()); f.publish(f.get()); QVERIFY(f.button("nativeScanPreferences_resume_Reset")->isEnabled()); }
    }
    void forgedConsistentResetIsRejectedBeforeRevisionFloor() {
        Fixture f; f.read(); f.click("nativeScanPreferences_hold_Reset"); f.values["hold"]=1; f.poll(2); const QVariantMap good=f.get(); QVariantMap bad=good,write=good.value("lastWrite").toMap(); write["state"]="observed_consistent"; bad["lastWrite"]=write; bad["statusRevision"]="18446744073709551615"; f.publish(bad); QVERIFY(!f.button("nativeScanPreferences_hold_Reset")->isEnabled()); f.publish(good); QVERIFY(f.button("nativeScanPreferences_hold_Reset")->isEnabled());
    }
    void canceledVerificationDoesNotUndoOrPredictRadioInitialValue() {
        Fixture f; f.read(); f.replyReads=false; f.wires.clear(); f.click("nativeScanPreferences_resume_Reset"); QCOMPARE(f.wires,QList<QByteArray>({"EX004059;"})); QVERIFY(f.button("nativeScanPreferencesCancel")->isEnabled()); f.wires.clear(); f.click("nativeScanPreferencesCancel"); f.poll(4); QVERIFY(f.wires.isEmpty()); QVERIFY(f.child<QLabel*>("nativeScanPreferencesWriteStatus")->text().contains("indeterminate"));
    }
    void oldPushLateGetAndReplacementCannotRestoreStaleWriteAuthority() {
        Fixture f; f.read(); const QVariantMap old=f.get(); f.observe("TX0"); f.publish(old); QVERIFY(!f.button("nativeScanPreferences_hold_Reset")->isEnabled()); f.observe("RX"); f.widget.reset(); bool nested=false; f.onResult=[&](quint64,const QVariant& result) { if (!nested && result.toMap().contains("rows")) { nested=true; f.observe("TX0"); } }; f.makeWidget(); QVERIFY(nested); QVERIFY(!f.button("nativeScanPreferences_hold_Reset")->isEnabled()); f.onResult={};
        f.replyReads=false; f.install(); QVERIFY(f.child<QLabel*>("nativeScanPreferencesStatus")->text().isEmpty()); QVERIFY(!f.button("nativeScanPreferences_hold_Reset")->isEnabled());
    }
    void cleanDraftTracksObservedAndEditedDraftSurvivesMalformedPause() {
        Fixture f; f.read(); f.observe("EX00404 001"); QCOMPARE(f.draft("hold")->currentData().toInt(),1); f.stage("hold",0); f.observe("EX00404 001"); QVERIFY(f.button("nativeScanPreferences_hold_Apply")->isEnabled()); QVariantMap malformed=f.get(); QVariantList rows=malformed.value("rows").toList(); QVariantMap row=rows[0].toMap(); row["value"]=true; rows[0]=row; malformed["rows"]=rows; f.publish(malformed); QVERIFY(!f.button("nativeScanPreferences_hold_Apply")->isEnabled()); f.publish(f.get()); QCOMPARE(f.draft("hold")->currentData().toInt(),0); QVERIFY(f.button("nativeScanPreferences_hold_Apply")->isEnabled());
    }
    void comboModelAndAccessibilityReentryRespectNewestStatusAndLifetime() {
        Fixture f; f.read(); bool nested=false; const auto connection=QObject::connect(f.draft("hold")->model(),&QAbstractItemModel::rowsInserted,&f.model,[&] { if (!nested) { nested=true; f.observe("EX00404 001"); } }); f.observe("EX00404 000"); QVERIFY(nested); QCOMPARE(f.draft("hold")->currentData().toInt(),1); QObject::disconnect(connection);
        QAccessible::setActive(true); if (!QAccessible::isActive()) { QSKIP("Accessibility unavailable; combo model reentry proven, accessibility callback not proven."); } const auto previous=QAccessible::installUpdateHandler(accessibleUpdate); const auto cleanup=qScopeGuard([previous] { accessibilityCallback={}; QAccessible::installUpdateHandler(previous); }); nested=false;
        accessibilityCallback=[&](QAccessibleEvent* event) { if (!nested && event->object()==f.child<QLabel*>("nativeScanPreferences_hold_Observed")) { nested=true; f.observe("EX00404 000"); } }; f.observe("EX00404 001"); QVERIFY(nested); QCOMPARE(f.draft("hold")->currentData().toInt(),0);
        nested=false; accessibilityCallback=[&](QAccessibleEvent* event) { if (!nested && event->object()==f.child<QLabel*>("nativeScanPreferences_hold_Observed")) { nested=true; f.widget.reset(); } }; const QPointer<NativeScanPreferencesWidget> alive(f.widget.get()); f.observe("EX00404 001"); QVERIFY(nested); QVERIFY(!alive); accessibilityCallback={}; f.makeWidget(); f.draft("hold")->setFocus(); f.widget.reset(); QCoreApplication::processEvents();
    }
};
int main(int argc,char** argv) { TestSettingsProfile profile("native-scan-preferences"); if (!profile.isValid()) { return 1; } QApplication app(argc,argv); NativeScanPreferencesWidgetTest test; return QTest::qExec(&test,argc,argv); }
#include "native_scan_preferences_widget_test.moc"
