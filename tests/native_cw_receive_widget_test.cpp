// Production widget -> RadioModel -> injected Kenwood transport; no radio peer.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeCwReceiveWidget.h"
#include "models/RadioModel.h"
#include <QAbstractItemModel>
#include <QAccessible>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QScopeGuard>
#include <QSpinBox>
#include <QStringList>
#include <QtTest>
#include <functional>
#include <memory>
namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) { b.m_clock.start(); b.m_protocol.start("cw-fixture","fixture-only",false,0); b.m_protocol.receive("##CN1;",1); b.m_protocol.receive("##ID1;",2); b.ready(); }
    static void observe(KenwoodBackend& b,const QByteArray& message) { b.adopt(message); }
    static void poll(KenwoodBackend& b,qint64 now) { b.pollCwReceive(now); }
};
}
namespace {
using namespace AetherSDR;
using Backend=kenwood::KenwoodBackend;
std::function<void(QAccessibleEvent*)> accessibilityCallback;
void accessibleUpdate(QAccessibleEvent* event) { if (accessibilityCallback) { accessibilityCallback(event); } }
struct Fixture {
    QList<QByteArray> wires;
    QVariantMap state,textState;
    QMap<QString,int> values{{"screen",0},{"threshold",10},{"filter",1},{"enabled",1}};
    bool replyReads=false;
    std::function<void(const QString&,const QVariantMap&)> onStatus;
    std::function<void(quint64,const QVariant&)> onResult;
    RadioModel model;
    Backend* backend=nullptr;
    std::unique_ptr<NativeCwReceiveWidget> widget;
    qint64 now=0;
    Fixture() { install(); makeWidget(); }
    void install() {
        auto owned=std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& bytes) { wires.append(bytes); if (replyReads) { const QByteArray response=reply(bytes); if (!response.isEmpty()) { observe(response); } } return true; },{}}); backend=owned.get(); model.setBackendForTest(std::move(owned),"kenwood");
        QObject::connect(backend,&IRadioBackend::extensionStatus,&model,[this](const QString&,const QString& kind,const QVariantMap& status) { if (kind=="cwReceive") { state=status; } else if (kind=="cwReceiveText") { textState=status; } if (onStatus) { onStatus(kind,status); } });
        QObject::connect(backend,&IRadioBackend::extensionResult,&model,[this](quint64 request,const QVariant& result) { if (onResult) { onResult(request,result); } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"),QByteArray("FR0"),QByteArray("FT0"),QByteArray("FA00014074000"),QByteArray("OM03"),QByteArray("SF0000140740003"),QByteArray("TS0"),QByteArray("RX")}) { observe(message); }
        // setBackendForTest omits setupBackend lifecycle. Supply its public
        // notification only, never a copied status/control relay.
        emit model.connectionStateChanged(true);
    }
    void makeWidget() { widget=std::make_unique<NativeCwReceiveWidget>(model); widget->resize(1000,850); widget->show(); QCoreApplication::processEvents(); }
    QByteArray reply(const QByteArray& wire) const {
        if (wire=="CA;") { return "CA0"; } if (wire=="AI;") { return "AI2"; } if (wire=="DS0;") { return "DS00"; }
        if (wire=="CD0;") { return "CD0"+QByteArray::number(values["screen"]); }
        if (wire=="CD1;") { return "CD1"+QByteArray::number(values["threshold"]).rightJustified(3,'0'); }
        if (wire=="CD3;") { return "CD3"+QByteArray::number(values["filter"]); }
        if (wire=="CD5;") { return "CD5"+QByteArray::number(values["enabled"]); } return {};
    }
    void observe(const QByteArray& message) { kenwood::KenwoodBackendTestAccess::observe(*backend,message); }
    void poll(int count=1) { for (int index=0;index<count;++index) { kenwood::KenwoodBackendTestAccess::poll(*backend,now+=100); } }
    template<class T> T child(const QString& name) { return widget->findChild<T>(name); }
    QPushButton* button(const QString& name) { return child<QPushButton*>(name); }
    void click(const QString& name) { QTest::mouseClick(button(name),Qt::LeftButton); }
    QComboBox* choice(const QString& id) { return child<QComboBox*>("nativeCwReceive_"+id+"_Draft"); }
    QSpinBox* threshold() { return child<QSpinBox*>("nativeCwReceive_threshold_Draft"); }
    QString observed(const QString& id) { return child<QLabel*>("nativeCwReceive_"+id+"_Observed")->text(); }
    QString history() { return child<QPlainTextEdit*>("nativeCwReceiveTranscript")->toPlainText(); }
    void stage(const QString& id,int value) { if (id=="threshold") { threshold()->setFocus(); threshold()->selectAll(); QTest::keyClicks(threshold(),QString::number(value)); QTest::keyClick(threshold(),Qt::Key_Tab); QCOMPARE(threshold()->value(),value); return; } QComboBox* editor=choice(id); const int position=editor->findData(value); QVERIFY(position>=0); editor->setFocus(); QTest::keyClick(editor,Qt::Key_Home); for (int index=0;index<position;++index) { QTest::keyClick(editor,Qt::Key_Down); } QTest::keyClick(editor,Qt::Key_Return); QCOMPARE(editor->currentData().toInt(),value); }
    void read() { replyReads=true; click("nativeCwReceiveRead"); poll(7); }
    void publish(const QVariantMap& snapshot) { emit backend->extensionStatus("kenwood","cwReceive",snapshot); }
    void publishText(const QVariantMap& snapshot) { emit backend->extensionStatus("kenwood","cwReceiveText",snapshot); }
    QVariantMap get() { QVariantMap result; QString error; constexpr quint64 request=0x43575141; const auto done=QObject::connect(backend,&IRadioBackend::extensionResult,&model,[&](quint64 id,const QVariant& value) { if (id==request) { result=value.toMap(); } }); const auto failed=QObject::connect(backend,&IRadioBackend::extensionError,&model,[&](quint64 id,const QString& value) { if (id==request) { error=value; } }); model.invokeBackendExtension("kenwood","cw.receive.get",request); QObject::disconnect(done); QObject::disconnect(failed); if (!error.isEmpty()) { qWarning()<<error; } return result; }
    void exportQa(const QString& name) { const QString directory=qEnvironmentVariable("AETHER_CW_RECEIVE_QA_DIR"); if (directory.isEmpty()) { return; } QVERIFY(QDir().mkpath(directory)); QVERIFY(widget->grab().save(QDir(directory).filePath(name+".png"))); QJsonArray labels; for (QLabel* label : widget->findChildren<QLabel*>()) { labels.append(QJsonObject{{"name",label->objectName()},{"text",label->text()},{"height",label->height()},{"hintHeight",label->sizeHint().height()}}); } QFile output(QDir(directory).filePath(name+"-geometry.json")); QVERIFY(output.open(QIODevice::WriteOnly)); const QByteArray bytes=QJsonDocument(QJsonObject{{"width",widget->width()},{"height",widget->height()},{"labels",labels},{"backendStatus",QJsonObject::fromVariantMap(get())}}).toJson(); QCOMPARE(output.write(bytes),qint64(bytes.size())); }
};
}
class NativeCwReceiveWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void autoZeroObservationUsesRealReadRouteWithoutActions() {
        Fixture f; QLabel* label=f.child<QLabel*>("nativeCwReceiveAutoZero"); QVERIFY(label); QVERIFY(label->isVisible());
        QVERIFY(label->text().contains("Unknown")); f.wires.clear(); f.read();
        QCOMPARE(f.wires.count("CA;"),1); QVERIFY(label->text().contains("Inactive"));
        f.observe("CA1"); f.poll(); QVERIFY(label->text().contains("Active"));
        f.observe("CA2"); f.poll(); QVERIFY(label->text().contains("Unknown; last observed Active"));
        QVariantMap malformed=f.get(); malformed["statusRevision"]=QString::number(malformed.value("statusRevision").toString().toULongLong()+1);
        QVariantMap activity=malformed.value("autoZero").toMap(); activity["active"]=1; malformed["autoZero"]=activity; f.publish(malformed);
        QVERIFY(label->text().contains("Unknown"));
        f.widget.reset();
        for (const QByteArray& wire : f.wires) { QVERIFY(wire!="CA0;" && wire!="CA1;"); }
    }
    void unsupportedControlsStayVisibleAndReasoned() {
        RadioModel model; NativeCwReceiveWidget widget(model); widget.resize(1000,850); widget.show(); QCoreApplication::processEvents();
        QStringList names{"nativeCwReceiveRead","nativeCwReceiveCancel","nativeCwReceiveClear"}; for (const QString& id : {QString("screen"),QString("threshold"),QString("filter"),QString("enabled")}) { names<<"nativeCwReceive_"+id+"_Draft"<<"nativeCwReceive_"+id+"_Apply"; }
        for (const QString& name : names) { QWidget* control=widget.findChild<QWidget*>(name); QVERIFY(control); QVERIFY(control->isVisible()); QVERIFY(!control->isEnabled()); QVERIFY(control->accessibleDescription().contains("connect",Qt::CaseInsensitive) || control->accessibleDescription().contains("support",Qt::CaseInsensitive)); }
        QVERIFY(widget.findChild<QPlainTextEdit*>("nativeCwReceiveTranscript")->isReadOnly());
    }
    void cachedGetIsPassiveAndExplicitReadIsExactlySevenQueries() {
        Fixture f; f.wires.clear(); const QVariantMap initial=f.get(); QVERIFY(!initial.isEmpty()); QVERIFY(f.wires.isEmpty()); f.read(); QCOMPARE(f.wires,QList<QByteArray>({"AI;","CD0;","CD1;","CD3;","CD5;","DS0;","CA;"})); QVERIFY(f.button("nativeCwReceive_threshold_Apply")->isEnabled()); f.exportQa("native-cw-known-read");
    }
    void everyScalarValueHasActualApplyAndMatchingReadback() {
        Fixture f; f.read();
        const QList<QPair<QString,QByteArray>> rows{{"screen","CD0"},{"threshold","CD1"},{"filter","CD3"},{"enabled","CD5"}};
        for (const auto& entry : rows) { const int minimum=entry.first=="threshold" ? 1 : 0,maximum=entry.first=="threshold" ? 30 : entry.first=="filter" ? 2 : 1;
            for (int value=minimum;value<=maximum;++value) { f.stage(entry.first,value); QVERIFY2(f.button("nativeCwReceive_"+entry.first+"_Apply")->isEnabled(),qPrintable(entry.first+QString::number(value))); const QString old=f.observed(entry.first); f.wires.clear(); f.click("nativeCwReceive_"+entry.first+"_Apply"); const QByteArray encoded=entry.first=="threshold" ? QByteArray::number(value).rightJustified(3,'0') : QByteArray::number(value); QCOMPARE(f.wires,QList<QByteArray>({entry.second+encoded+";"})); QCOMPARE(f.observed(entry.first),old); QVERIFY(!f.button("nativeCwReceive_"+entry.first+"_Apply")->isEnabled()); f.values[entry.first]=value; f.poll(); QCOMPARE(f.wires,QList<QByteArray>({entry.second+encoded+";",entry.second+";"})); QVERIFY(f.button("nativeCwReceive_"+entry.first+"_Apply")->isEnabled()); }
        }
    }
    void nativeCharactersProsignsAndLocalClearNeverSendText() {
        Fixture f; f.read(); f.wires.clear(); f.observe("CD2C"); f.observe("CD2Q"); f.observe("CD2 "); f.observe("CD2_"); for (const QByteArray& token : {QByteArray("BT"),QByteArray("AR"),QByteArray("AS"),QByteArray("HH"),QByteArray("SK"),QByteArray("KN"),QByteArray("BK"),QByteArray("SN")}) { f.observe("CD2"+token); }
        QCOMPARE(f.history(),QString("CQ _<BT><AR><AS><HH><SK><KN><BK><SN>")); QVERIFY(f.wires.isEmpty()); f.exportQa("native-cw-received-text"); const QVariantMap oldText=f.textState;
        f.click("nativeCwReceiveClear"); QVERIFY(f.history().isEmpty()); QVERIFY(f.wires.isEmpty()); f.publishText(oldText); QVERIFY(f.history().isEmpty()); f.exportQa("native-cw-local-clear");
    }
    void unrelatedModeDoesNotBlockThresholdFilterButStopsPositiveDecoder() {
        Fixture f; f.read(); f.observe("OM02"); f.observe("SF0000140740002"); f.observe("CD00"); f.observe("CD1010"); f.observe("CD31"); f.observe("CD51");
        f.stage("threshold",12); QVERIFY(f.button("nativeCwReceive_threshold_Apply")->isEnabled()); f.wires.clear(); f.click("nativeCwReceive_threshold_Apply"); QCOMPARE(f.wires,QList<QByteArray>({"CD1012;"})); f.values["threshold"]=12; f.poll();
        f.stage("filter",2); QVERIFY(f.button("nativeCwReceive_filter_Apply")->isEnabled()); f.wires.clear(); f.click("nativeCwReceive_filter_Apply"); QCOMPARE(f.wires,QList<QByteArray>({"CD32;"})); f.values["filter"]=2; f.poll();
        f.stage("enabled",0); QVERIFY(f.button("nativeCwReceive_enabled_Apply")->isEnabled()); f.wires.clear(); f.click("nativeCwReceive_enabled_Apply"); QCOMPARE(f.wires,QList<QByteArray>({"CD50;"}));
    }
    void streamWithdrawalsKeepHistoricalTextButNeverAppendStaleEvents() {
        Fixture f; f.read(); f.observe("CD2A"); QCOMPARE(f.history(),QString("A")); f.observe("AI0"); f.observe("CD2B"); QCOMPARE(f.history(),QString("A")); f.observe("AI2"); f.observe("DS01"); f.observe("CD2C"); QCOMPARE(f.history(),QString("A")); f.observe("DS00"); f.observe("TX0"); f.observe("CD2D"); QCOMPARE(f.history(),QString("A")); f.observe("RX"); f.observe("CD2E"); QCOMPARE(f.history(),QString("AE"));
    }
    void malformedAndOversizeNativeEventsCannotBecomeRecognizedText() {
        Fixture f; f.read(); for (const QByteArray& payload : {QByteArray("ZZ"),QByteArray("ABC"),QByteArray(1,'\x01'),QByteArray::fromHex("c3a9")}) { f.observe("CD2"+payload); } QVERIFY(f.history().isEmpty()); QVERIFY(f.get().value("transcript").toMap().value("invalid").toInt()>=4);
        for (int index=0;index<1030;++index) { f.observe("CD2A"); } const QVariantMap transcript=f.get().value("transcript").toMap(); QCOMPARE(transcript.value("events").toList().size(),1024); QVERIFY(transcript.value("dropped").toInt()>=6); QCOMPARE(f.history().size(),1024);
    }
    void retainedBackendDisconnectPreservesHistoryAndAllowsLocalClear() {
        Fixture f; f.read(); f.observe("CD2A"); QCOMPARE(f.history(),QString("A")); const QPointer<Backend> retained(f.backend);
        f.model.disconnectFromRadio(); QVERIFY(retained); QCOMPARE(f.model.backend(),static_cast<IRadioBackend*>(retained.data())); QVERIFY(!f.model.isConnected());
        // As for connection in this injected fixture, supply only the public
        // lifecycle notification omitted by setBackendForTest's setup bypass.
        emit f.model.connectionStateChanged(false);
        QCOMPARE(f.history(),QString("A")); QVERIFY(f.button("nativeCwReceiveClear")->isEnabled()); f.wires.clear(); f.click("nativeCwReceiveClear"); QVERIFY(f.history().isEmpty()); QVERIFY(f.wires.isEmpty()); QVERIFY(!f.button("nativeCwReceiveRead")->isEnabled());
    }
    void nativePunctuationIsLiteralAndUnknownAiDoesNotBecomeOn() {
        Fixture f; f.wires.clear(); f.observe("CD2<"); f.observe("CD2>"); f.observe("CD2&"); QCOMPARE(f.history(),QString("<>&")); QVERIFY(f.wires.isEmpty()); const QVariantMap initial=f.get().value("stream").toMap(); QVERIFY(!initial.value("aiKnown").toBool()); QVERIFY(!initial.value("available").toBool());
        f.observe("AI2"); f.observe("CD2A"); QCOMPARE(f.history(),QString("<>&A")); f.observe("AI1"); QVERIFY(!f.get().value("stream").toMap().value("available").toBool()); QVERIFY(!f.get().value("stream").toMap().value("aiKnown").toBool());
    }
    void oldGetAndTextPushCannotOverwriteNewTextOrClear() {
        Fixture f; f.read(); f.observe("CD2A"); const QVariantMap old=f.get(); const QVariantMap oldText=f.textState; f.observe("CD2B"); f.publish(old); QCOMPARE(f.history(),QString("AB"));
        f.click("nativeCwReceiveClear"); f.publish(old); f.publishText(oldText); QVERIFY(f.history().isEmpty());
        f.widget.reset(); bool nested=false; f.onResult=[&](quint64,const QVariant& result) { if (!nested && result.toMap().contains("controls")) { nested=true; f.observe("CD2C"); } }; f.makeWidget(); QVERIFY(nested); QCOMPARE(f.history(),QString("C")); f.onResult={};
    }
    void malformedHighControlAndTextCannotPoisonRecoveryOrDraft() {
        Fixture f; f.read(); f.stage("threshold",17); const QVariantMap good=f.get();
        for (const QString& high : {QString("1000000"),QString("18446744073709551615")}) {
            QVariantMap bad=f.get(); bad["statusRevision"]=high; QVariantList controls=bad.value("controls").toList(); QVariantMap row=controls[0].toMap(); row["unknown"]=true; controls[0]=row; bad["controls"]=controls; f.publish(bad); QVERIFY(!f.button("nativeCwReceive_threshold_Apply")->isEnabled()); QCOMPARE(f.threshold()->value(),17);
            f.click("nativeCwReceiveRead"); f.poll(7); QVERIFY(f.button("nativeCwReceive_threshold_Apply")->isEnabled()); QCOMPARE(f.threshold()->value(),17);
            f.observe("CD2A"); QVariantMap badText=f.textState; badText["statusRevision"]=high; QVariantMap stream=badText.value("stream").toMap(),transcript=badText.value("transcript").toMap(); stream["revision"]=high; transcript["revision"]=high; transcript["unexpected"]=true; badText["stream"]=stream; badText["transcript"]=transcript; f.publishText(badText); QVERIFY(f.history().isEmpty()); f.publish(f.get()); QVERIFY(!f.history().isEmpty());
        }
        for (const QVariant& revision : {QVariant(true),QVariant("01"),QVariant("18446744073709551616")}) { QVariantMap bad=good; bad["statusRevision"]=revision; f.publish(bad); QVERIFY(!f.button("nativeCwReceive_threshold_Apply")->isEnabled()); f.publish(f.get()); QVERIFY(f.button("nativeCwReceive_threshold_Apply")->isEnabled()); }
    }
    void nativeContextWithdrawalDuringPendingPublicationCannotWrite() {
        Fixture f; f.read(); f.stage("threshold",17); bool nested=false;
        f.onStatus=[&](const QString& kind,const QVariantMap& snapshot) { if (nested || kind!="cwReceive") { return; } for (const QVariant& raw : snapshot.value("controls").toList()) { if (raw.toMap().value("id")==QLatin1String("threshold") && raw.toMap().value("pending").toBool()) { nested=true; f.observe("TX0"); break; } } };
        f.wires.clear(); f.click("nativeCwReceive_threshold_Apply"); QVERIFY(nested); QVERIFY(!f.wires.contains("CD1017;")); QCOMPARE(f.threshold()->value(),17); QVERIFY(!f.button("nativeCwReceive_threshold_Apply")->isEnabled()); f.onStatus={};
        f.observe("CD2A"); f.wires.clear(); f.click("nativeCwReceiveClear"); QVERIFY(f.wires.isEmpty()); QVERIFY(f.history().isEmpty());
    }
    void transcriptCallbackClearAndRowModelReentryKeepNewestState() {
        Fixture f; f.read(); bool nested=false; const auto connection=QObject::connect(f.choice("filter")->model(),&QAbstractItemModel::rowsInserted,&f.model,[&] { if (!nested) { nested=true; f.observe("CD32"); } }); f.observe("CD31"); QVERIFY(nested); QCOMPARE(f.choice("filter")->currentData().toInt(),2); QObject::disconnect(connection);
        nested=false; const auto clear=QObject::connect(f.child<QPlainTextEdit*>("nativeCwReceiveTranscript"),&QPlainTextEdit::textChanged,&f.model,[&] { if (!nested && !f.history().isEmpty()) { nested=true; f.click("nativeCwReceiveClear"); } }); f.observe("CD2A"); QVERIFY(nested); QVERIFY(f.history().isEmpty()); QObject::disconnect(clear);
    }
    void cleanDraftTracksObservedButExternalCycleRetiresEditedDraft() {
        Fixture f; f.read(); f.observe("CD31"); f.observe("CD32"); QCOMPARE(f.choice("filter")->currentData().toInt(),2); f.stage("threshold",17); f.observe("CD1010"); QVERIFY(f.button("nativeCwReceive_threshold_Apply")->isEnabled()); f.observe("CD1012"); f.observe("CD1010"); QCOMPARE(f.threshold()->value(),17); QVERIFY(!f.button("nativeCwReceive_threshold_Apply")->isEnabled()); f.threshold()->setFocus(); QTest::keyClick(f.threshold(),Qt::Key_Tab); QVERIFY(!f.button("nativeCwReceive_threshold_Apply")->isEnabled()); f.stage("threshold",18); QVERIFY(f.button("nativeCwReceive_threshold_Apply")->isEnabled());
    }
    void newBindingResetsFloorsDraftAndOldHandoffLabelWithoutReplay() {
        Fixture f; f.read(); f.stage("threshold",17); f.click("nativeCwReceive_threshold_Apply"); QVERIFY(!f.child<QLabel*>("nativeCwReceiveStatus")->text().isEmpty()); f.replyReads=false; f.wires.clear(); f.install(); QVERIFY(f.history().isEmpty()); QVERIFY(f.child<QLabel*>("nativeCwReceiveStatus")->text().isEmpty()); for (const QByteArray& wire : f.wires) { QVERIFY(!wire.startsWith("CD10") && wire!="CD01;" && wire!="CD51;"); } f.read(); QCOMPARE(f.threshold()->value(),10);
    }
    void accessibilityReentryDeletionAndFocusedTeardownAreGuarded() {
        Fixture f; f.read(); QAccessible::setActive(true); if (!QAccessible::isActive()) { QSKIP("Accessibility unavailable; no accessibility callback proof."); }
        const auto previous=QAccessible::installUpdateHandler(accessibleUpdate); const auto cleanup=qScopeGuard([previous] { accessibilityCallback={}; QAccessible::installUpdateHandler(previous); }); bool nested=false;
        accessibilityCallback=[&](QAccessibleEvent* event) { if (!nested && event->object()==f.child<QLabel*>("nativeCwReceive_threshold_Observed")) { nested=true; f.observe("CD1012"); } }; f.observe("CD1011"); QVERIFY(nested); QCOMPARE(f.threshold()->value(),12);
        nested=false; accessibilityCallback=[&](QAccessibleEvent* event) { if (!nested && event->object()==f.child<QLabel*>("nativeCwReceive_threshold_Observed")) { nested=true; f.widget.reset(); } }; const QPointer<NativeCwReceiveWidget> alive(f.widget.get()); f.observe("CD1013"); QVERIFY(nested); QVERIFY(!alive); accessibilityCallback={}; f.makeWidget(); f.threshold()->setFocus(); f.threshold()->selectAll(); QTest::keyClicks(f.threshold(),"17"); f.widget.reset(); QCoreApplication::processEvents();
    }
};
int main(int argc,char** argv) { TestSettingsProfile profile("native-cw-receive"); if (!profile.isValid()) { return 1; } QApplication app(argc,argv); NativeCwReceiveWidgetTest test; return QTest::qExec(&test,argc,argv); }
#include "native_cw_receive_widget_test.moc"
