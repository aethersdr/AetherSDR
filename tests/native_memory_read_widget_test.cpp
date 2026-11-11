// Native memory operations are unavailable on this backend.
// Requests and disabled controls must never start memory I/O.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeMemoryReadWidget.h"
#include "models/RadioModel.h"
#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QLabel>
#include <QList>
#include <QMetaObject>
#include <QPushButton>
#include <QPixmap>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QVariantMap>
#include <QtTest>
#include <memory>
namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) { b.m_clock.start(); b.m_protocol.start("memory-unavailable-fixture","fixture-only",false,0); b.m_protocol.receive("##CN1;",1); b.m_protocol.receive("##ID1;",2); b.ready(); }
    static void observe(KenwoodBackend& b,const QByteArray& bytes) { b.adopt(bytes); }
    static void poll(KenwoodBackend& b,qint64 now) { b.pollMemoryInventory(now); }
};
}
namespace {
using namespace AetherSDR;
using Backend=kenwood::KenwoodBackend;
const QStringList kButtons{"nativeMemoryReadAll","nativeMemoryReadChannel","nativeMemoryReadQuick","nativeMemoryRefreshStatus","nativeMemoryCancel"};
const QStringList kSelectors{"nativeMemoryChannelDraft","nativeQuickMemoryDraft"};
struct Fixture {
    QList<QByteArray> wires;
    int results=0,errors=0,statuses=0;
    RadioModel model;
    Backend* backend=nullptr;
    std::unique_ptr<NativeMemoryReadWidget> widget;
    Fixture() { install(); widget=std::make_unique<NativeMemoryReadWidget>(model); widget->resize(1000,900); widget->show(); QCoreApplication::processEvents(); }
    void install() {
        auto owned=std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& bytes) { wires.append(bytes); return true; },{}}); backend=owned.get(); model.setBackendForTest(std::move(owned),"kenwood");
        QObject::connect(backend,&IRadioBackend::extensionResult,&model,[this](quint64,const QVariant&) { ++results; }); QObject::connect(backend,&IRadioBackend::extensionError,&model,[this](quint64,const QString&) { ++errors; }); QObject::connect(backend,&IRadioBackend::extensionStatus,&model,[this](const QString&,const QString& kind,const QVariantMap&) { if (kind=="memoryRead") { ++statuses; } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend); for (const QByteArray& bytes : {QByteArray("ID024"),QByteArray("FR0"),QByteArray("FT0"),QByteArray("FA00014074000"),QByteArray("OM02"),QByteArray("SF0000140740002"),QByteArray("TS0"),QByteArray("RX")}) { kenwood::KenwoodBackendTestAccess::observe(*backend,bytes); }
        // setBackendForTest omits setupBackend lifecycle wiring. Supply only
        // its public notification, never a copied control/status relay.
        emit model.connectionStateChanged(true);
    }
    QString reason() const { for (const QString& ns : model.backendCapabilities().extensionNamespaces) { const QVariantMap extension=model.backendCapabilities().extensions.value(ns).toMap(); if (extension.contains("nativeMemoryReadUnavailableReason")) { return extension.value("nativeMemoryReadUnavailableReason").toString(); } } return {}; }
    void resetEvidence() { wires.clear(); results=errors=statuses=0; }
    void poll() { for (int index=0;index<150;++index) { kenwood::KenwoodBackendTestAccess::poll(*backend,(index+1)*100); } }
};
}
class NativeMemoryUnavailableWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void actualBackendAdvertisesReasonWithoutMemoryReadCapability() {
        Fixture f; const RadioCapabilities caps=f.model.backendCapabilities(); QVERIFY(!f.reason().isEmpty());
        for (const QString& ns : caps.extensionNamespaces) { const QVariantMap record=caps.extensions.value(ns).toMap(); QVERIFY(!record.contains("nativeMemoryReadVersion")); QVERIFY(!record.contains("nativeMemoryReadVerbs")); if (record.contains("nativeMemoryReadUnavailableReason")) { QCOMPARE(record.value("nativeMemoryReadUnavailableReason").typeId(),QMetaType::QString); } }
        QVERIFY(f.widget->findChild<QLabel*>("nativeMemoryStatus")->text().contains(f.reason()));
        for (const QString& name : kButtons+kSelectors) { QWidget* control=f.widget->findChild<QWidget*>(name); QVERIFY(control); QVERIFY(control->isVisible()); QVERIFY(!control->isEnabled()); QCOMPARE(control->toolTip(),f.reason()); QCOMPARE(control->accessibleDescription(),f.reason()); }
        QCOMPARE(f.results,0); QCOMPARE(f.errors,0);
        const QString qaDirectory=qEnvironmentVariable("AETHER_MEMORY_EXCLUSION_QA_DIR"); if (!qaDirectory.isEmpty()) { QVERIFY(QDir().mkpath(qaDirectory)); QVERIFY(f.widget->grab().save(QDir(qaDirectory).filePath("reason-visible.png"))); }
    }
    void DisabledButtonSignalsAndLocalSelectorsCannotInvokeMemory() {
        Fixture f; f.resetEvidence();
        for (const QString& name : kButtons) { QPushButton* button=f.widget->findChild<QPushButton*>(name); QTest::mouseClick(button,Qt::LeftButton); QVERIFY(QMetaObject::invokeMethod(button,"clicked",Qt::DirectConnection,Q_ARG(bool,false))); }
        f.widget->findChild<QSpinBox*>("nativeMemoryChannelDraft")->setValue(119); f.widget->findChild<QSpinBox*>("nativeQuickMemoryDraft")->setValue(9); f.widget->findChild<QTableWidget*>("nativeMemoryChannels")->selectRow(7); f.poll(); QCoreApplication::processEvents(); QVERIFY(f.wires.isEmpty()); QCOMPARE(f.results,0); QCOMPARE(f.errors,0); QCOMPARE(f.statuses,0);
    }
    void OldMemoryStatusDoesNotReviveWithdrawnFeature() {
        Fixture f; f.resetEvidence(); emit f.backend->extensionStatus("kenwood","memoryRead",{{"statusRevision","18446744073709551615"},{"connected",true},{"context","old-memory-session"}}); emit f.backend->extensionResult(0x4d454d0000000001ULL,QVariantMap{{"connected",true}}); emit f.model.capabilitiesChanged(f.model.isConnected(),f.model.backendCapabilities());
        for (const QString& name : kButtons+kSelectors) { QVERIFY(!f.widget->findChild<QWidget*>(name)->isEnabled()); }
        QVERIFY(f.widget->findChild<QLabel*>("nativeMemoryStatus")->text().contains(f.reason())); QCOMPARE(f.widget->findChild<QTableWidget*>("nativeMemoryChannels")->item(0,1)->text(),QString("Unknown")); QVERIFY(f.wires.isEmpty());
    }
    void ReplacementAndDisconnectKeepReasonWithoutAnyReadReplay() {
        Fixture f; f.install(); f.resetEvidence(); emit f.model.capabilitiesChanged(f.model.isConnected(),f.model.backendCapabilities()); for (const QString& name : kButtons+kSelectors) { QWidget* control=f.widget->findChild<QWidget*>(name); QVERIFY(!control->isEnabled()); QCOMPARE(control->accessibleDescription(),f.reason()); }
        f.model.disconnectFromRadio(); emit f.model.connectionStateChanged(false); f.resetEvidence(); for (const QString& name : kButtons) { QVERIFY(QMetaObject::invokeMethod(f.widget->findChild<QPushButton*>(name),"clicked",Qt::DirectConnection,Q_ARG(bool,false))); } f.poll(); QVERIFY(f.wires.isEmpty()); QCOMPARE(f.errors,0); QCOMPARE(f.results,0);
    }
    void UnrelatedUnsupportedModelUsesDefaultReason() {
        RadioModel model; NativeMemoryReadWidget widget(model); widget.resize(1000,900); widget.show(); QCoreApplication::processEvents();
        for (const QString& name : kButtons+kSelectors) { QWidget* control=widget.findChild<QWidget*>(name); QVERIFY(control); QVERIFY(control->isVisible()); QVERIFY(!control->isEnabled()); QVERIFY(!control->toolTip().isEmpty()); QVERIFY(control->accessibleDescription().contains("connect",Qt::CaseInsensitive) || control->accessibleDescription().contains("supported",Qt::CaseInsensitive)); }
        QVERIFY(!widget.findChild<QLabel*>("nativeMemoryStatus")->text().contains("Native memories unavailable:"));
    }
};
int main(int argc,char** argv) { TestSettingsProfile profile("native-memory-unavailable"); if (!profile.isValid()) { return 1; } QApplication app(argc,argv); NativeMemoryUnavailableWidgetTest test; return QTest::qExec(&test,argc,argv); }
#include "native_memory_read_widget_test.moc"
