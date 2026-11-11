#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativePskReceiveWidget.h"
#include "models/RadioModel.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QtTest>
#include <memory>
namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void ready(KenwoodBackend& b) { b.m_clock.start();b.m_protocol.start("psk-fixture","fixture-only",false,0);b.m_protocol.receive("##CN1;",1);b.m_protocol.receive("##ID1;",2);b.ready(); }
    static void feed(KenwoodBackend& b,const QByteArray& frame) { b.adopt(frame); }
    static void poll(KenwoodBackend& b,qint64 now) { b.pollPskReceive(now); }
};
}
using namespace AetherSDR;
namespace {
struct Fixture {
    QList<QByteArray> wires;
    QVariantMap state;
    RadioModel model;
    kenwood::KenwoodBackend* backend=nullptr;
    std::unique_ptr<NativePskReceiveWidget> widget;
    qint64 now=0;
    Fixture(bool receive=true) {
        auto owned=std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{[this](const QByteArray& bytes) { wires.append(bytes);return true; },{}});
        backend=owned.get();model.setBackendForTest(std::move(owned),"kenwood");
        QObject::connect(backend,&IRadioBackend::extensionStatus,&model,[this](const QString&,const QString& kind,const QVariantMap& value) { if (kind=="pskReceive") { state=value; } });
        widget=std::make_unique<NativePskReceiveWidget>(model);widget->resize(1100,650);widget->show();
        kenwood::KenwoodBackendTestAccess::ready(*backend);feed("ID024");if (receive) { feed("RX"); }
        emit model.connectionStateChanged(true);wires.clear();
    }
    void feed(const QByteArray& bytes) { kenwood::KenwoodBackendTestAccess::feed(*backend,bytes); }
    void poll() { now+=100;kenwood::KenwoodBackendTestAccess::poll(*backend,now); }
    QPushButton* button(const char* name) { return widget->findChild<QPushButton*>(name); }
    QComboBox* draft() { return widget->findChild<QComboBox*>("pskReceive_afcRangeDraft"); }
    QLabel* observed() { return widget->findChild<QLabel*>("pskReceive_afcRangeObserved"); }
    void settle(int range=0,int bank=0) { poll();feed("MF"+QByteArray::number(bank));poll();feed("EX00211 "+QByteArray::number(range).rightJustified(3,'0'));poll(); }
    void read(int range=0,int bank=0) { QTest::mouseClick(button("pskReceiveRead"),Qt::LeftButton);settle(range,bank); }
    void choose(int value) { const int index=draft()->findData(value);QVERIFY(index>=0);draft()->setCurrentIndex(index);QVERIFY(QMetaObject::invokeMethod(draft(),"activated",Qt::DirectConnection,Q_ARG(int,index))); }
};
}
class NativePskReceiveWidgetTest final:public QObject {
    Q_OBJECT
private slots:
    void cachedOpeningAndExplicitTwoQueryRead() {
        Fixture f(false);QVERIFY(f.wires.isEmpty());QVERIFY(f.button("pskReceiveRead")->isEnabled());
        QVERIFY(!f.button("pskReceive_afcRangeApply")->isEnabled());f.read();
        QCOMPARE(f.wires,(QList<QByteArray>{"MF;","EX00211;"}));
        QVERIFY(!f.observed()->text().contains("Unknown",Qt::CaseInsensitive));
        QVERIFY(!f.button("pskReceive_afcRangeApply")->isEnabled());
    }
    void bothEnumChoicesUseActualApplyAndReadback() {
        Fixture f;f.read();
        for (int value:{1,0}) {
            f.wires.clear();f.choose(value);QVERIFY(f.wires.isEmpty());
            QVERIFY(f.button("pskReceive_afcRangeApply")->isEnabled());
            QTest::mouseClick(f.button("pskReceive_afcRangeApply"),Qt::LeftButton);
            QCOMPARE(f.wires,(QList<QByteArray>{QByteArray("EX00211 ")+QByteArray::number(value).rightJustified(3,'0')+";"}));
            QVERIFY(!f.button("pskReceive_afcRangeApply")->isEnabled());
            f.settle(value);QCOMPARE(f.draft()->currentData().toInt(),value);QVERIFY(f.draft()->isEnabled());
        }
        const QString qa=qEnvironmentVariable("AETHER_PSK_RECEIVE_QA_DIR");
        if (!qa.isEmpty()) { QVERIFY(QDir().mkpath(qa));QApplication::processEvents();QVERIFY(f.widget->grab().save(qa+"/psk-range-observed.png")); }
        f.wires.clear();f.widget.reset();QVERIFY(f.wires.isEmpty());
    }
    void resetDisplaysEitherNativeValueWithoutPredictingDefault() {
        Fixture f;f.read();
        for (int native:{1,0}) {
            f.wires.clear();QVERIFY(f.button("pskReceive_afcRangeReset")->isEnabled());
            const int previous=f.draft()->currentData().toInt();
            QTest::mouseClick(f.button("pskReceive_afcRangeReset"),Qt::LeftButton);
            QCOMPARE(f.wires,(QList<QByteArray>{"EX002119;"}));QCOMPARE(f.draft()->currentData().toInt(),previous);
            f.settle(native);QCOMPARE(f.draft()->currentData().toInt(),native);
            QCOMPARE(f.state.value("lastWrite").toMap().value("state").toString(),QString("sent_unverified"));
        }
        const QString qa=qEnvironmentVariable("AETHER_PSK_RECEIVE_QA_DIR");
        if (!qa.isEmpty()) { QVERIFY(QDir().mkpath(qa));QApplication::processEvents();QVERIFY(f.widget->grab().save(qa+"/psk-range-reset-unverified.png")); }
    }
    void bankChangeRetiresStagedDraftRatherThanRebasingIt() {
        Fixture f;f.read();f.choose(1);f.feed("MF1");
        QVERIFY(!f.button("pskReceive_afcRangeApply")->isEnabled());
        f.read(0,1);f.wires.clear();QTest::mouseClick(f.button("pskReceive_afcRangeApply"),Qt::LeftButton);
        QVERIFY(f.wires.isEmpty());f.choose(1);QVERIFY(f.button("pskReceive_afcRangeApply")->isEnabled());
    }
    void malformedHighRevisionDoesNotPoisonValidRecovery() {
        Fixture f;f.read();const QVariantMap good=f.state;QVariantMap bad=good;
        bad["statusRevision"]="999999";bad["bank"]=QVariantMap{{"known",false},{"revision","0"},{"reason","unknown"}};
        emit f.backend->extensionStatus("kenwood","pskReceive",bad);
        QVERIFY(!f.button("pskReceive_afcRangeApply")->isEnabled());
        QVERIFY(f.observed()->text().contains("Unknown",Qt::CaseInsensitive));
        emit f.backend->extensionStatus("kenwood","pskReceive",good);QVERIFY(f.draft()->isEnabled());
    }
};
int main(int argc,char** argv) {
    TestSettingsProfile profile("native-psk-receive-widget");if (!profile.isValid()) { return 1; }
    QApplication app(argc,argv);AppSettings::instance().load();NativePskReceiveWidgetTest test;return QTest::qExec(&test,argc,argv);
}
#include "native_psk_receive_widget_test.moc"
