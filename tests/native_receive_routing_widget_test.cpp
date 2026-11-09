// Real Qt controls -> production model/backend with injected transport only.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeReceiveRoutingWidget.h"
#include "models/RadioModel.h"

#include <QAccessible>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QCoreApplication>
#include <QLabel>
#include <QList>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QStringList>
#include <QVariantMap>
#include <QtTest>
#include <functional>
#include <memory>
#include <utility>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend)
    {
        backend.m_clock.start(); backend.m_protocol.start("fixture-user", "fixture-password", false, 0);
        backend.m_protocol.receive("##CN1;", 1); backend.m_protocol.receive("##ID1;", 2); backend.ready();
    }
    static void observe(KenwoodBackend& backend, const QByteArray& message) { backend.adopt(message); }
};
}
namespace {
using namespace AetherSDR;
using Backend = kenwood::KenwoodBackend;
std::function<void(QAccessibleEvent*)> accessibilityCallback;
void accessibleUpdate(QAccessibleEvent* event) { if (accessibilityCallback) { accessibilityCallback(event); } }
template<class T> T* child(QWidget& widget, const QString& field)
{
    T* result = widget.findChild<T*>(QStringLiteral("nativeReceiveRouting") + field);
    if (!result) { qFatal("Missing routing control: %s", qPrintable(field)); }
    return result;
}
void toggle(QCheckBox* control) { control->setFocus(); QTest::keyClick(control, Qt::Key_Space); }
struct Fixture {
    QList<QByteArray> writes;
    QVariantMap status;
    std::function<void(const QByteArray&)> onWrite;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(quint64, const QVariant&)> onResult;
    RadioModel model;
    Backend* backend = nullptr;
    std::unique_ptr<NativeReceiveRoutingWidget> widget;
    explicit Fixture(char mode = '2') { install(mode); makeWidget(); }
    void install(char mode = '2')
    {
        auto owned = std::make_unique<Backend>(Backend::Transport{
            [this](const QByteArray& wire) { writes.append(wire); if (onWrite) { onWrite(wire); } return true; }, {}});
        backend = owned.get(); model.setBackendForTest(std::move(owned), "kenwood");
        QObject::connect(backend, &IRadioBackend::extensionStatus, &model,
            [this](const QString&, const QString& kind, const QVariantMap& value) {
                if (kind == "receiveRouting") { status = value; if (onStatus) { onStatus(value); } }
            });
        QObject::connect(backend, &IRadioBackend::extensionResult, &model,
            [this](quint64 id, const QVariant& value) { if (onResult) { onResult(id, value); } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FT0"), QByteArray("TS0"),
                QByteArray("SF000014074000") + mode, QByteArray("RX")}) { observe(message); }
        // The injection seam omits setupBackend connection wiring. This is the
        // real public model notification, not a copied command/status relay.
        emit model.connectionStateChanged(true);
    }
    void makeWidget()
    {
        widget = std::make_unique<NativeReceiveRoutingWidget>(model);
        widget->resize(800, 350); widget->show(); QCoreApplication::processEvents();
    }
    void observe(const QByteArray& wire) { kenwood::KenwoodBackendTestAccess::observe(*backend, wire); }
    QCheckBox* control(const QString& field) { return child<QCheckBox>(*widget, field); }
    QPushButton* read() { return child<QPushButton>(*widget, "Read"); }
    void clickRead() { QTest::mouseClick(read(), Qt::LeftButton); }
    void publish(const QVariantMap& state) { emit backend->extensionStatus("kenwood", "receiveRouting", state); }
};
}
class NativeReceiveRoutingWidgetTest : public QObject {
    Q_OBJECT
private slots:
    void unknownIsTristateAndReadIsAvailable()
    {
        Fixture f;
        for (const QString& field : QStringList{"rxAntenna", "externalOutput"}) {
            QCOMPARE(f.control(field)->checkState(), Qt::PartiallyChecked); QVERIFY(!f.control(field)->isEnabled());
            QVERIFY(!f.control(field)->accessibleDescription().isEmpty()); QVERIFY(!f.control(field)->isHidden());
        }
        QVERIFY(f.read()->isEnabled()); QVERIFY(!f.read()->autoDefault());
        f.writes.clear(); f.clickRead(); QCOMPARE(f.writes, QList<QByteArray>({"AN;"}));
    }
    void everyModeBothDirectionsAreActualUiAndRadioConfirmed()
    {
        for (char mode : QByteArray("12345679ABCDEF")) {
            Fixture f(mode); f.observe("AN1010");
            for (const QString& field : QStringList{"rxAntenna", "externalOutput"}) {
                QVERIFY2(f.control(field)->isEnabled(), qPrintable(field));
                QCOMPARE(f.control(field)->checkState(), Qt::Unchecked);
                f.writes.clear(); toggle(f.control(field));
                const QByteArray on = field == "rxAntenna" ? "AN9199;" : "AN9991;";
                QCOMPARE(f.writes, QList<QByteArray>({on, "AN;"}));
                QCOMPARE(f.control(field)->checkState(), Qt::Unchecked); QVERIFY(!f.control(field)->isEnabled());
                f.observe(field == "rxAntenna" ? "AN1110" : "AN1011");
                QCOMPARE(f.control(field)->checkState(), Qt::Checked); QVERIFY(f.control(field)->isEnabled());
                QVERIFY(!f.control(field)->accessibleDescription().contains("not currently active"));
                f.writes.clear(); toggle(f.control(field));
                const QByteArray off = field == "rxAntenna" ? "AN9099;" : "AN9990;";
                QCOMPARE(f.writes, QList<QByteArray>({off, "AN;"}));
                QCOMPARE(f.control(field)->checkState(), Qt::Checked);
                f.observe("AN1010"); QCOMPARE(f.control(field)->checkState(), Qt::Unchecked);
            }
        }
    }
    void frequencyChangeRetiresBothAndRefetches()
    {
        Fixture f; f.observe("AN1111"); f.writes.clear(); f.observe("FA00014075000");
        QCOMPARE(f.writes.count("AN;"), 1);
        for (const QString& field : QStringList{"rxAntenna", "externalOutput"}) {
            QCOMPARE(f.control(field)->checkState(), Qt::PartiallyChecked); QVERIFY(!f.control(field)->isEnabled());
        }
        f.observe("AN1010"); QVERIFY(f.control("rxAntenna")->isEnabled());
        QCOMPARE(f.control("rxAntenna")->checkState(), Qt::Unchecked);
    }
    void malformedStatusReadRecoveryAndNoFalseConfirmation()
    {
        Fixture f; f.observe("AN1010"); const QVariantMap good = f.status;
        const auto check = [&](QVariantMap bad) {
            bad["statusRevision"] = f.status.value("statusRevision"); f.publish(bad); QCOMPARE(f.control("rxAntenna")->checkState(), Qt::PartiallyChecked);
            QVERIFY(!f.control("externalOutput")->isEnabled()); QVERIFY(f.read()->isEnabled());
            f.writes.clear(); f.clickRead(); QCOMPARE(f.writes, QList<QByteArray>({"AN;"}));
            f.observe("AN1010"); QVERIFY(f.control("rxAntenna")->isEnabled());
        };
        QVariantMap bad = good; bad["rxAntenna"] = 1; check(bad);
        bad = good; bad.remove("externalOutput"); check(bad);
        bad = good; bad["context"] = 12; check(bad);
        bad = good; bad["editable"] = QString("true"); check(bad);
        bad = good; QVariantMap operations = bad["operations"].toMap(); QVariantMap operation = operations["rxAntenna"].toMap();
        operation["pending"] = true; operations["rxAntenna"] = operation; bad["operations"] = operations; check(bad);
        f.writes.clear(); toggle(f.control("externalOutput"));
        for (const QByteArray& invalid : {QByteArray("AN1911"), QByteArray("AN1019"), QByteArray("AN101"), QByteArray("AN10111")}) { f.observe(invalid); }
        QCOMPARE(f.control("externalOutput")->checkState(), Qt::Unchecked); QVERIFY(!f.control("externalOutput")->isEnabled());
        f.observe("AN1011"); QCOMPARE(f.control("externalOutput")->checkState(), Qt::Checked);
    }
    void olderPushCannotReinstateRetiredContext()
    {
        Fixture f; f.observe("AN1111"); const QVariantMap old = f.status;
        QVERIFY(old.contains("statusRevision")); f.observe("FA00014075000");
        const QString revision = f.status.value("statusRevision").toString();
        QVERIFY(revision.toULongLong() > old.value("statusRevision").toString().toULongLong());
        f.publish(old);
        QCOMPARE(f.control("rxAntenna")->checkState(), Qt::PartiallyChecked);
        QCOMPARE(f.control("externalOutput")->checkState(), Qt::PartiallyChecked);
        QVERIFY(!f.control("rxAntenna")->isEnabled());
        f.observe("AN1010"); QVERIFY(f.control("rxAntenna")->isEnabled());
        f.publish(old); QCOMPARE(f.control("rxAntenna")->checkState(), Qt::Unchecked);
    }
    void malformedRevisionIsNotAnOrderingAuthority()
    {
        Fixture f; f.observe("AN1010");
        const QList<QVariant> invalid{QVariant(), QVariant(1), QVariant(true), QVariant(1.0), QVariant(QString()),
            QVariant(QString("-1")), QVariant(QString("+1")), QVariant(QString("01")), QVariant(QString("1 ")),
            QVariant(QString("18446744073709551616")), QVariant(QString(200, QChar('9')))};
        for (const QVariant& value : invalid) {
            QVariantMap bad = f.status; bad.insert("statusRevision", value); f.publish(bad);
            QCOMPARE(f.control("rxAntenna")->checkState(), Qt::PartiallyChecked); QVERIFY(f.read()->isEnabled());
            f.writes.clear(); f.clickRead(); QCOMPARE(f.writes, QList<QByteArray>({"AN;"}));
            f.observe("AN1010"); QVERIFY(f.control("rxAntenna")->isEnabled());
        }
    }
    void malformedPayloadCannotPoisonAcceptedRevisionFloor()
    {
        Fixture f; f.observe("AN1010");
        for (const QString& high : QStringList{"1000000", "18446744073709551615"}) {
            QVariantMap malformed = f.status;
            malformed["statusRevision"] = high; malformed["rxAntenna"] = 1;
            f.publish(malformed);
            QCOMPARE(f.control("rxAntenna")->checkState(), Qt::PartiallyChecked);
            QVERIFY(f.read()->isEnabled()); f.writes.clear(); f.clickRead();
            QCOMPARE(f.writes, QList<QByteArray>({"AN;"}));
            // The producer's actual lower revision remains admissible after a
            // malformed high token; the real AN path supplies fresh truth.
            f.observe("AN1111");
            QVERIFY(f.status.value("statusRevision").toString().toULongLong() < high.toULongLong());
            QCOMPARE(f.control("rxAntenna")->checkState(), Qt::Checked);
            QCOMPARE(f.control("externalOutput")->checkState(), Qt::Checked);
            QVERIFY(f.control("rxAntenna")->isEnabled());
        }
    }
    void validPartialFailurePreservesUnaffectedConfirmation()
    {
        Fixture f; f.observe("AN1111"); QVariantMap partial = f.status;
        partial.remove("externalOutput"); QVariantMap operations = partial["operations"].toMap();
        QVariantMap external = operations["externalOutput"].toMap(); external["editable"] = false;
        external["reason"] = QString("External output readback is unavailable after transport failure.");
        operations["externalOutput"] = external; partial["operations"] = operations; f.publish(partial);
        QCOMPARE(f.control("rxAntenna")->checkState(), Qt::Checked); QVERIFY(f.control("rxAntenna")->isEnabled());
        QCOMPARE(f.control("externalOutput")->checkState(), Qt::PartiallyChecked); QVERIFY(!f.control("externalOutput")->isEnabled());
        f.observe("AN1010"); QCOMPARE(f.control("externalOutput")->checkState(), Qt::Unchecked); QVERIFY(f.control("externalOutput")->isEnabled());
    }
    void newerStatusWinsOuterGetAndReplacementRejectsOldResult()
    {
        Fixture f; f.observe("AN1010"); bool nested = false;
        f.onResult = [&](quint64, const QVariant& value) {
            if (!nested && value.toMap().contains("operations")) { nested = true; f.observe("AN1111"); }
        };
        f.clickRead(); QVERIFY(nested); QCOMPARE(f.control("externalOutput")->checkState(), Qt::Checked);
        bool replaced = false; quint64 oldId = 0; QVariant oldValue;
        f.onResult = [&](quint64 id, const QVariant& value) {
            if (!replaced && value.toMap().contains("operations")) {
                replaced = true; oldId = id; oldValue = value; f.install(); f.observe("AN1010");
            }
        };
        f.clickRead(); QVERIFY(replaced); emit f.backend->extensionResult(oldId, oldValue);
        QCOMPARE(f.control("rxAntenna")->checkState(), Qt::Unchecked); QCOMPARE(f.control("externalOutput")->checkState(), Qt::Unchecked);
    }
    void contextWithdrawalDuringPendingPublicationCannotWrite()
    {
        Fixture f; f.observe("AN1010"); bool retired = false;
        f.onStatus = [&](const QVariantMap& value) {
            if (!retired && value.value("operations").toMap().value("rxAntenna").toMap().value("pending").toBool()) {
                retired = true; f.observe("FA00014075000");
            }
        };
        f.writes.clear(); toggle(f.control("rxAntenna")); QVERIFY(retired); QVERIFY(!f.writes.contains("AN9199;"));
        QCOMPARE(f.control("rxAntenna")->checkState(), Qt::PartiallyChecked);
    }
    void accessibilityReentryCannotRestoreRetiredRouting()
    {
        Fixture f; f.observe("AN1010");
        const bool active = QAccessible::isActive(); const QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(accessibleUpdate);
        const auto cleanup = qScopeGuard([previous, active] { accessibilityCallback = {}; QAccessible::installUpdateHandler(previous); QAccessible::setActive(active); });
        QAccessible::setActive(true);
        if (!QAccessible::isActive()) { QSKIP("No active platform accessibility backend; explicit callback reentry not exercised."); }
        bool nested = false;
        accessibilityCallback = [&](QAccessibleEvent* event) {
            if (!nested && event->type() == QAccessible::ValueChanged && event->object() == f.control("rxAntenna")) {
                nested = true; f.observe("AN1010");
            }
        };
        f.observe("AN1111"); QVERIFY(nested);
        QCOMPARE(f.control("rxAntenna")->checkState(), Qt::Unchecked); QCOMPARE(f.control("externalOutput")->checkState(), Qt::Unchecked);
        nested = false;
        accessibilityCallback = [&](QAccessibleEvent* event) {
            if (!nested && event->type() == QAccessible::ValueChanged && event->object() == f.control("rxAntenna")) {
                nested = true; f.backend->disconnectRadio(); emit f.model.connectionStateChanged(false);
            }
        };
        f.observe("AN1111"); QVERIFY(nested);
        QCOMPARE(f.control("externalOutput")->checkState(), Qt::PartiallyChecked); QVERIFY(!f.read()->isEnabled());
    }
    void widgetDeletionAndDisconnectRetireCallbacks()
    {
        Fixture f; f.observe("AN1010"); f.onWrite = [&](const QByteArray& wire) { if (wire == "AN9199;") { f.widget.reset(); } };
        toggle(f.control("rxAntenna")); QVERIFY(!f.widget); f.onWrite = {}; f.observe("AN1110");
        f.makeWidget(); f.observe("AN1010"); f.backend->disconnectRadio(); emit f.model.connectionStateChanged(false);
        f.writes.clear(); f.observe("AN1111");
        QCOMPARE(f.control("rxAntenna")->checkState(), Qt::PartiallyChecked); QVERIFY(!f.control("rxAntenna")->isEnabled());
        QVERIFY(f.writes.isEmpty());
    }
};
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("native-receive-routing-widget")); if (!profile.isValid()) { return 1; }
    QApplication application(argc, argv); AppSettings::instance().load();
    NativeReceiveRoutingWidgetTest test; return QTest::qExec(&test, argc, argv);
}
#include "native_receive_routing_widget_test.moc"
