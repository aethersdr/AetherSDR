// Actual Qt control events -> production model extension route -> injected CAT.
// No RF, socket, device, real credential storage or audible-EQ claim.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeReceiveEqWidget.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QAccessible>
#include <QScopeGuard>
#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QVariantList>
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
QList<int> curve(int seed = 0)
{
    QList<int> result;
    for (int band = 0; band < 18; ++band) { result.append((seed + band * 7) % 31); }
    return result;
}
QByteArray frame(const QList<int>& raw)
{
    QByteArray result("UR");
    for (int value : raw) { result += QByteArray::number(value).rightJustified(2, '0'); }
    return result;
}
template<class T> T* child(QWidget& widget, const QString& name)
{
    T* result = widget.findChild<T*>(name);
    if (!result) { qFatal("Missing EQ test control: %s", qPrintable(name)); }
    return result;
}
bool activate(QComboBox* combo, int index)
{
    // Index selection is UI preparation; only the actual Qt activation event
    // invokes the production handler, not a backend/private-widget setter.
    combo->setCurrentIndex(index);
    return QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, index));
}
void clickCheck(QCheckBox* box)
{
    box->setFocus(); QTest::keyClick(box, Qt::Key_Space);
}
void editSpin(QSpinBox* spin, int db)
{
    spin->setFocus(); spin->selectAll();
    QTest::keyClicks(spin, QString::number(db)); QTest::keyClick(spin, Qt::Key_Return);
}
struct Fixture {
    QList<QByteArray> writes;
    QVariantMap status;
    QVariantMap inspection;
    std::function<void(const QByteArray&)> onWrite;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(quint64, const QVariant&)> onResult;
    RadioModel model;
    Backend* backend = nullptr;
    std::unique_ptr<NativeReceiveEqWidget> widget;
    explicit Fixture(char mode = '2') { installBackend(mode); makeWidget(); }
    void installBackend(char mode = '2')
    {
        auto owned = std::make_unique<Backend>(Backend::Transport{
            [this](const QByteArray& command) { writes.append(command); if (onWrite) { onWrite(command); } return true; }, {}});
        backend = owned.get(); model.setBackendForTest(std::move(owned), "kenwood");
        QObject::connect(backend, &IRadioBackend::extensionStatus, &model,
            [this](const QString&, const QString& kind, const QVariantMap& value) {
                if (kind == "receiveEq") { status = value; if (onStatus) { onStatus(value); } }
            });
        QObject::connect(backend, &IRadioBackend::extensionResult, &model, [this](quint64 id, const QVariant& result) {
            if (result.toMap().contains("copyToken") && result.toMap().contains("state")) { inspection = result.toMap(); }
            if (onResult) { onResult(id, result); }
        });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FT0"), QByteArray("TS0"),
                QByteArray("SF000014074000") + mode, QByteArray("RX")}) { observe(message); }
        // The injection seam omits setupBackend lifecycle wiring. This enters
        // the real model publication/binding edge, not a copied control relay.
        emit model.connectionStateChanged(true);
    }
    void makeWidget()
    {
        widget = std::make_unique<NativeReceiveEqWidget>(model);
        widget->resize(780, 1450); widget->show(); QCoreApplication::processEvents();
    }
    void observe(const QByteArray& message) { kenwood::KenwoodBackendTestAccess::observe(*backend, message); }
    void load(int effect = 2, bool enabled = false, const QList<int>& raw = curve())
    {
        observe(enabled ? "EQR01" : "EQR00"); observe("EQR1" + QByteArray::number(effect)); observe(frame(raw));
    }
    QPushButton* button(const char* suffix) { return child<QPushButton>(*widget, QStringLiteral("nativeReceiveEq") + suffix); }
    QComboBox* effect() { return child<QComboBox>(*widget, "nativeReceiveEqEffect"); }
    QCheckBox* enabled() { return child<QCheckBox>(*widget, "nativeReceiveEqEnabled"); }
    QSpinBox* draft(int band) { return child<QSpinBox>(*widget, QStringLiteral("nativeReceiveEqDraft%1").arg(band)); }
    QCheckBox* selected(int band) { return child<QCheckBox>(*widget, QStringLiteral("nativeReceiveEqResetBand%1").arg(band)); }
    QString observed(int band) { return child<QLabel>(*widget, QStringLiteral("nativeReceiveEqObserved%1").arg(band))->text(); }
    QString message() { return child<QLabel>(*widget, "nativeReceiveEqStatus")->text(); }
    void click(const char* suffix) { QTest::mouseClick(button(suffix), Qt::LeftButton); }
    void publish(const QVariantMap& state) { emit backend->extensionStatus("kenwood", "receiveEq", state); }
};
}
class NativeReceiveEqWidgetTest : public QObject {
    Q_OBJECT
private slots:
    void unknownEnableIsExplicitlyUnknown()
    {
        Fixture fixture;
        QCOMPARE(fixture.enabled()->checkState(), Qt::PartiallyChecked);
        QVERIFY(!fixture.enabled()->isEnabled()); QCOMPARE(fixture.effect()->currentIndex(), -1);
        fixture.writes.clear(); clickCheck(fixture.enabled()); QVERIFY(fixture.writes.isEmpty());
        fixture.load(2, false); QCOMPARE(fixture.enabled()->checkState(), Qt::Unchecked);
        fixture.observe("EQR01"); QCOMPARE(fixture.enabled()->checkState(), Qt::Checked);
        fixture.backend->disconnectRadio(); emit fixture.model.connectionStateChanged(false);
        QCOMPARE(fixture.enabled()->checkState(), Qt::PartiallyChecked); QVERIFY(!fixture.enabled()->isEnabled());
    }
    void everyModeEffectAndEnable_data()
    {
        QTest::addColumn<char>("mode");
        const char codes[] = "12345679ABCDEF";
        const char* names[] = {"LSB", "USB", "CW", "FM", "AM", "RTTY", "CWR", "RTTYR", "PSK", "PSKR", "DIGL", "DIGU", "DFM", "DAM"};
        for (int index = 0; index < 14; ++index) { QTest::newRow(names[index]) << codes[index]; }
    }
    void everyModeEffectAndEnable()
    {
        QFETCH(char, mode);
        Fixture fixture(mode); fixture.load();
        for (int effect = 0; effect < 9; ++effect) {
            const int previous = (effect + 1) % 9;
            fixture.load(previous, false); fixture.writes.clear();
            QVERIFY(fixture.effect()->isEnabled()); QVERIFY(activate(fixture.effect(), effect));
            QVERIFY(fixture.writes.contains("EQR1" + QByteArray::number(effect) + ';'));
            QVERIFY(fixture.writes.contains("EQR1;")); QCOMPARE(fixture.effect()->currentIndex(), previous);
            fixture.observe("EQR1" + QByteArray::number(effect)); fixture.observe(frame(curve()));
            QCOMPARE(fixture.effect()->currentIndex(), effect);
            for (const bool wanted : {false, true}) {
                fixture.observe(wanted ? "EQR00" : "EQR01"); fixture.writes.clear();
                QVERIFY(fixture.enabled()->isEnabled()); clickCheck(fixture.enabled());
                QVERIFY(fixture.writes.contains(wanted ? "EQR01;" : "EQR00;")); QVERIFY(fixture.writes.contains("EQR0;"));
                QCOMPARE(fixture.enabled()->isChecked(), !wanted);
                fixture.observe(wanted ? "EQR01" : "EQR00"); QCOMPARE(fixture.enabled()->isChecked(), wanted);
            }
        }
    }
    void fullCurveDraftAndPreciseResets()
    {
        Fixture fixture; const QList<int> initial = curve(); fixture.load(2, false, initial);
        QList<int> changed;
        fixture.writes.clear();
        for (int band = 0; band < 18; ++band) {
            const int raw = (initial[band] + band % 4 + 1) % 31; changed.append(raw);
            QVERIFY(fixture.draft(band)->isEnabled()); editSpin(fixture.draft(band), 6 - raw);
            QCOMPARE(fixture.draft(band)->value(), 6 - raw); QCOMPARE(fixture.observed(band), QStringLiteral("%1 dB").arg(6 - initial[band]));
        }
        QVERIFY(fixture.writes.isEmpty()); QVERIFY(fixture.button("Apply")->isEnabled()); fixture.click("Apply");
        QVERIFY(fixture.writes.contains(frame(changed) + ';')); QVERIFY(fixture.writes.contains("UR;"));
        QCOMPARE(fixture.observed(0), QStringLiteral("%1 dB").arg(6 - initial[0]));
        fixture.observe(frame(changed));
        for (int band = 0; band < 18; ++band) { QCOMPARE(fixture.observed(band), QStringLiteral("%1 dB").arg(6 - changed[band])); }
        QVERIFY(!fixture.button("Apply")->isEnabled());
        QList<int> reset = changed;
        for (const int band : {0, 7, 17}) { clickCheck(fixture.selected(band)); reset[band] = 99; }
        fixture.writes.clear(); fixture.click("ResetSelected"); QVERIFY(fixture.writes.contains(frame(reset) + ';'));
        fixture.observe(frame(reset)); // Set-only99 cannot confirm a curve.
        QCOMPARE(fixture.observed(0), QStringLiteral("%1 dB").arg(6 - changed[0]));
        QList<int> returned = changed; returned[0] = 12; returned[7] = 18; returned[17] = 24;
        fixture.observe(frame(returned));
        for (int band = 0; band < 18; ++band) { QCOMPARE(fixture.observed(band), QStringLiteral("%1 dB").arg(6 - returned[band])); }
        fixture.writes.clear(); fixture.click("ResetAll"); QVERIFY(fixture.writes.contains(frame(QList<int>(18, 99)) + ';'));
        const QList<int> actual = curve(9); fixture.observe(frame(actual));
        for (int band = 0; band < 18; ++band) { QCOMPARE(fixture.observed(band), QStringLiteral("%1 dB").arg(6 - actual[band])); }
    }
    void copyDestinationsAndSelfRemainHonest()
    {
        for (int item = 0; item < 4; ++item) {
            const int destination = item == 3 ? 1 : item;
            const int source = item == 3 ? 7 : 2;
            Fixture fixture; const QList<int> raw = curve(4); fixture.load(source, false, raw);
            QComboBox* destinationControl = child<QComboBox>(*fixture.widget, "nativeReceiveEqDestination");
            QVERIFY(activate(destinationControl, destination)); fixture.writes.clear(); fixture.click("Copy");
            QCOMPARE(fixture.writes, (QList<QByteArray>{"EQR2" + QByteArray::number(destination) + ';', "EQR0;", "EQR1;"}));
            QCOMPARE(fixture.effect()->currentIndex(), source); QVERIFY(fixture.message().contains("not yet verified"));
            const QString token = fixture.status.value("copyRecord").toMap().value("token").toString(); QVERIFY(!token.isEmpty());
            if (source == destination + 6) {
                fixture.click("Inspect"); QCOMPARE(fixture.inspection.value("copyToken").toString(), token);
                QCOMPARE(fixture.inspection.value("state").toString(), QStringLiteral("awaiting"));
            }
            fixture.writes.clear(); fixture.click("SelectDestination");
            QVERIFY(fixture.writes.contains("EQR1" + QByteArray::number(destination + 6) + ';'));
            fixture.observe("EQR1" + QByteArray::number(destination + 6)); fixture.observe("EQR00");
            fixture.writes.clear(); fixture.click("Read"); QVERIFY(fixture.writes.contains("UR;"));
            fixture.load(destination + 6, false, raw); fixture.click("Inspect");
            QCOMPARE(fixture.inspection.value("copyToken").toString(), token);
            QCOMPARE(fixture.inspection.value("state").toString(), QStringLiteral("matched"));
            QVERIFY(fixture.message().contains("matched"));
        }
    }
    void inspectionWaitsForFreshUnpendingCurve()
    {
        Fixture fixture; const QList<int> original = curve(2); fixture.load(6, false, original);
        fixture.click("Copy"); // Default destination User1 is the current effect.
        QVERIFY(fixture.button("Inspect")->isEnabled()); fixture.click("Inspect");
        QCOMPARE(fixture.inspection.value("state").toString(), QStringLiteral("awaiting"));
        editSpin(fixture.draft(0), -7); QVERIFY(fixture.button("Apply")->isEnabled()); fixture.click("Apply");
        QVERIFY(!fixture.button("Inspect")->isEnabled());
        fixture.inspection.clear(); fixture.click("Inspect"); QVERIFY(fixture.inspection.isEmpty());
        fixture.observe(frame(original)); QVERIFY(fixture.button("Inspect")->isEnabled()); fixture.click("Inspect");
        QCOMPARE(fixture.inspection.value("state").toString(), QStringLiteral("matched"));
    }
    void invalidSetterResultNeverClaimsSent()
    {
        Fixture fixture; fixture.load(); bool injected = false;
        fixture.onResult = [&](quint64 id, const QVariant& result) {
            if (!injected && result.typeId() == QMetaType::Bool) {
                injected = true; emit fixture.backend->extensionResult(id, false);
            }
        };
        clickCheck(fixture.enabled()); QVERIFY(injected); QVERIFY(fixture.message().contains("invalid"));
        QVERIFY(!fixture.message().contains("Request sent"));
    }
    void replacementRejectsOldRequestResultsAndDraft()
    {
        Fixture fixture; fixture.load(2, true); editSpin(fixture.draft(0), -8);
        QVERIFY(fixture.button("Apply")->isEnabled()); quint64 oldRequest = 0;
        fixture.onResult = [&](quint64 id, const QVariant&) { oldRequest = id; };
        fixture.click("Read"); QVERIFY(oldRequest != 0); fixture.onResult = {};
        const QVariantMap oldStatus = fixture.status;
        const QPointer<Backend> retired(fixture.backend);
        fixture.installBackend('3'); QVERIFY(retired.isNull());
        QVERIFY(!fixture.button("Apply")->isEnabled());
        fixture.load(5, false, curve(20)); QCOMPARE(fixture.effect()->currentIndex(), 5);
        QCOMPARE(fixture.observed(0), QStringLiteral("-14 dB")); QVERIFY(!fixture.button("Apply")->isEnabled());
        emit fixture.backend->extensionResult(oldRequest, oldStatus);
        QCOMPARE(fixture.effect()->currentIndex(), 5); QCOMPARE(fixture.observed(0), QStringLiteral("-14 dB"));
        fixture.writes.clear(); fixture.click("Apply"); QVERIFY(fixture.writes.isEmpty());
    }
    void revisionsSourcesAndMalformedStatuses()
    {
        Fixture fixture; fixture.load(); editSpin(fixture.draft(0), -9); QVERIFY(fixture.button("Apply")->isEnabled());
        fixture.observe(frame(curve(5))); // External observed edit replaces the draft revision.
        QVERIFY(!fixture.button("Apply")->isEnabled()); QCOMPARE(fixture.draft(0)->value(), 1);
        fixture.writes.clear(); fixture.click("Apply"); QVERIFY(fixture.writes.isEmpty());
        editSpin(fixture.draft(0), -10);
        fixture.observe("FR1"); fixture.observe("SF1000140740002");
        QVERIFY(!fixture.button("Apply")->isEnabled()); fixture.writes.clear(); fixture.click("Apply"); QVERIFY(fixture.writes.isEmpty());
        fixture.load();
        QVariantMap invalid = fixture.status; invalid.remove("enabled");
        QVariantMap operations = invalid.value("operations").toMap(); QVariantMap enabled = operations.value("enabled").toMap();
        enabled["editable"] = true; operations["enabled"] = enabled; invalid["operations"] = operations;
        fixture.publish(invalid); QVERIFY(!fixture.enabled()->isEnabled()); QVERIFY(!fixture.effect()->isEnabled());
        fixture.load(); invalid = fixture.status; invalid["curveRaw"] = QVariantList{1, 2}; fixture.publish(invalid);
        QVERIFY(!fixture.button("Apply")->isEnabled()); QVERIFY(!fixture.button("ResetAll")->isEnabled());
        fixture.load(); invalid = fixture.status; invalid["radioStoredCurve"] = false; fixture.publish(invalid);
        QVERIFY(!fixture.enabled()->isEnabled()); QVERIFY(fixture.message().contains("invalid"));
    }
    void reentrantGetAndPendingContextWithdrawal()
    {
        Fixture fixture; fixture.load(); bool attempted = false;
        fixture.onWrite = [&](const QByteArray&) { if (!attempted) { attempted = true; fixture.button("Read")->click(); } };
        fixture.writes.clear(); fixture.click("Read");
        QVERIFY(attempted); QCOMPARE(fixture.writes, (QList<QByteArray>{"EQR0;", "EQR1;", "UR;"})); fixture.onWrite = {};
        editSpin(fixture.draft(0), -8); bool retired = false;
        fixture.onStatus = [&](const QVariantMap& state) {
            if (!retired && state.value("operations").toMap().value("curve").toMap().value("pending").toBool()) {
                retired = true; fixture.observe("FR1"); fixture.observe("SF1000140740002");
            }
        };
        fixture.writes.clear(); fixture.click("Apply"); QVERIFY(retired);
        for (const QByteArray& command : fixture.writes) { QVERIFY(!(command.startsWith("UR") && command.size() > 3)); }
        QCOMPARE(fixture.enabled()->checkState(), Qt::PartiallyChecked);
        QCOMPARE(fixture.effect()->currentIndex(), -1);
        for (int band = 0; band < 18; ++band) {
            QCOMPARE(fixture.observed(band), QStringLiteral("Unknown"));
            QVERIFY(!fixture.draft(band)->isEnabled());
        }
        fixture.onStatus = {}; fixture.load(); QVERIFY(fixture.effect()->isEnabled());
    }
    void olderPushAndNewProducerBinding()
    {
        Fixture fixture; fixture.load(2, true); const QVariantMap old = fixture.status;
        fixture.observe("FR1"); fixture.observe("SF1000140740002"); fixture.publish(old);
        QCOMPARE(fixture.enabled()->checkState(), Qt::PartiallyChecked); QCOMPARE(fixture.effect()->currentIndex(), -1);
        for (int band = 0; band < 18; ++band) { QCOMPARE(fixture.observed(band), QString("Unknown")); }
        fixture.load(5, false, curve(10)); fixture.publish(old);
        QCOMPARE(fixture.effect()->currentIndex(), 5); QCOMPARE(fixture.observed(0), QString("-4 dB"));
        for (int read = 0; read < 20; ++read) { fixture.load(5, false, curve(10)); }
        const quint64 high = fixture.status.value("statusRevision").toString().toULongLong();
        fixture.installBackend(); fixture.load(6, true, curve(12));
        QVERIFY(fixture.status.value("statusRevision").toString().toULongLong() < high);
        QCOMPARE(fixture.effect()->currentIndex(), 6); QCOMPARE(fixture.enabled()->checkState(), Qt::Checked);
        QCOMPARE(fixture.observed(0), QString("-6 dB"));
    }
    void malformedProducerRevisionAndHighPayloadReadRecovery()
    {
        Fixture fixture; fixture.load();
        const QList<QVariant> malformed{QVariant(), QVariant(1), QVariant(true), QVariant(1.0), QVariant(QString()),
            QVariant(QString("-1")), QVariant(QString("+1")), QVariant(QString("01")), QVariant(QString("1 ")),
            QVariant(QString("18446744073709551616")), QVariant(QString(200, QChar('9')))};
        for (const QVariant& revision : malformed) {
            QVariantMap bad = fixture.status; bad["statusRevision"] = revision; fixture.publish(bad);
            QCOMPARE(fixture.enabled()->checkState(), Qt::PartiallyChecked); QVERIFY(fixture.button("Read")->isEnabled());
            fixture.writes.clear(); fixture.click("Read");
            QCOMPARE(fixture.writes, (QList<QByteArray>{"EQR0;", "EQR1;", "UR;"}));
            fixture.load(); QCOMPARE(fixture.observed(0), QString("6 dB"));
        }
        for (const QString& high : QStringList{"1000000", "18446744073709551615"}) {
            QVariantMap bad = fixture.status; bad["statusRevision"] = high; bad["enabled"] = 1; fixture.publish(bad);
            QCOMPARE(fixture.enabled()->checkState(), Qt::PartiallyChecked); fixture.writes.clear(); fixture.click("Read");
            QCOMPARE(fixture.writes, (QList<QByteArray>{"EQR0;", "EQR1;", "UR;"})); fixture.load(5, true, curve(12));
            QVERIFY(fixture.status.value("statusRevision").toString().toULongLong() < high.toULongLong());
            QCOMPARE(fixture.observed(0), QString("-6 dB")); QCOMPARE(fixture.enabled()->checkState(), Qt::Checked);
        }
    }
    void newerPushedCurveWinsOuterGetAndRefreshesAvailability()
    {
        Fixture fixture; fixture.load(); bool nested = false;
        fixture.onResult = [&](quint64, const QVariant& value) {
            if (!nested && value.toMap().contains("curveRaw")) { nested = true; fixture.observe(frame(curve(8))); }
        };
        fixture.click("Read"); QVERIFY(nested); QCOMPARE(fixture.observed(0), QString("-2 dB"));
        QVERIFY(fixture.button("Read")->isEnabled()); QVERIFY(fixture.effect()->isEnabled());
        fixture.onResult = {};
    }
    void accessibilityNestedCurveAndContextCannotResumeStaleRender()
    {
        Fixture fixture; fixture.load(); const bool active = QAccessible::isActive();
        const QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(accessibleUpdate);
        const auto cleanup = qScopeGuard([previous, active] { accessibilityCallback = {}; QAccessible::installUpdateHandler(previous); QAccessible::setActive(active); });
        QAccessible::setActive(true);
        if (!QAccessible::isActive()) { QSKIP("No active accessibility backend; explicit EQ callback reentry not exercised."); }
        bool nested = false;
        accessibilityCallback = [&](QAccessibleEvent* event) {
            if (!nested && event->type() == QAccessible::ValueChanged
                && event->object() == child<QLabel>(*fixture.widget, "nativeReceiveEqObserved0")) {
                nested = true; fixture.observe(frame(curve(11)));
            }
        };
        fixture.observe(frame(curve(7))); QVERIFY(nested);
        for (int band = 0; band < 18; ++band) { QCOMPARE(fixture.observed(band), QString("%1 dB").arg(6 - curve(11)[band])); }
        nested = false;
        accessibilityCallback = [&](QAccessibleEvent* event) {
            if (!nested && event->type() == QAccessible::ValueChanged
                && event->object() == child<QLabel>(*fixture.widget, "nativeReceiveEqObserved0")) {
                nested = true; fixture.observe("FR1"); fixture.observe("SF1000140740002");
            }
        };
        fixture.observe(frame(curve(14))); QVERIFY(nested);
        QCOMPARE(fixture.enabled()->checkState(), Qt::PartiallyChecked); QCOMPARE(fixture.effect()->currentIndex(), -1);
        for (int band = 0; band < 18; ++band) { QCOMPARE(fixture.observed(band), QString("Unknown")); QVERIFY(!fixture.draft(band)->isEnabled()); }
    }
    void lifetimeAndRetiredSessionCallbacks()
    {
        Fixture fixture; fixture.load();
        QPointer<NativeReceiveEqWidget> retired(fixture.widget.get());
        fixture.onWrite = [&](const QByteArray&) { fixture.widget.reset(); };
        fixture.click("Read"); QVERIFY(retired.isNull()); fixture.onWrite = {};
        fixture.observe("EQR01"); fixture.observe(frame(curve(3))); // Disconnected Qt receiver is gone.
        fixture.makeWidget(); fixture.load();
        fixture.widget->close(); fixture.widget.reset();
        fixture.observe("EQR00"); fixture.makeWidget(); fixture.load();
        fixture.backend->disconnectRadio(); emit fixture.model.connectionStateChanged(false);
        fixture.writes.clear(); fixture.observe("EQR01"); fixture.click("Apply");
        QVERIFY(fixture.writes.isEmpty()); QVERIFY(!fixture.enabled()->isEnabled());
    }
};
int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("native-receive-eq-widget"));
    if (!settings.isValid()) { return 1; }
    QApplication application(argc, argv); AppSettings::instance().load();
    NativeReceiveEqWidgetTest test; return QTest::qExec(&test, argc, argv);
}
#include "native_receive_eq_widget_test.moc"
