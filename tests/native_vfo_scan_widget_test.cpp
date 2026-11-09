// Actual widget -> RadioModel -> Kenwood injected transport; no radio/service.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeVfoScanWidget.h"
#include "models/RadioModel.h"
#include <QAbstractItemModel>
#include <QAccessible>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QMap>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QStringList>
#include <QtTest>
#include <functional>
#include <memory>
namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) { b.m_clock.start(); b.m_protocol.start("vfo-scan-fixture","fixture-only",false,0); b.m_protocol.receive("##CN1;",1); b.m_protocol.receive("##ID1;",2); b.ready(); }
    static void observe(KenwoodBackend& b,const QByteArray& bytes) { b.adopt(bytes); }
    static void poll(KenwoodBackend& b,qint64 now) { b.pollVfoScan(now); }
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
    QMap<QString,int> values{{"enabled",0},{"speed",5},{"tone",0},{"type",1}};
    char mode='2'; int toneType=0,toneIndex=0,ctcssIndex=0,tf=0; bool replyReads=false;
    qint64 now=0;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(quint64,const QVariant&)> onResult;
    RadioModel model;
    Backend* backend=nullptr;
    std::unique_ptr<NativeVfoScanWidget> widget;
    Fixture() { install(); makeWidget(); }
    void install() {
        auto owned=std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& bytes) { wires.append(bytes); if (replyReads) { const QByteArray response=reply(bytes); if (!response.isEmpty()) { observe(response); } } return true; },{}}); backend=owned.get(); model.setBackendForTest(std::move(owned),"kenwood");
        QObject::connect(backend,&IRadioBackend::extensionStatus,&model,[this](const QString&,const QString& kind,const QVariantMap& status) { if (kind=="vfoScan") { state=status; if (onStatus) { onStatus(status); } } }); QObject::connect(backend,&IRadioBackend::extensionResult,&model,[this](quint64 request,const QVariant& result) { if (onResult) { onResult(request,result); } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend); for (const QByteArray& message : {QByteArray("ID024"),QByteArray("FR0"),QByteArray("FT0"),QByteArray("FA00014074000"),QByteArray("OM02"),QByteArray("SF0000140740002"),QByteArray("TS0"),QByteArray("RX")}) { observe(message); }
        // Injected setup omits setupBackend lifecycle wiring. Supply only its
        // public notification, never a copied control or status relay.
        emit model.connectionStateChanged(true);
    }
    void makeWidget() { widget=std::make_unique<NativeVfoScanWidget>(model); widget->resize(1050,850); widget->show(); QCoreApplication::processEvents(); }
    QByteArray reply(const QByteArray& wire) const {
        if (wire=="MF;") { return "MF0"; } if (wire=="FR;") { return "FR0"; } if (wire=="MV;") { return "MV0"; } if (wire=="QR;") { return "QR00"; } if (wire=="OM0;") { return QByteArray("OM0")+mode; } if (wire=="TS;") { return "TS"+QByteArray::number(tf); }
        if (wire=="SF0;") { return QByteArray("SF000014074000")+mode; }
        if (wire=="SC0;") { return "SC0"+QByteArray::number(values["enabled"])+"0"; } if (wire=="SC1;") { return "SC1"+QByteArray::number(values["speed"]); } if (wire=="SC2;") { return "SC2"+QByteArray::number(values["tone"]); } if (wire=="SC3;") { return "SC3"+QByteArray::number(values["type"]); }
        if (wire=="TO;") { return "TO"+QByteArray::number(toneType); } if (wire=="TN;") { return "TN"+QByteArray::number(toneIndex).rightJustified(2,'0'); } if (wire=="CN;") { return "CN"+QByteArray::number(ctcssIndex).rightJustified(2,'0'); } return {};
    }
    void observe(const QByteArray& message) { kenwood::KenwoodBackendTestAccess::observe(*backend,message); }
    void poll(int count=1) { for (int index=0;index<count;++index) { kenwood::KenwoodBackendTestAccess::poll(*backend,now+=100); } }
    template<class T> T child(const QString& name) { return widget->findChild<T>(name); }
    QPushButton* button(const QString& name) { return child<QPushButton*>(name); }
    void click(const QString& name) { QTest::mouseClick(button(name),Qt::LeftButton); }
    QComboBox* draft(const QString& id) { return child<QComboBox*>("nativeVfoScan_"+id+"_Draft"); }
    QString observed(const QString& id) { return child<QLabel*>("nativeVfoScan_"+id+"_Observed")->text(); }
    void stage(const QString& id,int value) { QComboBox* editor=draft(id); const int position=editor->findData(value); QVERIFY(position>=0); QVERIFY(editor->model()->flags(editor->model()->index(position,0)) & Qt::ItemIsEnabled); editor->setFocus(); QTest::keyClick(editor,Qt::Key_Home); while (editor->currentData().toInt()!=value) { const int previous=editor->currentIndex(); QTest::keyClick(editor,Qt::Key_Down); QVERIFY(editor->currentIndex()!=previous); } QTest::keyClick(editor,Qt::Key_Return); QCOMPARE(editor->currentData().toInt(),value); }
    void read() { replyReads=true; click("nativeVfoScanRead"); poll(13); }
    void setMode(char value) { mode=value; observe(QByteArray("OM0")+mode); observe(QByteArray("SF000014074000")+mode); }
    void publish(const QVariantMap& snapshot) { emit backend->extensionStatus("kenwood","vfoScan",snapshot); }
    QVariantMap get() { QVariantMap result; QString error; constexpr quint64 request=0x53435141; const auto done=QObject::connect(backend,&IRadioBackend::extensionResult,&model,[&](quint64 id,const QVariant& value) { if (id==request) { result=value.toMap(); } }); const auto failed=QObject::connect(backend,&IRadioBackend::extensionError,&model,[&](quint64 id,const QString& value) { if (id==request) { error=value; } }); model.invokeBackendExtension("kenwood","scan.vfo.get",request); QObject::disconnect(done); QObject::disconnect(failed); if (!error.isEmpty()) { qWarning()<<error; } return result; }
    void exportQa(const QString& tag) { const QString directory=qEnvironmentVariable("AETHER_VFO_SCAN_QA_DIR"); if (directory.isEmpty()) { return; } QVERIFY(QDir().mkpath(directory)); QVERIFY(widget->grab().save(QDir(directory).filePath(tag+".png"))); QJsonArray labels; for (QLabel* label : widget->findChildren<QLabel*>()) { labels.append(QJsonObject{{"name",label->objectName()},{"text",label->text()},{"height",label->height()},{"hintHeight",label->sizeHint().height()}}); } QFile file(QDir(directory).filePath(tag+"-geometry.json")); QVERIFY(file.open(QIODevice::WriteOnly)); const QByteArray bytes=QJsonDocument(QJsonObject{{"width",widget->width()},{"height",widget->height()},{"labels",labels},{"backendStatus",QJsonObject::fromVariantMap(get())}}).toJson(); QCOMPARE(file.write(bytes),qint64(bytes.size())); }
};
}
class NativeVfoScanWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void unsupportedControlsAreVisibleWithReasons() {
        RadioModel model; NativeVfoScanWidget widget(model); widget.resize(1050,850); widget.show(); QCoreApplication::processEvents(); QStringList names{"nativeVfoScanRead","nativeVfoScanCancel"}; for (const QString& id : {QString("enabled"),QString("speed"),QString("tone"),QString("type")}) { names<<"nativeVfoScan_"+id+"_Draft"<<"nativeVfoScan_"+id+"_Apply"; } for (const QString& name : names) { QWidget* control=widget.findChild<QWidget*>(name); QVERIFY(control); QVERIFY(control->isVisible()); QVERIFY(!control->isEnabled()); QVERIFY(control->accessibleDescription().contains("connect",Qt::CaseInsensitive) || control->accessibleDescription().contains("support",Qt::CaseInsensitive)); }
    }
    void cachedGetIsPassiveAndReadHasThirteenJobsWithoutSetters() {
        Fixture f; f.wires.clear(); QVERIFY(!f.get().isEmpty()); QVERIFY(f.wires.isEmpty()); f.read(); qInfo()<<"VFO scan full-read actual wires"<<f.wires; QCOMPARE(f.wires,QList<QByteArray>({"MF;","FR;","SF0;","MV;","QR;","OM0;","TS;","SF0;","SF1;","TS;","SC0;","SC1;","SC2;","SC3;","TO;","TN;","CN;"})); const QVariantMap refresh=f.get().value("refresh").toMap(); QCOMPARE(refresh.value("total").toInt(),13); QCOMPARE(refresh.value("observed").toInt(),13); QCOMPARE(refresh.value("failed").toInt(),0); f.exportQa("native-vfo-scan-known-read");
    }
    void EnabledAndAllNineSpeedChoicesUseActualApplyReadback() {
        Fixture f; f.read(); for (const QString& id : {QString("enabled"),QString("speed")}) { const int low=id=="speed" ? 1 : 0,high=id=="speed" ? 9 : 1; const QByteArray prefix=id=="speed" ? "SC1" : "SC0"; for (int value=low;value<=high;++value) { f.stage(id,value); QVERIFY(f.button("nativeVfoScan_"+id+"_Apply")->isEnabled()); const QString old=f.observed(id); f.wires.clear(); f.click("nativeVfoScan_"+id+"_Apply"); QCOMPARE(f.wires,QList<QByteArray>({prefix+QByteArray::number(value)+";"})); QCOMPARE(f.observed(id),old); f.values[id]=value; f.poll(); QCOMPARE(f.wires.mid(1),QList<QByteArray>({prefix+";"})); QVERIFY(f.button("nativeVfoScan_"+id+"_Apply")->isEnabled()); } }
    }
    void ToneChoicesHaveFourPostReadsAndCompanionsAreObservedNotRestored() {
        Fixture f; f.setMode('4'); f.read(); for (int value : {0,1,2}) { f.stage("tone",value); QVERIFY(f.button("nativeVfoScan_tone_Apply")->isEnabled()); const QString old=f.observed("tone"); f.wires.clear(); f.click("nativeVfoScan_tone_Apply"); QCOMPARE(f.wires,QList<QByteArray>({"SC2"+QByteArray::number(value)+";"})); QCOMPARE(f.observed("tone"),old); f.values["tone"]=value; f.toneType=value; f.toneIndex=50; f.ctcssIndex=49; f.poll(4); QCOMPARE(f.wires.mid(1),QList<QByteArray>({"SC2;","TO;","TN;","CN;"})); const QVariantMap tones=f.get().value("toneObservations").toMap(); QCOMPARE(tones.value("type").toMap().value("value").toInt(),value); QCOMPARE(tones.value("index").toMap().value("value").toInt(),50); QCOMPARE(tones.value("ctcssIndex").toMap().value("value").toInt(),49); }
        QVERIFY(f.child<QLabel*>("nativeVfoScanTones")->text().contains("not Hz")); f.exportQa("native-vfo-scan-tone-companions");
    }
    void ProgramObservationRemainsVisibleDimAndVfoSelectionIsExplicit() {
        Fixture f; f.values["type"]=0; f.read(); QComboBox* choice=f.draft("type"); const int program=choice->findData(0); QVERIFY(program>=0); QVERIFY(!(choice->model()->flags(choice->model()->index(program,0)) & Qt::ItemIsEnabled)); QVERIFY(!choice->itemData(program,Qt::ToolTipRole).toString().isEmpty()); QCOMPARE(choice->currentData().toInt(),0); f.exportQa("native-vfo-scan-program-unavailable"); f.stage("type",1); f.wires.clear(); f.click("nativeVfoScan_type_Apply"); QCOMPARE(f.wires,QList<QByteArray>({"SC31;"})); f.values["type"]=1; f.poll(); QCOMPARE(f.wires,QList<QByteArray>({"SC31;","SC3;"}));
    }
    void FmSpeedAndNonFmToneStayUnavailableWhileReadsStillWork() {
        Fixture f; f.read(); QVERIFY(!f.button("nativeVfoScan_tone_Apply")->isEnabled()); f.setMode('4'); QVERIFY(!f.button("nativeVfoScan_speed_Apply")->isEnabled()); f.observe("SC15"); QVERIFY(!f.button("nativeVfoScan_speed_Apply")->isEnabled()); f.observe("SC20"); f.stage("tone",1); QVERIFY(f.button("nativeVfoScan_tone_Apply")->isEnabled());
    }
    void TfSetOnAllowsTypeButDoesNotPretendReceiveModeQualified() {
        Fixture f; f.read(); f.tf=1; f.observe("TS1"); f.observe("SC31"); QVERIFY(!f.button("nativeVfoScan_speed_Apply")->isEnabled()); QVERIFY(!f.button("nativeVfoScan_enabled_Apply")->isEnabled()); f.stage("type",1); QVERIFY(f.button("nativeVfoScan_type_Apply")->isEnabled()); f.wires.clear(); f.click("nativeVfoScan_type_Apply"); QCOMPARE(f.wires,QList<QByteArray>({"SC31;"})); QVERIFY(f.child<QLabel*>("nativeVfoScanSource")->text().contains("TF-SET On"));
    }
    void PhysicalBAndUnknownBankDoNotAcquireFtOrFrequencyRequirements() {
        Fixture f; f.observe("FR1"); f.observe("MV0"); f.observe("QR00"); f.observe("SC31"); const QVariantMap source=f.get().value("source").toMap(); QCOMPARE(source.value("physicalSource").toString(),QString("B")); QVERIFY(!source.value("bankKnown").toBool()); f.stage("type",1); QVERIFY(f.button("nativeVfoScan_type_Apply")->isEnabled()); f.wires.clear(); f.click("nativeVfoScan_type_Apply"); QCOMPARE(f.wires,QList<QByteArray>({"SC31;"}));
    }
    void FrequencyStepsDuringPendingScanDoNotRetireProducer() {
        Fixture f; f.read(); f.stage("speed",7); bool nested=false; f.onStatus=[&](const QVariantMap& status) { for (const QVariant& raw : status.value("rows").toList()) { if (!nested && raw.toMap().value("id")==QLatin1String("speed") && raw.toMap().value("pending").toBool()) { nested=true; f.observe("FA00014075000"); f.observe("FA00014076000"); } } }; f.wires.clear(); f.click("nativeVfoScan_speed_Apply"); QVERIFY(nested); QCOMPARE(f.wires,QList<QByteArray>({"SC17;"})); f.onStatus={}; f.values["speed"]=7; f.poll(); QVERIFY(f.child<QLabel*>("nativeVfoScanWriteStatus")->text().contains("observed_consistent"));
    }
    void PendingReceiveWithdrawalCannotSendAndCancelNeverStopsRadio() {
        Fixture f; f.read(); f.stage("speed",7); bool nested=false; f.onStatus=[&](const QVariantMap& status) { for (const QVariant& raw : status.value("rows").toList()) { if (!nested && raw.toMap().value("id")==QLatin1String("speed") && raw.toMap().value("pending").toBool()) { nested=true; f.observe("TX0"); } } }; f.wires.clear(); f.click("nativeVfoScan_speed_Apply"); QVERIFY(nested); QVERIFY(!f.wires.contains("SC17;")); QVERIFY(!f.button("nativeVfoScan_speed_Apply")->isEnabled()); f.onStatus={}; f.observe("RX"); f.replyReads=false; f.click("nativeVfoScanRead"); QVERIFY(f.button("nativeVfoScanCancel")->isEnabled()); f.wires.clear(); f.click("nativeVfoScanCancel"); f.poll(15); QVERIFY(f.wires.isEmpty());
    }
    void malformedHighDescriptorCannotPoisonFloorOrLoseDraft() {
        Fixture f; f.read(); f.stage("speed",7); for (const QString& revision : {QString("1000000"),QString("18446744073709551615")}) { QVariantMap bad=f.get(); bad["statusRevision"]=revision; QVariantMap source=bad.value("source").toMap(); source["unexpected"]=true; bad["source"]=source; f.publish(bad); QVERIFY(!f.button("nativeVfoScan_speed_Apply")->isEnabled()); f.click("nativeVfoScanRead"); f.poll(13); QCOMPARE(f.draft("speed")->currentData().toInt(),7); QVERIFY(f.button("nativeVfoScan_speed_Apply")->isEnabled()); }
        for (const QVariant& invalid : {QVariant(true),QVariant("01"),QVariant("18446744073709551616")}) { QVariantMap bad=f.get(); bad["statusRevision"]=invalid; f.publish(bad); QVERIFY(!f.button("nativeVfoScan_speed_Apply")->isEnabled()); f.publish(f.get()); QVERIFY(f.button("nativeVfoScan_speed_Apply")->isEnabled()); }
    }
    void invalidToneCompanionCannotBecomeHzOrKnownDefault() {
        Fixture f; f.setMode('4'); f.read(); for (const QVariant& invalid : {QVariant(99),QVariant(true),QVariant(17.0),QVariant("17")}) { QVariantMap bad=f.get(); QVariantMap tones=bad.value("toneObservations").toMap(),index=tones.value("index").toMap(); index["value"]=invalid; tones["index"]=index; bad["toneObservations"]=tones; f.publish(bad); QVERIFY(!f.button("nativeVfoScan_tone_Apply")->isEnabled()); f.publish(f.get()); QVERIFY(f.button("nativeVfoScan_tone_Apply")->isEnabled()); }
    }
    void olderPendingPushAndLateGetCannotReviveWithdrawnContext() {
        Fixture f; f.read(); const QVariantMap old=f.get(); f.observe("TX0"); f.publish(old); QVERIFY(!f.button("nativeVfoScan_speed_Apply")->isEnabled()); f.observe("RX"); f.widget.reset(); bool nested=false; f.onResult=[&](quint64,const QVariant& result) { if (!nested && result.toMap().contains("rows")) { nested=true; f.observe("TX0"); } }; f.makeWidget(); QVERIFY(nested); QVERIFY(!f.button("nativeVfoScan_speed_Apply")->isEnabled()); f.onResult={};
    }
    void newBindingClearsOldHandoffAndNeverReplaysScan() {
        Fixture f; f.read(); f.stage("speed",7); f.click("nativeVfoScan_speed_Apply"); QVERIFY(!f.child<QLabel*>("nativeVfoScanStatus")->text().isEmpty()); f.replyReads=false; f.wires.clear(); f.install(); QVERIFY(f.child<QLabel*>("nativeVfoScanStatus")->text().isEmpty()); QVERIFY(!f.button("nativeVfoScan_speed_Apply")->isEnabled()); for (const QByteArray& wire : f.wires) { QVERIFY(!wire.startsWith("SC0") && !wire.startsWith("SC1") && !wire.startsWith("SC2") && !wire.startsWith("SC3")); }
    }
    void unsupportedRowsKeepReadableNamesWithUnknownValues() {
        RadioModel model;NativeVfoScanWidget widget(model);
        const QMap<QString,QString> names{{"enabled","VFO scan"},{"speed","Scan speed index"},{"tone","Tone scan"},{"type","Scan selection"}};
        for (auto it=names.cbegin();it!=names.cend();++it) { QCOMPARE(widget.findChild<QLabel*>("nativeVfoScan_"+it.key()+"_Observed")->text(),it.value()+": Unknown"); }
    }
    void completedReadbackDoesNotKeepAwaitingStatus() {
        Fixture f;f.read();f.stage("speed",6);f.click("nativeVfoScan_speed_Apply");f.values["speed"]=6;f.poll();
        QCOMPARE(f.get().value("lastWrite").toMap().value("state").toString(),QString("observed_consistent"));
        QVERIFY(!f.child<QLabel*>("nativeVfoScanStatus")->text().contains("awaiting radio observations",Qt::CaseInsensitive));
    }
    void comboModelAndAccessibilityReentryCannotResumeStaleRendering() {
        Fixture f; f.read(); bool nested=false; const auto connection=QObject::connect(f.draft("speed")->model(),&QAbstractItemModel::rowsInserted,&f.model,[&] { if (!nested) { nested=true; f.observe("SC17"); } }); f.observe("SC16"); QVERIFY(nested); QCOMPARE(f.draft("speed")->currentData().toInt(),7); QObject::disconnect(connection);
        QAccessible::setActive(true); if (!QAccessible::isActive()) { QSKIP("Accessibility unavailable; combo model reentry proven, no accessibility callback proof."); } const auto previous=QAccessible::installUpdateHandler(accessibleUpdate); const auto cleanup=qScopeGuard([previous] { accessibilityCallback={}; QAccessible::installUpdateHandler(previous); }); nested=false;
        accessibilityCallback=[&](QAccessibleEvent* event) { if (!nested && event->object()==f.child<QLabel*>("nativeVfoScan_speed_Observed")) { nested=true; f.observe("SC18"); } }; f.observe("SC16"); QVERIFY(nested); QCOMPARE(f.draft("speed")->currentData().toInt(),8);
        nested=false; accessibilityCallback=[&](QAccessibleEvent* event) { if (!nested && event->object()==f.child<QLabel*>("nativeVfoScan_speed_Observed")) { nested=true; f.widget.reset(); } }; const QPointer<NativeVfoScanWidget> alive(f.widget.get()); f.observe("SC16"); QVERIFY(nested); QVERIFY(!alive); accessibilityCallback={}; f.makeWidget(); f.draft("speed")->setFocus(); f.widget.reset(); QCoreApplication::processEvents();
    }
};
int main(int argc,char** argv) { TestSettingsProfile profile("native-vfo-scan"); if (!profile.isValid()) { return 1; } QApplication app(argc,argv); NativeVfoScanWidgetTest test; return QTest::qExec(&test,argc,argv); }
#include "native_vfo_scan_widget_test.moc"
