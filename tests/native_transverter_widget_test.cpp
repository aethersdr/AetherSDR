// Production widget/model/backend routes using injected CAT transport, no peer.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeTransverterWidget.h"
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
#include <QLineEdit>
#include <QList>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QTableWidget>
#include <QtTest>
#include <functional>
#include <memory>
namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) { b.m_clock.start(); b.m_protocol.start("transverter-fixture","fixture-only",false,0); b.m_protocol.receive("##CN1;",1); b.m_protocol.receive("##ID1;",2); b.ready(); }
    static void observe(KenwoodBackend& b,const QByteArray& message) { b.adopt(message); }
    static void poll(KenwoodBackend& b,qint64 now) { b.pollTransverter(now); }
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
    bool enabled=false,replyReads=false;
    int direction=0,basic=0;
    qint64 magnitude=0,fa=28000000,fb=14074000,now=0;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(quint64,const QVariant&)> onResult;
    RadioModel model;
    Backend* backend=nullptr;
    std::unique_ptr<NativeTransverterWidget> widget;
    Fixture() { install(); makeWidget(); }
    void install() {
        auto owned=std::make_unique<Backend>(Backend::Transport{[this](const QByteArray& bytes) { wires.append(bytes); if (replyReads) { const QByteArray response=reply(bytes); if (!response.isEmpty()) { observe(response); } } return true; },{}}); backend=owned.get(); model.setBackendForTest(std::move(owned),"kenwood");
        QObject::connect(backend,&IRadioBackend::extensionStatus,&model,[this](const QString&,const QString& kind,const QVariantMap& status) { if (kind=="transverter") { state=status; if (onStatus) { onStatus(status); } } });
        QObject::connect(backend,&IRadioBackend::extensionResult,&model,[this](quint64 request,const QVariant& result) { if (onResult) { onResult(request,result); } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend); for (const QByteArray& message : {QByteArray("ID024"),QByteArray("FR0"),QByteArray("FT0"),QByteArray("FA00028000000"),QByteArray("FB00014074000"),QByteArray("OM02"),QByteArray("SF0000280000002"),QByteArray("SF1000140740002"),QByteArray("TS0"),QByteArray("RX")}) { observe(message); }
        // Injected setup omits lifecycle relay; supply only its public signal.
        emit model.connectionStateChanged(true);
    }
    void makeWidget() { widget=std::make_unique<NativeTransverterWidget>(model); widget->resize(1200,950); widget->show(); QCoreApplication::processEvents(); }
    QByteArray reply(const QByteArray& wire) const {
        if (wire=="XV;") { return enabled ? "XV1" : "XV0"; } if (wire=="XO;") { return "XO"+QByteArray::number(direction)+QByteArray::number(magnitude).rightJustified(11,'0'); } if (wire=="DS0;") { return "DS0"+QByteArray::number(basic); }
        if (wire=="FR;") { return "FR0"; } if (wire=="FT;") { return "FT0"; } if (wire=="FA;") { return "FA"+QByteArray::number(fa).rightJustified(11,'0'); } if (wire=="FB;") { return "FB"+QByteArray::number(fb).rightJustified(11,'0'); }
        if (wire=="SF0;") { return "SF0"+QByteArray::number(fa).rightJustified(11,'0')+"2"; } if (wire=="SF1;") { return "SF1"+QByteArray::number(fb).rightJustified(11,'0')+"2"; }
        if (wire=="IF;") { QByteArray frame(37,'0'); frame.replace(0,2,"IF"); frame.replace(2,11,QByteArray::number(fa).rightJustified(11,'0')); frame[29]='2'; return frame; }
        if (wire=="MN;") { return "MN000"; } if (wire=="MV;") { return "MV0"; } if (wire=="QR;") { return "QR00"; } return {};
    }
    void observe(const QByteArray& message) { kenwood::KenwoodBackendTestAccess::observe(*backend,message); }
    void poll(int count=1) { for (int index=0;index<count;++index) { kenwood::KenwoodBackendTestAccess::poll(*backend,now+=100); } }
    template<class T> T child(const QString& name) { return widget->findChild<T>(name); }
    QPushButton* button(const QString& name) { return child<QPushButton*>(name); }
    void click(const QString& name) { QTest::mouseClick(button(name),Qt::LeftButton); }
    void select(QComboBox* combo,const QVariant& value) { const int position=combo->findData(value); QVERIFY(position>=0); combo->setFocus(); QTest::keyClick(combo,Qt::Key_Home); for (int step=0;step<position;++step) { QTest::keyClick(combo,Qt::Key_Down); } QTest::keyClick(combo,Qt::Key_Return); QCOMPARE(combo->currentData(),value); }
    void stageEnabled(bool value) { select(child<QComboBox*>("nativeTransverterEnabledDraft"),value); }
    void stageOffset(int sign,qint64 hz) { select(child<QComboBox*>("nativeTransverterDirectionDraft"),sign); QLineEdit* input=child<QLineEdit*>("nativeTransverterMagnitudeDraft"); input->setFocus(); input->selectAll(); QTest::keyClicks(input,QString::number(hz)); QCOMPARE(input->text().toLongLong(),hz); }
    void read(const QString& scope="all") { select(child<QComboBox*>("nativeTransverterReadScope"),scope); replyReads=true; click("nativeTransverterRead"); poll(scope=="controls" ? 3 : scope=="companions" ? 12 : 15); }
    void publish(const QVariantMap& snapshot) { emit backend->extensionStatus("kenwood","transverter",snapshot); }
    QVariantMap get() { QVariantMap result; QString error; constexpr quint64 request=0x58565141; const auto done=QObject::connect(backend,&IRadioBackend::extensionResult,&model,[&](quint64 id,const QVariant& value) { if (id==request) { result=value.toMap(); } }); const auto failed=QObject::connect(backend,&IRadioBackend::extensionError,&model,[&](quint64 id,const QString& value) { if (id==request) { error=value; } }); model.invokeBackendExtension("kenwood","transverter.get",request); QObject::disconnect(done); QObject::disconnect(failed); if (!error.isEmpty()) { qWarning()<<error; } return result; }
    void exportQa(const QString& tag) { const QString directory=qEnvironmentVariable("AETHER_TRANSVERTER_QA_DIR"); if (directory.isEmpty()) { return; } QVERIFY(QDir().mkpath(directory)); QVERIFY(widget->grab().save(QDir(directory).filePath(tag+".png"))); QJsonArray labels; for (QLabel* label : widget->findChildren<QLabel*>()) { labels.append(QJsonObject{{"name",label->objectName()},{"text",label->text()},{"height",label->height()},{"hintHeight",label->sizeHint().height()}}); } QJsonArray widths; for (int column=0;column<5;++column) { widths.append(child<QTableWidget*>("nativeTransverterFrequencies")->columnWidth(column)); } QFile file(QDir(directory).filePath(tag+"-geometry.json")); QVERIFY(file.open(QIODevice::WriteOnly)); const QByteArray bytes=QJsonDocument(QJsonObject{{"width",widget->width()},{"height",widget->height()},{"labels",labels},{"columnWidths",widths},{"backendStatus",QJsonObject::fromVariantMap(get())}}).toJson(); QCOMPARE(file.write(bytes),qint64(bytes.size())); }
};
}
class NativeTransverterWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void unsupportedControlsAreVisibleDimAndReasoned() {
        RadioModel model; NativeTransverterWidget widget(model); widget.resize(1200,950); widget.show(); QCoreApplication::processEvents();
        for (const QString& name : {QString("nativeTransverterEnabledDraft"),QString("nativeTransverterEnabledApply"),QString("nativeTransverterDirectionDraft"),QString("nativeTransverterMagnitudeDraft"),QString("nativeTransverterOffsetApply"),QString("nativeTransverterPreview"),QString("nativeTransverterReadScope"),QString("nativeTransverterRead"),QString("nativeTransverterCancel")}) { QWidget* control=widget.findChild<QWidget*>(name); QVERIFY(control); QVERIFY(control->isVisible()); QVERIFY(!control->isEnabled()); QVERIFY(control->accessibleDescription().contains("connect",Qt::CaseInsensitive) || control->accessibleDescription().contains("support",Qt::CaseInsensitive)); }
    }
    void cachedGetAndExplicitReadScopesArePassiveOrBoundedQueries() {
        Fixture f; f.wires.clear(); QVERIFY(!f.get().isEmpty()); QVERIFY(f.wires.isEmpty()); f.read();
        QCOMPARE(f.wires,QList<QByteArray>({"FR;","SF0;","FT;","SF0;","XV;","FA;","FB;","SF0;","SF1;","XO;","FA;","FB;","SF0;","SF1;","IF;","DS0;","MN;","MV;","QR;"})); const QVariantMap refresh=f.get().value("refresh").toMap(); QCOMPARE(refresh.value("total").toInt(),15); QCOMPARE(refresh.value("completed").toInt(),15); QCOMPARE(refresh.value("skipped").toInt(),2); QCOMPARE(refresh.value("observed").toInt(),13); f.exportQa("native-transverter-known-read");
        f.wires.clear(); f.read("controls"); QCOMPARE(f.wires,QList<QByteArray>({"XV;","XO;","DS0;"}));
    }
    void deferredWritesRemainVisibleUnavailableAndCannotSendSetters() {
        Fixture f;
        f.read();
        const QList<QString> names{QStringLiteral("nativeTransverterEnabledDraft"),
            QStringLiteral("nativeTransverterEnabledApply"),
            QStringLiteral("nativeTransverterDirectionDraft"),
            QStringLiteral("nativeTransverterMagnitudeDraft"),
            QStringLiteral("nativeTransverterOffsetApply")};
        for (const QString& name : names) {
            QWidget* control = f.child<QWidget*>(name);
            QVERIFY(control);
            QVERIFY(control->isVisible());
            QVERIFY(!control->isEnabled());
            QVERIFY(control->accessibleDescription().contains("deferred", Qt::CaseInsensitive));
        }
        const QString observed = f.child<QLabel*>("nativeTransverterObserved")->text();
        QVERIFY(observed.contains("Off"));
        f.wires.clear();
        // Direct signal invocation also exercises the production slot guards;
        // disabled widgets alone cannot establish a no-command invariant.
        emit f.button("nativeTransverterEnabledApply")->clicked();
        emit f.button("nativeTransverterOffsetApply")->clicked();
        QComboBox* enabled = f.child<QComboBox*>("nativeTransverterEnabledDraft");
        enabled->setCurrentIndex(enabled->findData(true));
        emit enabled->activated(enabled->currentIndex());
        QLineEdit* magnitude = f.child<QLineEdit*>("nativeTransverterMagnitudeDraft");
        magnitude->setText(QStringLiteral("402000000"));
        emit magnitude->textEdited(magnitude->text());
        emit f.button("nativeTransverterEnabledApply")->clicked();
        emit f.button("nativeTransverterOffsetApply")->clicked();
        QVERIFY(f.wires.isEmpty());
        QCOMPARE(f.child<QLabel*>("nativeTransverterObserved")->text(), observed);
        QVERIFY(!f.button("nativeTransverterEnabledApply")->isEnabled());
        QVERIFY(!f.button("nativeTransverterOffsetApply")->isEnabled());
        QVERIFY(f.button("nativeTransverterRead")->isEnabled());
        QVERIFY(f.child<QComboBox*>("nativeTransverterReadScope")->isEnabled());
    }
    void rawOffsetObservationsRetainExactMagnitudeWithoutClaimingRf() {
        Fixture f;
        f.read();
        const QList<QPair<int, qint64>> values{{0, 402000000LL}, {1, 28000000LL},
            {0, 99999999999LL}, {1, 0LL}};
        for (const auto& value : values) {
            f.observe("XO" + QByteArray::number(value.first)
                + QByteArray::number(value.second).rightJustified(11, '0'));
            const QVariantMap state = f.get();
            QVERIFY(state.value("offsetKnown").toBool());
            QCOMPARE(state.value("direction").toInt(), value.first);
            QCOMPARE(state.value("magnitudeHz").typeId(), QMetaType::LongLong);
            QCOMPARE(state.value("magnitudeHz").toLongLong(), value.second);
            QCOMPARE(f.child<QLineEdit*>("nativeTransverterMagnitudeDraft")->text(),
                QString::number(value.second));
            QVERIFY(!f.button("nativeTransverterOffsetApply")->isEnabled());
            const QVariantMap frequency = state.value("frequency").toMap();
            for (const QVariant& raw : frequency.value("vfos").toList()) {
                QVERIFY(!raw.toMap().value("display").toMap().value("known").toBool());
            }
        }
        f.exportQa("native-transverter-read-only-offset");
    }
    void observedOffsetPreviewIsPureAndNeverClaimsActualRfProjection() {
        Fixture f;
        f.magnitude = 402000000;
        f.read();
        f.wires.clear();
        QVERIFY(f.button("nativeTransverterPreview")->isEnabled());
        f.click("nativeTransverterPreview");
        QVERIFY(f.wires.isEmpty());
        const QString preview = f.child<QLabel*>("nativeTransverterPreviewResult")->text();
        QVERIFY(preview.contains("430000000"));
        QVERIFY(!preview.contains("confirmed", Qt::CaseInsensitive));
        QVERIFY(!f.button("nativeTransverterOffsetApply")->isEnabled());
    }
    void malformedStatusDoesNotPoisonRevisionOrBecomeConfirmed() {
        Fixture f;
        f.read();
        for (const QString& revision : {QString("1000000"), QString("18446744073709551615")}) {
            const QVariantMap valid = f.get();
            QVariantMap bad = valid;
            bad["statusRevision"] = revision;
            QVariantMap frequency = bad.value("frequency").toMap();
            QVariantList rows = frequency.value("vfos").toList();
            QVariantMap row = rows[0].toMap();
            row["unexpected"] = true;
            rows[0] = row;
            frequency["vfos"] = rows;
            bad["frequency"] = frequency;
            f.publish(bad);
            QVERIFY(f.child<QLabel*>("nativeTransverterObserved")->text().contains("Unknown"));
            f.publish(valid);
            QVERIFY(f.child<QLabel*>("nativeTransverterObserved")->text().contains("Off"));
        }
        for (const QVariant& invalid : {QVariant(17), QVariant(17.0), QVariant(true), QVariant("17")}) {
            const QVariantMap valid = f.get();
            QVariantMap bad = valid;
            bad["magnitudeHz"] = invalid;
            f.publish(bad);
            QVERIFY(f.child<QLabel*>("nativeTransverterObserved")->text().contains("Unknown"));
            f.publish(valid);
            QCOMPARE(f.child<QLineEdit*>("nativeTransverterMagnitudeDraft")->text(), QString("0"));
            QVERIFY(!f.button("nativeTransverterOffsetApply")->isEnabled());
        }
    }
    void physicalTransverterOnRetiresTheSessionWithoutASetter() {
        Fixture f;
        f.read();
        f.replyReads = false;
        f.wires.clear();
        f.observe("XV1");
        QVERIFY(!f.model.isConnected());
        // As at fixture setup, deliver the public lifecycle notification that
        // setBackendForTest intentionally does not relay.
        emit f.model.connectionStateChanged(false);
        for (const QString& name : {QString("nativeTransverterEnabledApply"),
                QString("nativeTransverterOffsetApply"), QString("nativeTransverterRead")}) {
            QVERIFY(!f.button(name)->isEnabled());
            QVERIFY(!f.button(name)->accessibleDescription().isEmpty());
        }
        for (const QByteArray& wire : f.wires) {
            QVERIFY(wire != "XV0;" && wire != "XV1;"
                && !wire.startsWith("XO0") && !wire.startsWith("XO1"));
        }
    }
    void comboModelReentryAndFocusedDestructionDoNotResumeOldRender() {
        Fixture f;
        f.read();
        bool nested = false;
        const auto connection = QObject::connect(
            f.child<QComboBox*>("nativeTransverterEnabledDraft")->model(),
            &QAbstractItemModel::rowsInserted, &f.model, [&] {
                if (!nested) {
                    nested = true;
                    f.observe("XO000402000000");
                }
            });
        f.observe("XO000401000000");
        QVERIFY(nested);
        QCOMPARE(f.child<QLineEdit*>("nativeTransverterMagnitudeDraft")->text(), QString("402000000"));
        QObject::disconnect(connection);
        f.child<QLineEdit*>("nativeTransverterMagnitudeDraft")->setFocus();
        f.widget.reset();
        QCoreApplication::processEvents();
    }
    void accessibilityNestedStatusAndDeletionAreGuarded() {
        Fixture f;
        f.read();
        QAccessible::setActive(true);
        if (!QAccessible::isActive()) { QSKIP("Accessibility unavailable; no callback proof."); }
        const auto previous = QAccessible::installUpdateHandler(accessibleUpdate);
        const auto cleanup = qScopeGuard([previous] {
            accessibilityCallback = {};
            QAccessible::installUpdateHandler(previous);
        });
        bool nested = false;
        accessibilityCallback = [&](QAccessibleEvent* event) {
            if (!nested && event->object() == f.child<QLabel*>("nativeTransverterObserved")) {
                nested = true;
                f.observe("XO000402000000");
            }
        };
        f.observe("XO000401000000");
        QVERIFY(nested);
        QCOMPARE(f.child<QLineEdit*>("nativeTransverterMagnitudeDraft")->text(), QString("402000000"));
        nested = false;
        accessibilityCallback = [&](QAccessibleEvent* event) {
            if (!nested && event->object() == f.child<QLabel*>("nativeTransverterObserved")) {
                nested = true;
                f.widget.reset();
            }
        };
        const QPointer<NativeTransverterWidget> alive(f.widget.get());
        f.observe("XO000403000000");
        QVERIFY(nested);
        QVERIFY(!alive);
    }
};
int main(int argc,char** argv) { TestSettingsProfile profile("native-transverter"); if (!profile.isValid()) { return 1; } QApplication app(argc,argv); NativeTransverterWidgetTest test; return QTest::qExec(&test,argc,argv); }
#include "native_transverter_widget_test.moc"
